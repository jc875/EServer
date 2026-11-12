#include "RtmpChunk.h"
#include <string.h>

int RtmpChunk::stream_id_ = 0;

RtmpChunk::RtmpChunk()
{
    state_ = PARSE_HEADER;
    chunk_stream_id_ = -1;
    // 每个 RtmpConnection 都有一个 RtmpChunk；构造时从进程级静态计数器
    // 取得一个新的 Message Stream ID（MSID），供 createStream 响应使用。
    // 这不是网络包里的 CSID，也不是直播房间 ID。
    // 简化实现：规范语义是"服务器在 createStream 命令到来时才分配 MSID"，
    // 这里改成"连接构造时预取"，一连接一流场景能工作，但与协议习惯不一致。
    ++stream_id_ ;
}

RtmpChunk::~RtmpChunk()
{
}

// 协议状态机：一次调用解析头或体，由外层 HandleChunk 循环继续推进。
int RtmpChunk::Parse(BufferReader &in_buffer, RtmpMessage &out_rtmp_msg)
{
    int ret = 0;
    if(!in_buffer.ReadableBytes())
    {
        return 0;
    }

    if(state_ == PARSE_HEADER)
    {
        ret = ParseChunkHeader(in_buffer);
    }
    else
    {
        ret = ParseChunkBody(in_buffer);
        if(ret > 0 && chunk_stream_id_ >= 0)  //解析成功
        {
            // auto& 是引用，直接修改 map 中的对象；[] 在键不存在时创建默认对象。
            auto& rtmp_msg = rtmp_message_[chunk_stream_id_];
            if(rtmp_msg.index == rtmp_msg.lenght) //说明message是一个完整的
            {
                if(rtmp_msg.timestamp >= 0xffffff)
                {
                    rtmp_msg._timestamp += rtmp_msg.extend_timestamp;
                }
                else
                {
                    rtmp_msg._timestamp += rtmp_msg.timestamp;
                }
                // 时间戳累计规则：fmt=0 时 timestamp 是绝对时间（index==0 分支已重置
                // _timestamp=0），加上即得本消息时间；fmt=1/2 时是相对上一消息的增量。
                // 隐患：下方 Clear() 会清零本通道 timestamp，若对端下一条用 fmt=1/2
                // （增量），基数丢失、时间戳从 0 重算；主流推流端用 fmt=0 绝对时间戳，
                // 所以当前不触发。

                // 结构体复制会共享消息体的 shared_ptr，不深拷贝全部音视频字节。
                // 随后 Clear 换新缓冲区，不影响输出对象持有的旧消息体。
                out_rtmp_msg = rtmp_msg;
                chunk_stream_id_ = -1;
                rtmp_msg.Clear();
            }
        }
    }
    return ret;
}

int RtmpChunk::CreateChunk(uint32_t csid, RtmpMessage &in_msg, char *buf, uint32_t buf_size)
{
    // buf_offset 含输出头部；playload_offset 只计算已复制的原消息体。
    uint32_t buf_offset = 0, playload_offset = 0;
    // 注意：这是现有估算，未完整计入首块头和所有扩展头，不是可靠容量上界。
    // 调用方还需保证有效的非零块大小、消息体和足够输出空间。
    // 估算构成：lenght + (块数-1)*5 只覆盖"续块头"（1 字节基础头 + 最多 4 字节
    // 扩展时间戳），首块头约 15 字节没算进去，靠 SendRtmpChunks 的 +1024 兜底；
    // 以后收紧发送缓冲必须重算这里。
    uint32_t capacity = in_msg.lenght + in_msg.lenght / out_chunk_size_ * 5;
    if(buf_size < capacity)
    {
        return -1;
    }

    // 首块发完整头（fmt=0，11 字节 Message Header）：
    // 接收方靠它一次性获得消息总长、类型、MSID 和绝对时间，作为后续块的继承基准。
    // 后续块只用 fmt=3（1 字节 Basic Header），完全沿用首块参数，压缩线上字节数。
    buf_offset += CreateBasicHeader(0,csid,buf + buf_offset);
    buf_offset += CreateMessageHeader(0,in_msg,buf + buf_offset);
    if(in_msg._timestamp >= 0xffffff)
    {
        WriteUint32BE((char*)buf + buf_offset,(uint32_t)in_msg.extend_timestamp);
        buf_offset += 4;
    }
    while (in_msg.lenght > 0)
    {
        if(in_msg.lenght > out_chunk_size_)
        {
            memcpy(buf + buf_offset,in_msg.playload.get() + playload_offset,out_chunk_size_);
            playload_offset += out_chunk_size_;
            buf_offset += out_chunk_size_;
            in_msg.lenght -= out_chunk_size_;

            // 还有消息体未发送：为下一块写 fmt=3 基础头，沿用当前消息参数。
            buf_offset += CreateBasicHeader(3,csid,buf + buf_offset);
            if(in_msg._timestamp >= 0xffffff)
            {
                WriteUint32BE(buf + buf_offset,(uint32_t)in_msg.extend_timestamp);
                buf_offset += 4;
            }
        }
        else //最后一个包
        {
            memcpy(buf + buf_offset,in_msg.playload.get() + playload_offset,in_msg.lenght);
            buf_offset += in_msg.lenght;
            in_msg.lenght = 0;
            break;
        }
    }
    
    return buf_offset;
}

int RtmpChunk::ParseChunkHeader(BufferReader &buffer)
{
    uint32_t bytes_used = 0;
    uint8_t* buf = (uint8_t*)buffer.Peek();
    uint32_t buf_size = buffer.ReadableBytes();

    // Parse 已确认至少一个可读字节。Peek 只取地址，不消费数据。
    uint8_t flags = buf[bytes_used];
    uint8_t fmt = (flags >> 6); // 高 2 位：0/1/2/3，决定 Message Header 长度
    if(fmt >= 4) //fmt 0-3
    {
        return -1;
    }
    bytes_used += 1;
    // 低 6 位；0/1 是扩展标记。扩展 CSID 需要至少 10 位。
    // CSID 来自每个 RTMP Chunk 的 Basic Header：
    // 它标识“这块数据走哪条 Chunk 通道”，由客户端发送或由本端发送时指定。
    // CSID 不是 RtmpChunk::stream_id_，也不是 RtmpMessage::stream_id（MSID）。
    uint32_t csid = flags & 0x3f;
    if(csid == 0) //2字节
    {
        if(buf_size < (bytes_used + 1))
        {
            return 0;
        }

        csid += buf[bytes_used] + 64;
        bytes_used += 1;
    }
    else if(csid  == 1) //3字节
    {
        if(buf_size < (bytes_used + 2))
        {
            return 0;
        }
        // 扩展 CSID 按小端排列：64 + 低字节 + 高字节*256。
        csid = buf[bytes_used] + buf[bytes_used + 1] * 256 + 64;
        bytes_used += 2;
    }

    // fmt 决定 Message Header 携带哪些字段（省略字段从本 CSID 通道历史继承）：
    //   fmt=0：11 字节全头（时间/长度/类型/MSID），新消息必用
    //   fmt=1：7 字节（省 MSID，同流换消息）
    //   fmt=2：3 字节（只带增量时间戳，同一消息的多块）
    //   fmt=3：0 字节（全继承，仅 Basic Header）
    uint32_t header_len = KChunkMessageHeaderLenght[fmt];
    if(buf_size < (header_len + bytes_used))
    {
        return 0;
    }

    // 先清零再复制当前 fmt 实际携带的前缀；省略字段应从通道历史继承。
    RtmpMessageHeader header;
    memset(&header,0,sizeof(RtmpMessageHeader));
    memcpy(&header,buf + bytes_used,header_len);
    bytes_used += header_len;

    // 按 CSID 查找当前通道，不能只用 MSID，因为同一消息流可以含多个通道。
    // 每个 CSID 都有独立的历史头和消息拼装进度；Chunk 可以交错到达。
    auto& rtmp_msg = rtmp_message_[csid];
    chunk_stream_id_ = rtmp_msg.csid = csid;

    // fmt=0/1 都带新消息长度和类型；这里分配整条消息的存储空间。
    if(fmt == 0 || fmt == 1)
    {
        uint32_t lenght = ReadUint24BE((char*)header.lenght);
        if(rtmp_msg.lenght != lenght || !rtmp_msg.playload)
        {
            rtmp_msg.lenght = lenght;
            rtmp_msg.playload.reset(new char[rtmp_msg.lenght],std::default_delete<char[]>());
        }
        rtmp_msg.index = 0;
        rtmp_msg.type_id = header.type_id;
    }

    if(fmt == 0)
    {
        // MSID 是头部中的小端例外，时间和长度使用大端。
        rtmp_msg.stream_id = ReadUint32LE((char*)header.stream_id);
    }

    uint32_t timestamp = ReadUint24BE((char*)header.timestamp);
    uint32_t extend_timestamp = 0;
    // 扩展时间是否存在与 fmt 和历史字段相关；当前判断/累加是简化实现。
    // 若下面因输入不足返回，前面已修改部分消息状态，需注意重试的一致性。
    if(timestamp >= 0xffffff || rtmp_msg.timestamp >= 0xffffff)
    {
        if(buf_size < (bytes_used + 4))
        {
            return 0;
        }
        extend_timestamp = ReadUint32BE((char*)buf + bytes_used);
        bytes_used += 4;
    }

    // 只在新消息开始时（index==0）更新时间戳：
    // fmt=0 是绝对时间，直接赋值并重置累计；fmt=1/2 是相对上一消息的增量，累加；
    // fmt=3 不更新（沿用通道内上一次的时间）。
    if(rtmp_msg.index == 0)
    {
        if(fmt == 0)
        {
            rtmp_msg._timestamp = 0;
            rtmp_msg.timestamp = timestamp;
            rtmp_msg.extend_timestamp = extend_timestamp;
        }
        else
        {
            if(rtmp_msg.timestamp >= 0xffffff)
            {
                rtmp_msg.extend_timestamp += extend_timestamp;
            }
            else
            {
                rtmp_msg.timestamp += timestamp;
            }
        }
    }

    // 头已完整消费，下次调用从当前块的数据开始，不再重新解释该头。
    state_ = PARSE_BODY;
    buffer.Retrieve(bytes_used);
    return bytes_used;
}

int RtmpChunk::ParseChunkBody(BufferReader &buffer)
{
    uint32_t bytes_used = 0;
    uint8_t* buf = (uint8_t*)buffer.Peek();
    uint32_t buf_size = buffer.ReadableBytes();

    if(chunk_stream_id_ < 0) //小于0，说明chunk头解析失败
    {
        return -1;
    }

    auto& rtmp_msg = rtmp_message_[chunk_stream_id_];
    // 当前块的数据长度 = min(消息剩余长度, 接收块大小)。
    // 例：总长300、块大小128，依次读取128、128、44。
    uint32_t chunk_size = rtmp_msg.lenght - rtmp_msg.index;
    if(chunk_size > in_chunk_size_)
    {
        chunk_size = in_chunk_size_;
    }

    if(buf_size < (chunk_size + bytes_used))
    {
        return 0;
    }

    if(rtmp_msg.index + chunk_size > rtmp_msg.lenght)
    {
        return -1;
    }

    // get() 取消息体裸指针，+index 定位尚未填充处；复制后再累计进度。
    memcpy(rtmp_msg.playload.get() + rtmp_msg.index,buf + bytes_used,chunk_size);
    bytes_used += chunk_size;
    rtmp_msg.index += chunk_size;

    if(rtmp_msg.index >= rtmp_msg.lenght ||
    rtmp_msg.index % in_chunk_size_ == 0)
    { // 当前块已结束，下一次应解析块头；整条消息是否完成由 Parse 中 index==lenght 判断
        state_ = PARSE_HEADER;
    }
    buffer.Retrieve(bytes_used);
    return bytes_used;
}

int RtmpChunk::CreateBasicHeader(uint8_t fmt, uint32_t csid, char *buf)
{  
    int len= 0;

    if(csid >= 64 + 256) //说明这个basic头占3字节
    {
        buf[len++] = (fmt << 6) | 1;
        buf[len++] = (csid - 64) & 0xff;
        buf[len++] = ((csid - 64) >> 8) & 0xff;
    }
    else if(csid >= 64)//此分支写入2字节的 Basic Header
    {
        buf[len++] = (fmt << 6) | 0;
        buf[len++] = (csid - 64) & 0xff;
    }
    else //1字节
    {
        buf[len++] = (fmt << 6) | csid;
    }
    return len;
}

// 调用方当前只用 fmt=0。若扩展用于 fmt=1/2，应先准备时间增量，不能直接写绝对时间。
int RtmpChunk::CreateMessageHeader(uint8_t fmt, RtmpMessage &rtmp_msg, char *buf)
{
    int len = 0;

    if(fmt <= 2)
    {
        if(rtmp_msg._timestamp < 0xffffff)
        {
            WriteUint24BE((char*)buf,(uint32_t)rtmp_msg._timestamp);
        }
        else{
            WriteUint24BE((char*)buf,0xffffff);
        }
        len += 3;
    }
    if(fmt <= 1)
    {
        WriteUint24BE((char*)buf + len,rtmp_msg.lenght);
        len += 3;
        buf[len++] = rtmp_msg.type_id;
    }
    if(fmt == 0)
    {
        WriteUint32LE((char*)buf + len,rtmp_msg.stream_id);
        len += 4;
    }
    return len;
}
