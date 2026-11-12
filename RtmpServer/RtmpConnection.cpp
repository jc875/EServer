#include "RtmpConnection.h"
#include "rtmp.h"
#include "RtmpServer.h"

RtmpConnection::RtmpConnection(std::shared_ptr<RtmpServer> rtmp_server, TaskScheduler *scheduler, int socket)
    :RtmpConnection(scheduler,socket,rtmp_server.get())
{
    // 对外构造入口：公共初始化由委托构造函数完成，这里补充本连接独有的
    // 握手对象和服务器引用。服务器引用使用 weak_ptr，避免连接和服务器互相持有。
    handshake_.reset(new RtmpHandshake(RtmpHandshake::HANDSHAKE_C0C1));
    rtmp_server_ = rtmp_server;
}

RtmpConnection::RtmpConnection(TaskScheduler* scheduler,int socket,Rtmp* rtmp)
    :TcpConnection(scheduler,socket)
    ,rtmp_chunk_(new RtmpChunk())
    ,state_(HANDSHAKE)
{
    // 内部公共初始化：先构造 TCP 基类，再准备 RTMP 分块解析器。
    // 此时连接还没有完成 RTMP 握手，所以初始状态是 HANDSHAKE。
    peer_width_ = rtmp->GetPeerBandwidth();
    ackonwledgement_size_ = rtmp->GetAcknowledgementSize();
    max_chunk_size_ = rtmp->GetChunkSize();
    stream_path_ = rtmp->GetStreamPath();
    stream_name_ = rtmp->GetStreamName();
    app_ = rtmp->GetApp();

    // TCP 层收到数据后，把同一个 BufferReader 交给 RTMP 状态机处理。
    this->SetReadCallback([this](std::shared_ptr<TcpConnection> conn,BufferReader& buffer){
        return this->OnRead(buffer);
    });

    // TCP 层关闭连接后，移除该连接在 Session 中的发布者/播放器身份。
    this->SetCloseCallback([this](std::shared_ptr<TcpConnection> conn){
        this->OnClose();
    });
}

RtmpConnection::~RtmpConnection()
{
}

bool RtmpConnection::OnRead(BufferReader &buffer)
{
    // 参数 buffer：TCP 层累计的可读字节；一次回调中可能包含半个、
    // 一个或多个 RTMP 数据块。
    // 返回值：true 表示继续保留连接，false 表示协议处理失败、应关闭连接。
    bool ret = true;
    if(handshake_->IsCompleted())
    {
        // 握手完成后，剩余字节才是 RTMP Chunk，交给分块状态机。
        ret = HandleChunk(buffer);
    }
    else
    {
        // 握手阶段可能一次收不完整，因此 Parse 会消费当前已有数据，
        // 并在下次 TCP 读事件到来时继续使用同一个 buffer。
        std::shared_ptr<char> res(new char[4096],std::default_delete<char[]>());
        int res_size = handshake_->Parse(buffer,res.get(),4096);
        if(res_size < 0)
        {
            // 握手格式错误。
            ret = false;
        }
        if(res_size > 0)
        {
            // res 中是握手响应数据，交给 TCP 层发送。
            this->Send(res.get(),res_size);
        }
        if(handshake_->IsCompleted())
        {
            if(buffer.ReadableBytes() > 0)
            {
                // 一次 TCP 读事件可能同时带来握手尾部和 RTMP 数据，
                // 握手完成后立即继续处理剩余字节，避免等待下一次读事件。
                ret = HandleChunk(buffer);
            }
        }
    }
    return ret;
}

void RtmpConnection::OnClose()
{
    // 参数：无。连接关闭时清理 Session 中的 Sink，并通知相关业务事件。
    this->HandleDeleteStream();
}

bool RtmpConnection::HandleChunk(BufferReader &buffer)
{
    // 参数 buffer：TCP 层的可读字节缓冲区。
    // 返回值：true 表示当前字节流没有发现协议错误；false 表示应关闭连接。
    //
    // Parse() 的返回值不是“是否解析出完整 Message”：
    //   >0：本次调用消费了若干字节（可能只消费了块头或块体）；
    //    0：当前数据不足以推进下一步，等待下一次读事件；
    //   <0：协议格式错误。
    //
    // 一条 RTMP Message 可能被拆成多个 Chunk，也可能一次读事件包含多条
    // Message。因此这里循环推进状态机，而不是解析出一条 Message 就退出。
    int ret = -1;
    do
    {
        RtmpMessage rtmp_msg;
        ret = rtmp_chunk_->Parse(buffer,rtmp_msg);
        if(ret >= 0)
        {
            // Parse 只有在一条完整 Message 拼装完成时才会填充 out_rtmp_msg；
            // 中间块阶段虽然 ret > 0，但这里不会处理半条消息。
            if(rtmp_msg.IsCompleted())
            {
                if(!HandleMessage(rtmp_msg))
                {
                    // 业务处理失败也视为当前连接不可继续处理。
                    return false;
                }
            }
            if(ret == 0)
            {
                // 可能是缓冲区为空，也可能是数据不够一个完整的头/体。
                // 两种情况都必须保留解析状态，等待下一次 TCP 数据。
                break;
            }
        }
        else
        {
            // Parse 返回负数表示 RTMP Chunk 格式错误。
            return false;
        }
    } while (buffer.ReadableBytes() > 0);
    // 缓冲区耗尽，或当前块缺少后续字节；两者都不是错误。
    return true;
}

bool RtmpConnection::HandleMessage(RtmpMessage &rtmp_msg)
{
    // 参数 rtmp_msg：RtmpChunk 已经拼装完成的一条 RTMP Message。
    // 返回值：对应业务处理成功返回 true，否则返回 false。
    // type_id 决定消息类别，payload 中保存该类别的具体内容。
    bool ret = true;
    switch (rtmp_msg.type_id)
    {
    case RTMP_VIDEO: //视频数据
        ret = HandleVideo(rtmp_msg);
        break;
    case RTMP_AUDIO:
        ret = HandleAudio(rtmp_msg);
        break;
    case RTMP_INVOKE:
        ret = HandleInvoke(rtmp_msg);
        break;
    case RTMP_NOTIFY:
        ret = HandleNotify(rtmp_msg);
        break;
    case RTMP_SET_CHUNK_SIZE:
        // 对端改变后续发给本连接的 Chunk 大小，只影响接收方向。
        rtmp_chunk_->SetInChunkSize(ReadUint32BE(rtmp_msg.playload.get()));
        break;
    default:
        break;
    }
    return ret;
}

bool RtmpConnection::HandleInvoke(RtmpMessage &rtmp_msg)
{
    // 处理 AMF 命令消息，例如 connect、createStream、publish、play。
    // 参数 rtmp_msg：type_id 为 RTMP_INVOKE 的完整消息。
    // 返回值：命令解析和对应业务处理成功返回 true。
    bool ret = true;
    amf_decoder_.reset(); //解析消息

    // Invoke 的命令格式：["方法名", 事务ID, 参数...]，第一个 AMF 字符串是方法名。
    // decode(...,1) 只解码一个值（方法名），后面再按需解事务号和参数。
    int bytes_used =  amf_decoder_.decode(rtmp_msg.playload.get(),rtmp_msg.lenght,1);
    if(bytes_used < 0)
    {
        printf("bytes_used < 0\n");
        return false;
    }

    std::string method = amf_decoder_.getString();
    // 分发的钥匙是消息头部的 MSID（stream_id），不是方法名：
    //   MSID==0       → 连接级命令：connect 建 NetConnection、createStream 领频道号；
    //   MSID==stream_id_ → 流级命令：publish/play/DeleteStream，作用于已创建的流。
    // 这就是"connect、createStream、publish 是什么关系"的代码答案：一条流级命令
    // 必须携带 createStream 时领到的 MSID，服务器才能确认它属于本连接。
    if(rtmp_msg.stream_id == 0)
    {
        bytes_used += amf_decoder_.decode(rtmp_msg.playload.get() + bytes_used ,rtmp_msg.lenght - bytes_used);
        if(method == "connect")
        {
            ret = HandleConnect();
        }
        else if(method == "createStream")
        {
            ret = HandleCreateStream();
        }
    }
    // 只有属于本连接已创建流的命令才处理 publish/play/delete。
    else if(rtmp_msg.stream_id == stream_id_)
    {
        bytes_used += amf_decoder_.decode(rtmp_msg.playload.get() + bytes_used ,rtmp_msg.lenght - bytes_used,3);
        stream_name_ = amf_decoder_.getString();
        stream_path_ = "/" + app_ + "/" + stream_name_;

        if(rtmp_msg.lenght > bytes_used)
        {
            bytes_used += amf_decoder_.decode(rtmp_msg.playload.get() + bytes_used,rtmp_msg.lenght - bytes_used);
        }

        if(method == "publish")
        {
            ret = HandlePublish();
        }
        else if(method == "play")
        {
            ret = HandlePlay();
        }
        else if(method == "DeleteStream")
        {
            ret = HandleDeleteStream();
        }
    }
    return ret;
}

bool RtmpConnection::HandleNotify(RtmpMessage &rtmp_msg)
{
    // 处理没有返回值的 AMF 通知，当前主要接收发布端的元数据。
    // 返回值 false 表示 AMF 解码失败或服务器对象已失效。
    //准备解码器
    amf_decoder_.reset();

    // 第一个字符串通常是 @setDataFrame。
    int bytes_used = amf_decoder_.decode(rtmp_msg.playload.get(),rtmp_msg.lenght,1);

    std::string method = amf_decoder_.getString();
    if(method == "@setDataFrame")
    {
        amf_decoder_.reset();
        bytes_used = amf_decoder_.decode(rtmp_msg.playload.get() + bytes_used,rtmp_msg.lenght - bytes_used,1);
        if(bytes_used < 0)
        {
            return false;
        }
        // @setDataFrame 后面的字符串应为 onMetaData。
        if(amf_decoder_.getString() == "onMetaData")
        {
            amf_decoder_.decode(rtmp_msg.playload.get() + bytes_used,rtmp_msg.lenght - bytes_used);
            meta_data_ = amf_decoder_.getObjects();
        }

        // 元数据属于当前直播 Session，转发给已经加入该 Session 的 Sink。
        auto server = rtmp_server_.lock();
        if(!server)
        {
            return false;
        }

        auto session = rtmp_session_.lock();
        if(session)
        {
            session->SendMetaData(meta_data_);
        }
    }
    return true;
}

bool RtmpConnection::HandleAudio(RtmpMessage &rtmp_msg)
{
    // 处理一条完整的 RTMP 音频消息，并转发给当前 Session。
    // 参数 rtmp_msg：payload[0] 含音频格式，payload[1] 是 AAC packet type。
    // 返回值：服务器和 Session 均有效且转发成功时返回 true。
    uint8_t type = RTMP_AUDIO;
    uint8_t* playload = (uint8_t*)rtmp_msg.playload.get();
    uint32_t lenght = rtmp_msg.lenght;
    uint8_t sound_format = (playload[0] >> 4) & 0x0f; //音频格式
    uint8_t code_id = playload[0] & 0x0f; //编码id

    auto server = rtmp_server_.lock();
    if(!server)
    {
        return false;
    }

    auto session = rtmp_session_.lock();
    if(!session)
    {
        return false;
    }

    // AAC packet type == 0 表示 AAC sequence header，而不是普通音频帧。
    if(sound_format == RTMP_CODEC_ID_AAC && playload[1] == 0)
    {
        aac_sequence_header_szie_ = rtmp_msg.lenght;
        aac_sequence_header_.reset(new char[rtmp_msg.lenght],std::default_delete<char[]>());
        memcpy(aac_sequence_header_.get(),rtmp_msg.playload.get(),aac_sequence_header_szie_);

        session->SetAacSequenceHeader(aac_sequence_header_,aac_sequence_header_szie_);
        type = RTMP_AAC_SEQUENCE_HEADER;
    }

    // sequence header 和普通音频帧都交给 Session，由 Session 广播给播放器。
    session->SendMediaData(type,rtmp_msg.timestamp,rtmp_msg.playload,rtmp_msg.lenght);
    return true;
}

bool RtmpConnection::HandleVideo(RtmpMessage &rtmp_msg)
{
    // 处理一条完整的 RTMP 视频消息，并转发给当前 Session。
    // payload[0] 的高 4 位是帧类型，低 4 位是编码格式；
    // H.264 payload[1] == 0 表示 AVC sequence header。
    uint8_t type = RTMP_VIDEO;
    uint8_t* playload = (uint8_t*)rtmp_msg.playload.get();
    uint32_t lenght = rtmp_msg.lenght;

    uint8_t frame_type = (playload[0] >> 4) & 0x0f;//获取帧类型
    uint8_t code_id = playload[0] & 0x0f; //视频编码ID

    auto server = rtmp_server_.lock();
    if(!server)
    {
        return false;
    }

    auto session = rtmp_session_.lock();
    if(!session)
    {
        return false;
    }

    if(frame_type == 1 && code_id == RTMP_CODEC_ID_H264 && playload[1] == 0)
    {
        avc_sequence_header_size_ = rtmp_msg.lenght;
        avc_sequence_header_.reset(new char[rtmp_msg.lenght],std::default_delete<char[]>());
        memcpy(avc_sequence_header_.get(),rtmp_msg.playload.get(),avc_sequence_header_size_);

        session->SetAvcSequenceHeader(avc_sequence_header_,avc_sequence_header_size_);
        type = RTMP_AVC_SEQUENCE_HEADER;
    }

    // Session 负责把视频数据发送给所有播放器。
    session->SendMediaData(type,rtmp_msg.timestamp,rtmp_msg.playload,rtmp_msg.lenght);
    return true;
}

bool RtmpConnection::HandleConnect()
{
    // 处理 NetConnection.connect：
    // 1. 读取 app；2. 回复带宽、确认大小和 Chunk 大小；
    // 3. 返回 NetConnection.Connect.Success。
    // 返回值：请求参数有效并成功发送响应时返回 true。
    // 协议位置：握手后的第一步业务会话——客户端"报家门"（我要连哪个 app）。
    // 先发 3 条控制消息（type 5/6/1）谈好传输参数再回 _result，顺序有讲究：
    // 对端必须先知道新的 Chunk Size，才能正确解析后续所有消息。
    if(!amf_decoder_.hasObject("app"))//是否存在app应用程序
    {
        return false;
    }

    AmfObject amfObj = amf_decoder_.getObject("app");
    app_ = amfObj.amf_string; //获取引用程序的值
    if(app_ == "")
    {
        return false;
    }

    SendAcknowlegement();
    SetPeerBandWidth();
    SetChunkSize();

    //应答
    AmfObjects objects;
    amf_encoder_.reset();
    //编码结果
    amf_encoder_.encodeString("_result",7);
    amf_encoder_.encodeNumber(amf_decoder_.getNumber());

    objects["fmsVer"] = AmfObject(std::string("FMS/4,5,0,297"));
    objects["capabilities"] = AmfObject(255.0);
    objects["mode"] = AmfObject(1.0);
    amf_encoder_.encodeObjects(objects);
    //清空对象
    objects.clear();
    //添加参数
    objects["level"] = AmfObject(std::string("status"));
    objects["code"] = AmfObject(std::string("NetConnection.Connect.Success"));
    objects["description"] = AmfObject(std::string("Connection succeeded"));
    objects["objectEncoding"] = AmfObject(0.0);
    amf_encoder_.encodeObjects(objects);
    SendInvokeMessage(RTMP_CHUNK_INVOKE_ID,amf_encoder_.data(),amf_encoder_.size());
    printf("HandleConnect\n");
    return true;
}

bool RtmpConnection::HandleCreateStream()
{
    // 处理 createStream：向客户端分配一个 Message Stream ID。
    // 返回值：当前实现固定返回 true；stream_id_ 保存分配结果。
    // 协议位置：客户端"领频道号"。返回的 MSID 会成为后续 publish/play 等
    // 流级命令消息头部的 stream_id，服务器靠它和连接级命令（MSID=0）区分开。
    int stream_id = rtmp_chunk_->GetStreamId();

    AmfObjects objects;
    //结果
    amf_encoder_.reset();
    amf_encoder_.encodeString("_result",7);
    amf_encoder_.encodeNumber(amf_decoder_.getNumber());
    //需要填一个空对象
    amf_encoder_.encodeObjects(objects);
    amf_encoder_.encodeNumber(stream_id);
    SendInvokeMessage(RTMP_CHUNK_INVOKE_ID,amf_encoder_.data(),amf_encoder_.size());
    stream_id_ = stream_id;
    return true;
}

bool RtmpConnection::HandlePublish()
{
    // 处理 publish：将当前连接登记为某个 stream_path 的发布者。
    // 返回值：成功加入 Session 返回 true；流已被占用或服务器失效返回 false。
    // 协议位置：主播"开播"——把流名绑定到已领的 MSID 上。此后本连接发来的
    // 8/9 号媒体消息才会被转发；广播对象是 Session（房间），不是单个连接。
    auto server = rtmp_server_.lock();
    if(!server)
    {
        return false;
    }

    AmfObjects objects;
    amf_encoder_.reset();
    amf_encoder_.encodeString("onStatus",8);
    amf_encoder_.encodeNumber(0);
    amf_encoder_.encodeObjects(objects);

    bool is_error = false;
    // 同一路径只能有一个发布者。
    if(server->HasPublisher(stream_path_))
    {
        printf("HasPublisher\n");
        is_error = true;
        objects["level"] = AmfObject(std::string("error"));
        objects["code"] = AmfObject(std::string("NetStream.Publish.BadName"));//说明当前这个流已经推送
        objects["description"] = AmfObject(std::string("Stream already publishing."));
    }
    // 同一个连接也不能重复进入发布状态。
    else if(state_ == START_PUBLISH)
    {
        printf("START_PUBLISH\n");
        is_error = true;
        objects["level"] = AmfObject(std::string("error"));
        objects["code"] = AmfObject(std::string("NetStream.Publish.BadConnection"));//说明当前这个流已经推送
        objects["description"] = AmfObject(std::string("Stream already publishing."));
    }
    else
    {
        objects["level"] = AmfObject(std::string("status"));
        objects["code"] = AmfObject(std::string("NetStream.Publish.Start"));//说明当前这个流已经推送
        objects["description"] = AmfObject(std::string("Start publishing."));

        // 创建或获取该流对应的 Session，并通知业务层发布开始。
        server->AddSesion(stream_path_);
        rtmp_session_ = server->GetSession(stream_path_);

        if(server)
        {
            server->NotifyEvent("publish.start",stream_path_);
        }
    }
    amf_encoder_.encodeObjects(objects);
    SendInvokeMessage(RTMP_CHUNK_INVOKE_ID,amf_encoder_.data(),amf_encoder_.size());

    if(is_error)
    {
        return false;
    }else
    {
        state_ = START_PUBLISH;
        is_publishing_ = true;
    }

    auto session = rtmp_session_.lock();
    if(session)
    {
        session->AddSink(std::dynamic_pointer_cast<RtmpSink>(shared_from_this()));
    }
    return true;
}

bool RtmpConnection::HandlePlay()
{
    // 处理 play：回复播放器状态，然后把当前连接加入对应的 Session。
    // 返回值：响应发送成功并完成加入流程时返回 true。
    auto server = rtmp_server_.lock();
    if(!server)
    {
        return false;
    }
    AmfObjects objects;
    amf_encoder_.reset();

    // 先通知播放器重置播放状态。
    amf_encoder_.encodeString("onStatus",8);
    amf_encoder_.encodeNumber(0);
    objects["level"] = AmfObject(std::string("status"));
    objects["code"] = AmfObject(std::string("NetStream.Play.Reset"));
    objects["description"] = AmfObject(std::string("Resetting ond playing stream."));
    amf_encoder_.encodeObjects(objects);
    if(!SendInvokeMessage(RTMP_CHUNK_INVOKE_ID,amf_encoder_.data(),amf_encoder_.size()))
    {
        return false;
    }

    // 再通知播放器正式开始播放。
    objects.clear();
    amf_encoder_.reset();
    amf_encoder_.encodeString("onStatus",8);
    amf_encoder_.encodeNumber(0);
    objects["level"] = AmfObject(std::string("status"));
    objects["code"] = AmfObject(std::string("NetStream.Play.Start"));
    objects["description"] = AmfObject(std::string("Started playing."));
    amf_encoder_.encodeObjects(objects);
    if(!SendInvokeMessage(RTMP_CHUNK_INVOKE_ID,amf_encoder_.data(),amf_encoder_.size()))
    {
        return false;
    }

    // 告知客户端允许读取/写入 RTMP sample data。
    amf_encoder_.reset();
    amf_encoder_.encodeString("|RtmpSampleAccess",17);
    amf_encoder_.encodeBoolean(true); //允许读
    amf_encoder_.encodeBoolean(true); //允许写
    if(!this->SendNotifyMessage(RTMP_CHUNK_DATA_ID,amf_encoder_.data(),amf_encoder_.size()))
    {
        return false;
    }

    // 播放器加入 Session；之后由 Session 调用本对象的 SendMediaData。
    state_ = START_PLAY;
    //TODO 
    //session添加客户端
    rtmp_session_ = server->GetSession(stream_path_);
    auto session = rtmp_session_.lock();
    if(session)
    {
        session->AddSink(std::dynamic_pointer_cast<RtmpSink>(shared_from_this()));
    }

    if(server)
    {
        server->NotifyEvent("Play.start",stream_path_);
    }

    return true;
}

bool RtmpConnection::HandleDeleteStream()
{
    // 处理 DeleteStream 或连接关闭：
    // 从 Session 移除当前 Sink，清理发布/播放状态和 Chunk 拼装缓存。
    // 返回值：服务器对象有效且清理完成返回 true。
    auto server = rtmp_server_.lock();
    if(!server)
    {
        return false;
    }

    if(stream_path_ != "")
    {
        auto session = rtmp_session_.lock();
        if(session)
        {
            auto conn = std::dynamic_pointer_cast<RtmpSink>(shared_from_this());
            GetTaskSchduler()->AddTimer([session,conn](){
                session->RemoveSink(conn);
                return false;
            },1);
            if(is_publishing_)
            {
                server->NotifyEvent("publish,stop",stream_path_);
            }
            else if(is_playing_)
            {
                server->NotifyEvent("play.stop",stream_path_);
            }
        }

    is_playing_ = false;
    is_publishing_ = false;
    has_key_frame = false;
    rtmp_chunk_->Clear();
    }
    return true;
}

void RtmpConnection::SetPeerBandWidth()
{
    // 向客户端发送 Window Acknowledgement Size/Peer Bandwidth 控制消息。
    std::shared_ptr<char> data(new char[5],std::default_delete<char[]>());
    WriteUint32BE(data.get(),peer_width_);
    data.get()[4] = 2; // 0 1 2
    RtmpMessage rtmp_msg;
    rtmp_msg.type_id = RTMP_BANDWIDTH_SIZE;
    rtmp_msg.playload = data;
    rtmp_msg.lenght = 5;
    SendRtmpChunks(RTMP_CHUNK_CONTROL_ID,rtmp_msg);
}

void RtmpConnection::SendAcknowlegement()
{
    // 向客户端发送本端要求的确认窗口大小。
    std::shared_ptr<char> data(new char[4],std::default_delete<char[]>());
    WriteUint32BE(data.get(),ackonwledgement_size_);

    RtmpMessage rtmp_msg;
    rtmp_msg.type_id = RTMP_ACK_SIZE;
    rtmp_msg.playload = data;
    rtmp_msg.lenght = 4;
    SendRtmpChunks(RTMP_CHUNK_CONTROL_ID,rtmp_msg);
}

void RtmpConnection::SetChunkSize()
{
    // 设置本端发送方向的 Chunk 大小，并通知客户端同步该配置。
    rtmp_chunk_->SetOutChunkSize(max_chunk_size_);
    std::shared_ptr<char> data(new char[4],std::default_delete<char[]>());
    WriteUint32BE(data.get(),max_chunk_size_);
    RtmpMessage rtmp_msg;
    rtmp_msg.type_id = RTMP_SET_CHUNK_SIZE;
    rtmp_msg.playload = data;
    rtmp_msg.lenght = 4;
    SendRtmpChunks(RTMP_CHUNK_CONTROL_ID,rtmp_msg);
}

bool RtmpConnection::SendInvokeMessage(uint32_t csid, std::shared_ptr<char> playload, uint32_t playload_size)
{
    // 将 AMF Invoke payload 封装成 RTMP Message，再按 csid 分块发送。
    // 返回值：连接未关闭且已提交发送时返回 true。
    if(this->IsClosed())
    {
        return false;
    }

    RtmpMessage rtmp_msg;
    rtmp_msg.type_id = RTMP_INVOKE;
    rtmp_msg.timestamp = 0;
    rtmp_msg.stream_id = stream_id_;
    rtmp_msg.playload = playload;
    rtmp_msg.lenght = playload_size;
    SendRtmpChunks(csid,rtmp_msg);
    return true;
}

bool RtmpConnection::SendNotifyMessage(uint32_t csid, std::shared_ptr<char> playload, uint32_t playload_size)
{
    // 与 SendInvokeMessage 类似，但消息类型是 RTMP_NOTIFY。
    if(this->IsClosed())
    {
        return false;
    }

    RtmpMessage rtmp_msg;
    rtmp_msg.type_id = RTMP_NOTIFY;
    rtmp_msg.timestamp = 0;
    rtmp_msg.stream_id = stream_id_;
    rtmp_msg.playload = playload;
    rtmp_msg.lenght = playload_size;
    SendRtmpChunks(csid,rtmp_msg);
    return true;
}

bool RtmpConnection::IsKeyFrame(std::shared_ptr<char> data, uint32_t size)
{
    // 判断视频 payload 是否为 H.264 关键帧。
    // size 当前仅用于接口表达，调用方必须保证 data 至少有 1 字节。
    uint8_t frame_type = (data.get()[0] >> 4) & 0x0f;
    uint8_t code_id = data.get()[0] & 0x0f;
    return (frame_type == 1 && code_id == RTMP_CODEC_ID_H264);
}

void RtmpConnection::SendRtmpChunks(uint32_t csid, RtmpMessage &rtmp_msg)
{
    // 参数 csid：RTMP Chunk Stream ID；
    // 参数 rtmp_msg：待发送的完整消息。CreateChunk 会把消息拆成多个 Chunk。
    // 本函数只负责封装后提交给 TcpConnection::Send。
    // 容量估算：lenght + 续块头(每块最多 5 字节) + 1024 兜底首块头与扩展头。
    // 注意 CreateChunk 内部还有一份更紧的估算（不含首块头），靠这里的 +1024 兜底。
    uint32_t capacity = rtmp_msg.lenght + rtmp_msg.lenght / max_chunk_size_ * 5 + 1024; 
    std::shared_ptr<char> buffer(new char[capacity],std::default_delete<char[]>());
    int size = rtmp_chunk_->CreateChunk(csid,rtmp_msg,buffer.get(),capacity);
    if(size > 0)
    {
        this->Send(buffer.get(),size);
    }
}

bool RtmpConnection::SendMetaData(AmfObjects metaData)
{
    // RtmpSink 接口实现：将元数据编码为 onMetaData 通知并发送给播放器。
    // 返回值：连接有效、已有元数据且发送成功时返回 true。
    // ⚠ 已知问题（2026-10-31 核对）：这里检查并编码的是成员变量 meta_data_
    //（本连接作为发布端时自己收到的 onMetaData），而不是参数 metaData（Session
    // 转发来的元数据）。播放器连接从未发过 @setDataFrame，成员恒为空，因此这里
    // 永远 return false——播放器实际收不到 onMetaData。ffplay/ffmpeg 不依赖它
    //（靠序列头解码），依赖元数据的播放器会异常。修法：改用参数 metaData 编码，
    // 或把发布端解析的元数据统一存 Session 再下发给播放器。
    if(this->IsClosed())
    {
        return false;
    }

    if(metaData.size() == 0)
    {
        return false;
    }

    amf_encoder_.reset();
    amf_encoder_.encodeString("onMetaData",10);
    amf_encoder_.encodeECMA(metaData);
    if(!SendNotifyMessage(RTMP_CHUNK_DATA_ID,amf_encoder_.data(),amf_encoder_.size()))
    {
        return false;
    }
    return true;
}

bool RtmpConnection::SendMediaData(uint8_t type, uint64_t timestamp, std::shared_ptr<char> playload, uint32_t playload_size)
{
    // RtmpSink 接口实现：把 Session 转发来的音视频重新封装为 RTMP 消息。
    // type 可以是普通音视频，也可以是 AVC/AAC sequence header。
    // 返回值：连接有效且数据已处理时返回 true；无效输入或连接关闭返回 false。
    if(this->IsClosed())
    {
        return false;
    }

    if(playload_size == 0)
    {
        return false;
    }

    is_playing_ = true;

    if(type == RTMP_AVC_SEQUENCE_HEADER)
    {
        avc_sequence_header_ = playload;
        avc_sequence_header_size_ = playload_size;
    }
    else if(type == RTMP_AAC_SEQUENCE_HEADER)
    {
        aac_sequence_header_ = playload;
        aac_sequence_header_szie_ = playload_size;
    }

    // 播放端必须先拿到 AVC 配置和第一个关键帧，避免从不可解码的
    // P/B 帧开始播放；未满足条件时丢弃普通帧，但不视为错误。
    if(!has_key_frame && avc_sequence_header_size_ > 0
    && (type != RTMP_AVC_SEQUENCE_HEADER)
    && (type != RTMP_AAC_SEQUENCE_HEADER)) //说明数据包既不是序列头，还没有收到关键帧
    {
        if(IsKeyFrame(playload,playload_size))
        {
            has_key_frame = true;
        }
        else
        {
            return true;
        }
    }

    // 满足播放起始条件后，构造消息并按音频/视频选择对应的 CSID。
    RtmpMessage rtmp_msg;
    rtmp_msg._timestamp = timestamp;
    rtmp_msg.stream_id = stream_id_;
    rtmp_msg.playload = playload;
    rtmp_msg.lenght = playload_size;

    //还有msg类型
    if(type == RTMP_VIDEO ||type ==RTMP_AVC_SEQUENCE_HEADER)
    {
        rtmp_msg.type_id = RTMP_VIDEO;
        SendRtmpChunks(RTMP_CHUNK_VIDEO_ID,rtmp_msg);
    }
    else if(type == RTMP_AUDIO ||type ==RTMP_AAC_SEQUENCE_HEADER)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           
    {
        rtmp_msg.type_id = RTMP_AUDIO;
        SendRtmpChunks(RTMP_CHUNK_AUDIO_ID,rtmp_msg);
    }
    return true;

}
