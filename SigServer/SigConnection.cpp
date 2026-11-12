#include "SigConnection.h"
#include "ConnectionManager.h"
#include <algorithm>

// 构造函数：绑定框架的读/关两个回调，初始化状态机为 NONE（尚未注册房间号）
SigConnection::SigConnection(TaskScheduler *scheduler, int socket)
    :TcpConnection(scheduler,socket)
    ,state_(NONE)
{
        //设置回调函数，处理读数据
    this->SetReadCallback([this](std::shared_ptr<TcpConnection> conn,BufferReader& buffer){
        return this->OnRead(buffer);   // 框架收到 socket 可读 → 进入本类解析
    });

    //设置关闭回调，释放资源
    this->SetCloseCallback([this](std::shared_ptr<TcpConnection> conn){
        this->DisConnected();          // 对端断开 → 清理关联关系
    });
}

SigConnection::~SigConnection()
{
    Clear();
}

// 连接断开：清理现场（通知关联者、注销房间号）
void SigConnection::DisConnected()
{
    printf("disConnect");
    Clear();
}

// 记录一个关联方：把对方房间号加入自己的 objectes_（去重）
// 双向调用：A 想跟 B 建立关系时，A.AddCustom(B)、B.AddCustom(A) 各记一笔
void SigConnection::AddCustom(const std::string &code)
{
    //添加客户,这个客户可能是拉流器可能是推流器
    for(const auto idefy : objectes_)
    {
        if(idefy == code)//目标客户端已经添加
        {
            return;
        }
    }
    objectes_.push_back(code);
}

// 移除一个关联方：从 objectes_ 里删掉指定房间号
void SigConnection::RmoveCustom(const std::string &code)
{
    if(objectes_.empty())
    {
        return;
    }
    objectes_.erase(std::remove(objectes_.begin(),objectes_.end(),code),objectes_.end());
    if(objectes_.empty())
    {
        state_ = IDLE; //说明当前没有控制端，自己也不是控制端
    }
}

// 框架读回调：只要缓冲区还有未消费数据就继续解析（一个包一个包处理）
bool SigConnection::OnRead(BufferReader &buffer)
{
    while(buffer.ReadableBytes() > 0)
    {
        HandleMessage(buffer);
    }
    return true;
}

// 报文解析入口：先凑齐一个完整包（4B 头 + body），再按 cmd 分派
void SigConnection::HandleMessage(BufferReader &buffer)
{
    if(buffer.ReadableBytes() < sizeof(packet_head))
    {
        //数据不完整
        return ;   // 头都不够，等下一批数据（框架会在数据续到后再调 OnRead）
    }
    packet_head* data = (packet_head*)buffer.Peek();   // 直接按 packet_head 视图读（1 字节对齐）
    if(buffer.ReadableBytes() < data->len)//数据还是不完整
    {
        return;    // 头有了但 body 没到齐，继续等
    }
    // 包齐了：按 cmd 分派给对应处理器
    switch (data->cmd)
    {
    case JOIN:          // 注册房间号
        HandleJion(data);
        break;
    case OBTAINSTREAM:  // 控制端约流
        HandleObtainStream(data);
        break;
    case CREATESTREAM:  // 被控端上报流地址
        HandleCreateStream(data);
        break;
    case DELETESTREAM:  // 删除流
        HandleDeleteStream(data);
        break;    
    case MOUSE:         // 鼠标/键盘/滚轮 → 转发给被控端
    case MOUSEMOVE:
    case KEY:
    case WHEEL:
        HandleOtherMessage(data);
        break;
    default:
        break;          // 未知命令忽略
    }
    //更新缓冲区：这个包处理完，消费掉 data->len 字节，留给下一个包
    buffer.Retrieve(data->len);
}

// 清理（析构/断开/删流时调用）：广播删除流通知 → 注销自己 → 清空关联
void SigConnection::Clear()
{
    state_ = CLOSE;                     // 先置为关闭态
    conn_ = nullptr;                    // 松开对被控端的引用
    DeleteStream_body body; //需要通知所有关心者这个删除流通知
    for(auto iter : objectes_)          // 遍历所有关联方（关心我的/我关心的）
    {
        TcpConnection::Ptr con = ConnectionManager::GetInstance()->QueryConn(iter);  // 按房间号找到对方连接
        if(con)                          // 对方还在线
        {
            auto ctrCon = std::dynamic_pointer_cast<SigConnection>(con);   // 转回业务子类
            if(ctrCon)
            {
                ctrCon->RmoveCustom(code_);   // 让对方把"我"从它的关联列表里删掉（双向解绑）
                body.SetStreamCount(static_cast<int>(ctrCon->objectes_.size()));
                con->Send((const char*)&body,body.len); // 通知对方更新流关系
            }
        }
    }
    objectes_.clear();                  // 清空自己的关联列表
    ConnectionManager::GetInstance()->RmvConn(code_);  // 从全局注册表注销自己（房间号释放）
    printf("con size: %d\n",ConnectionManager::GetInstance()->Size());
}

// JOIN 处理：客户端上报房间号，登记身份（只能 JOIN 一次，重复报错）
void SigConnection::HandleJion(const packet_head *data)
{
    //准备一个应答
    JoinReply_body reply_body;
    Join_body* body = (Join_body*)data;            // 头部转成 JOIN 报文视图
    if(this->IsNoJion())                            // 自己还没注册过（NONE 态）
    {
        std::string code = body->GetId();           // 取出客户端上报的房间号
        TcpConnection::Ptr ptr = ConnectionManager::GetInstance()->QueryConn(code);//是不是已经存在
        // 【代码异味】TcpConnection:Ptr 少一个冒号。神奇的是它"能编译"：
        // C++ 会把 TcpConnection: 解析成 goto 标签(label)，后面 Ptr ptr = ... 是
        // 独立语句（Ptr 是基类暴露的 shared_ptr<TcpConnection> 别名），所以功能
        // 恰好等价于 TcpConnection::Ptr。但这是笔误，应写成 TcpConnection::Ptr。
        if(ptr)                                     // 房间号已被别人占
        {
            reply_body.SetCode(ERROR);
            this->Send((const char*)&reply_body,reply_body.len);
            return;
        }
        code_ = code;                               // 记下自己的房间号
        state_ = IDLE;                              // 进入空闲态（可被约流）
        ConnectionManager::GetInstance()->AddConn(code,shared_from_this());  // 登记到总台
        printf("Jion count: %d\n",ConnectionManager::GetInstance()->Size());
        reply_body.SetCode(SUCCESSFUL);
        this->Send((const char*)&reply_body,reply_body.len);
        return;
    }
    //已经创建：重复 JOIN 直接报错
    reply_body.SetCode(ERROR);
    this->Send((const char*)&reply_body,reply_body.len);
}

// OBTAINSTREAM 分发：转给核心实现 DoObtainStream
void SigConnection::HandleObtainStream(const packet_head *data)
{
    //获取流
    ObtainStream_body* body = (ObtainStream_body*)data;
    return this->DoObtainStream(body);
}

// CREATESTREAM 分发：转给核心实现 DoCreateStream
void SigConnection::HandleCreateStream(const packet_head *data)
{
    CreateStream_body* body = (CreateStream_body*)data;
    return this->DoCreateStream(body);
}

// DELETESTREAM 处理：只删除当前流关系，不注销客户端房间号。
void SigConnection::HandleDeleteStream(const packet_head *data)
{
    const auto* body = reinterpret_cast<const DeleteStream_body*>(data);

    if(state_ == PULLER)
    {
        if(conn_)
        {
            auto target = std::dynamic_pointer_cast<SigConnection>(conn_);
            if(target)
            {
                RmoveCustom(target->GetCode());
            }
            conn_ = nullptr;
        }
        state_ = IDLE;
        return;
    }

    if(state_ == PUSHER && body->streamCount <= 0)
    {
        streamAddres_.clear();
        state_ = IDLE;
    }
}

// 鼠标/键盘/滚轮消息：仅当自己是 PULLER(控制端) 且有被控端连接时，
// 原样转发给被控端（服务器只当中转站，不改内容）
void SigConnection::HandleOtherMessage(const packet_head *data)
{
    //鼠标，键盘消息 ,转发
    if(conn_ && state_ == PULLER) //说明当前是在拉流，
    {
    //  if(data->cmd == MOUSEMOVE)
    //  {
    //         MouseMove_body* mouse = (MouseMove_body*)data;
    //         printf("mouse xl: %d xr: %d yl: %d yr: %d\n",mouse->xl_ratio,mouse->xr_ratio,mouse->yl_ratio,mouse->yr_ratio);
    //  }
        conn_->Send((const char*)data,data->len);   // 直接转发给被控端（目标房间）
    }
}

// ============================================================
// 约流核心逻辑：控制端 A 想拉房间 code 的流，按目标的 5 种状态分支处理
// 目标 = QueryConn(code) 查到的连接
// ============================================================
void SigConnection::DoObtainStream(const packet_head *data)
{
    ObtainStreamReply_body reply;       // 回给"我"(控制端) 的应答
    CreateStream_body create_reply;     // 发给被控端"去推流"的通知（空包）
    std::string code = ((ObtainStream_body*)data)->GetId();  // 目标房间号
    TcpConnection::Ptr conn = ConnectionManager::GetInstance()->QueryConn(code);  // 查目标连接
    if(!conn)                            // 目标不存在
    {
        printf("远程目标不存在\n");
        reply.SetCode(ERROR);
        this->Send((const char*)&reply,reply.len);
        return;
    }
    if(conn == shared_from_this()) //不能控制自己
    {
        printf("不能控制自己\n");
        reply.SetCode(ERROR);
        this->Send((const char*)&reply,reply.len);
        return; 
    }
    if(this->IsIdle())//本身是空闲就去获取流（自己空闲才能去当控制端）
    {
        auto con = std::dynamic_pointer_cast<SigConnection>(conn);  // 目标转业务子类
        switch (con->GetRoleState())     // 按目标当前状态分支
        {
        case IDLE://目标是空闲，我们就去通知他去推流
            printf("目标空闲\n");
            this->state_ = PULLER;       // 我变成控制端(拉流)
            this->AddCustom(code);       // 我的关联列表记下目标
            con->AddCustom(code_);       // 目标也记下我（双向绑定）
            reply.SetCode(SUCCESSFUL);
            conn_ = conn; //我们就可以通过这个目标(被控端)连接器来转发消息
            //通知被控端来创建流
            con->Send((const char*)&create_reply,create_reply.len);  // 空包：目标收到后去推流
            this->Send((const char*)&reply,reply.len);               // 告诉我：约流成功
            break;
        case NONE:
            printf("目标没上线\n");
            reply.SetCode(ERROR);
            this->Send((const char*)&reply,reply.len);
            break;
        case CLOSE:
            printf("目标离线\n");
            reply.SetCode(ERROR);
            this->Send((const char*)&reply,reply.len);
            break;
        case PULLER: //目标拉流(控制端) 控制端不能控制控制端
            printf("目标忙碌\n");
            reply.SetCode(ERROR);
            this->Send((const char*)&reply,reply.len);
            break;
        case PUSHER: //推流说明他是被控端，所以我们可以去拉流
            if(con->GetStreamAddres().empty()) //异常
            {
                printf("目标正在推流，但是流地址异常\n");
                reply.SetCode(ERROR);
                this->Send((const char*)&reply,reply.len);
            }
            else//在推流，而且流地址正常
            {
                printf("目标正在推流\n");
                this->state_ = PULLER;   // 我变成控制端
                this->AddCustom(code);   // 双向绑定
                con->AddCustom(code_);
                //在推流 流已经存在，就不需要重新创建流，我们只需要播放流；
                PlayStream_body play_body;   // 直接把目标的流地址下发给我去拉
                play_body.SetCode(SUCCESSFUL);
                play_body.SetstreamAddres(con->GetStreamAddres());
                this->Send((const char*)&play_body,play_body.len);
            }
            break;
        default:
            break;
        }
    }
    else //本身是忙碌：我正忙着拉/推，不能再约别人的流
    {
        //返回错误
        reply.SetCode(ERROR);
        this->Send((const char*)&reply,reply.len);
    }
}

// ============================================================
// 被控端收到服务器"去推流"通知后开始推流，把流地址上报上来（CreateStreamReply_body），
// 服务器把流地址广播给所有关联的控制端(PULLER)，并把自己置为 PUSHER
// ============================================================
void SigConnection::DoCreateStream(const packet_head *data)
{
    PlayStream_body body;                       // 广播用的播放流报文
    CreateStreamReply_body* reply = (CreateStreamReply_body*)data;   // 被控端上报的流地址
    printf("body size: %d,streamaddr: %s\n",reply->len,reply->GetstreamAddres().c_str());
    streamAddres_ = reply->GetstreamAddres();   // 记住自己的流地址（供后续被约时直接下发）
    //判断所有连接的状态 ,如果连接器状态十空闲，我们就去回应
    for(auto idefy : objectes_)                 // 遍历所有关联方（这里应都是等我给地址的控制端）
    {
        TcpConnection::Ptr conn = ConnectionManager::GetInstance()->QueryConn(idefy);
        if(!conn)                                // 关联方已掉线，清理掉
        {
            this->RmoveCustom(idefy);
            continue;
        }
        auto con = std::dynamic_pointer_cast<SigConnection>(conn);
        if(streamAddres_.empty())//流地址异常
        {
            printf("流地址异常\n");
            con->state_ = IDLE;                 // 把对方退回空闲（同类可访问私有成员）
            body.SetCode(ERROR);
            con->Send((const char*)&body,body.len);
            continue;
        }
        switch (con->GetRoleState())            // 按关联方状态分支
        {
        case NONE:
        case IDLE:
        case CLOSE:
        case PUSHER:
            body.SetCode(ERROR);                // 对方状态不对（还没约/重复推流），发错误并解绑
            this->RmoveCustom(con->GetCode());
            con->Send((const char*)&body,body.len);
            break;
        case PULLER:                            // 对方是控制端：把流地址发过去，让它去拉
        this->state_ = PUSHER;                  // 我正式进入推流态
        body.SetCode(SUCCESSFUL);
        body.SetstreamAddres(streamAddres_);
        printf("streamadder: %s\n",streamAddres_.c_str());
        conn->Send((const char*)&body,body.len);
        break;
        default:
            break;
        }
    }
}
