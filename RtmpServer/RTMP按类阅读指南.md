# RTMP 按类阅读指南：从继承、回调到一场直播

这份指南专门接在“已经理解握手和 Chunk”的位置。重点不再重复每个协议位，而是解释：**派生类里没写的函数在哪儿、谁调用回调、main 为什么没写 accept、握手之后双方还说什么。**

代码块为当前工作区源码节选；解释写在代码外。代码里的原注释也可能不准确，下文会指出。这里只讲当前项目的主要路径，不代表它已实现完整、无缺陷的 RTMP 服务。

## 0. 阅读路线：一次只解决一个问题

| 顺序 | 读谁 | 必须能回答的问题 |
|---|---|---|
| 第1站 | C++ 关系速查 | 继承沿用、重写、重载、回调到底有什么区别？ |
| 第2站 | main + RtmpServer/TcpServer | Start 在哪，Acceptor 由谁创建？ |
| 第3站 | Acceptor + TcpSocket | 监听 fd 和客户端 fd 是怎么来的？ |
| 第4站 | EventLoop + Scheduler + Channel | 哪个线程发现可读，又是谁调用函数？ |
| 第5站 | RtmpConnection + TcpConnection | 不重写 HandleRead，也能进入 OnRead 吗？ |
| 第6站 | RtmpMessage + rtmp.h + AMF | 哪些是本地对象，哪些是线上数据？ |
| 第7站 | RtmpConnection 的命令函数 | 握手后依次交换什么，改变什么状态？ |
| 第8站 | RtmpSession + RtmpSink | 主播和观众怎么关联，媒体怎么转发？ |
| 第9站 | BufferWriter + 关闭路径 | 数据怎么续发，连接怎么清理？ |

先按上表读，不要一开始就把每个类所有成员逐个背下来。每章最后有一个“读完应能回答”，答不上来再回看那一章。

## 1. 先分清四件事：沿用、重写、重载、回调

### 1.1 继承沿用：子类没写，不代表对象没有

```cpp
// 教学示例，不是项目原码
class Base {
public:
    void Send() { /* 网络发送 */ }
};
class Derived : public Base { };
Derived obj;
obj.Send(); // 直接使用 Base::Send
```

派生对象包含基类子对象。你在 RtmpConnection.h 看不到 Send 的声明，也能通过它调用继承自 TcpConnection 的 public Send。访问权限仍然有效：基类 private 数据存在，但派生类不能直接访问它。

### 1.2 重写 override：替换基类虚函数的实现

基类把函数声明为 virtual，派生类用匹配的函数签名提供实现，就是重写。派生类不重复写 virtual、甚至没写 override，也可能已经重写；override 的价值是让编译器替你检查签名。

本项目 TcpServer::OnConnect 默认创建 TcpConnection，RtmpServer::OnConnect 创建 RtmpConnection。基类代码在对象正常构造完成后调用 this->OnConnect，会根据实际对象类型选择派生实现。

### 1.3 重载 overload：同名，但参数不同

TcpConnection 的两个 Send 是重载：一个接 shared_ptr<char>，一个接 const char*。编译器根据调用参数选用，不是靠运行时对象类型替换。

若派生类新写同名非虚函数，通常涉及名称隐藏，不能叫虚函数重写。名字相似也不构成重写：OnRead 和 HandleRead 根本不是同名函数。

### 1.4 回调：先存一段动作，等事情发生再执行

```cpp
// 教学示例
std::function<void()> callback;
callback = [this]() { this->OnRead(); }; // 注册：此时不执行 OnRead
callback();                            // 触发：这时才执行 OnRead
```

回调函数本身不要求是 virtual。它可以指向普通函数、lambda 或成员函数包装。也不会因为注册了回调就自动创建线程：谁执行 callback()，通常就在哪个线程运行。

### 1.5 本项目函数去向表

| 你看到的调用 | 定义在哪里 | 实际机制 |
|---|---|---|
| rtmp_server->Start(ip,port) | TcpServer | 虚函数，但 RtmpServer 没重写，沿用基类 |
| Start 内的 Stop() | TcpServer | Stop 也是虚函数；当前派生类没重写，执行基类版本 |
| this->OnConnect(fd) | 基类声明，RtmpServer 提供实现 | 虚函数重写，创建派生连接 |
| AddConnection/RemoveConnection | TcpServer | 当前派生类没重写，沿用基类连接表管理 |
| conn->Send(...) | TcpConnection | 沿用基类；两个参数形式构成重载 |
| Channel 调用 HandleRead() | TcpConnection | 虚函数，RtmpConnection 没重写 |
| 业务回调调用 OnRead(buffer) | RtmpConnection | 普通成员函数，通过注册的 lambda 调用 |
| Session 调用 SendMediaData | RtmpSink 声明，RtmpConnection 实现 | 虚函数重写 |
| Scheduler::Start 内调用 HandleEvent | TaskScheduler 声明，EpollTaskScheduler 实现 | 虚函数重写 |

**读完应能回答：**“没有重写”与“没有这个功能”一样吗？不一样，可能直接沿用基类实现。

## 2. main 与 RtmpServer：没有写 accept，是因为基类替你创建了它

先看入口里实际做了什么：


**实际源码：main.cpp，第 8 行起（节选）**

```cpp
    EventLoop loop(2);
    auto rtmp_server = RtmpServer::Create(&loop);
    rtmp_server->SetChunkSize(60000);
    rtmp_server->SetEventCallback([](std::string type,std::string stream_path){
        printf("[Event]%s,stream_path%s\n",type.c_str(),stream_path.c_str());
    });
    if(!rtmp_server->Start("172.21.205.121",1935))
    {
        printf("rtmp server failed\n");
    }
```


这里的 Start 来自 TcpServer，SetChunkSize 来自另一个基类 Rtmp。RtmpServer 同时继承了网络服务器能力和 RTMP 配置能力：


**实际源码：RtmpServer.h，第 6 行起（节选）**

```cpp
class RtmpServer : public TcpServer , public Rtmp,public std::enable_shared_from_this<RtmpServer>
{
```


构造 RtmpServer 时，会先构造基类 TcpServer；所以 main 不需要再显式创建 Acceptor。


**实际源码：RtmpServer.cpp，第 11 行起（节选）**

```cpp
RtmpServer::RtmpServer(EventLoop* eventloop)
    :TcpServer(eventloop)
    ,loop_(eventloop)
    ,event_callbacks_(10)
{
```



**实际源码：../EdoyunNet/TcpServer.cpp，第 5 行起（节选）**

```cpp
TcpServer::TcpServer(EventLoop *eventloop)
    :loop_(eventloop)
    ,port_(0)
    ,acceptor_(new Acceptor(eventloop))
{
```


`:TcpServer(eventloop)` 是调用基类构造函数；`,acceptor_(new Acceptor(eventloop))` 在基类里创建接收器。

```text
main 创建 RtmpServer
  → 先执行 TcpServer 构造
      → 创建 Acceptor
      → 注册“有新连接时做什么”
  → 执行 RtmpServer 自己的构造部分
```

Start 的实现完全在基类：


**实际源码：../EdoyunNet/TcpServer.cpp，第 29 行起（节选）**

```cpp
bool TcpServer::Start(std::string ip, uint16_t port)
{
    // 启动前先停止旧监听，保证重复 Start 时不会保留旧状态。
    Stop();
    if(!is_stared_)
    {
        int ret=acceptor_->Listen(ip,port);
        printf("TcpServer::Start ret=%d\r\n",ret);
        if(ret < 0)
        {
            return false;
        }
        port_ = port;
        ip_ = ip;
        is_stared_ = true;
    }
    return true;
}
```


你观察得对：Start 先调用 Stop。第一次启动时 is_stared_ 为 false，Stop 的主体不执行；重复启动时，它意图先停旧服务。当前 RtmpServer 没有重写 Start 或 Stop，所以都执行基类代码。

Stop 内部断开客户端和关闭监听，不能把它误理解为“停止 EventLoop 线程”。EventLoop/TaskScheduler 的 Stop、Quit 是另一套对象上的操作。

RtmpServer::Create 是创建对象的工厂函数，不是单例，头文件里“设单例”那条注释不准确。

**读完应能回答：**main 没有 Acceptor，谁创建的？TcpServer 构造函数创建的。

## 3. Acceptor 与 TcpSocket：有两种 socket，不要混

| socket | 有几个 | 做什么 | 可读通常意味着什么 |
|---|---|---|---|
| 监听 socket | 一个监听地址对应一个 | 等待客户端连接 | 有完成建连、可 accept 的连接；也需处理错误等情况 |
| 已连接客户端 socket | 每个客户端一个 | 收发该客户端的字节 | 有数据、对端关闭或相关状态，需用 recv 结果确认 |

TcpSocket 只是包装系统调用：Create 调 socket，Bind 调 bind，Listen 调 listen，Accept 调 accept。



**实际源码：../EdoyunNet/TcpSocket.cpp，第 92 行起（节选）**

```cpp
int TcpSocket::Accept()
{
    struct sockaddr_in addr = {0};
    socklen_t addrlen = sizeof(addr);
    return ::accept(sockfd_,(struct sockaddr*)&addr,&addrlen);
}
```


Accept 返回的新 fd 用于和这个客户端通信，原来的监听 fd 继续接收其他客户端。

Acceptor::Listen 中创建监听 Channel，并安排可读时调用 OnAccept：


**实际源码：../EdoyunNet/Acceptor.cpp，第 39 行起（节选）**

```cpp
    channelPtr_->SetReadCallback([this](){this->OnAccept();});
    channelPtr_->EnableReading();
    loop_->UpdateChannel(channelPtr_);
    return 0;
}
```



**实际源码：../EdoyunNet/Acceptor.cpp，第 55 行起（节选）**

```cpp
void Acceptor::OnAccept()
{
    // 监听 fd 可读表示有新连接请求，accept 返回的是客户端 fd。
    int fd = tcp_socket_->Accept();
    if(fd > 0)
    {
        if(new_connectCb_)
        {
            new_connectCb_(fd);
        }
    }
}
```


注意这里分两次通知：Channel 的 read callback 进入 OnAccept；OnAccept 拿到客户端 fd，再执行 new_connectCb_(fd)。new_connectCb_ 不是系统自动知道的，是 TcpServer 构造时设置的。

TCP 三次握手由内核处理，accept 从监听 socket 的队列取出已建立连接。应用取得客户端 fd 后，才开始按 RTMP 协议读取字节。

**读完应能回答：**监听 socket 可读后马上 recv 音视频吗？不是，先 accept 得到客户端 socket；音视频从客户端 socket 上读取。

## 4. EventLoop、线程与 Channel：到底谁在调用回调

线程是执行代码的执行单元；Connection 是保存一个连接状态的对象。它们不是一个东西。本项目不是一个客户端对应一个新线程，而是 EventLoop 创建若干 Scheduler 线程，多个连接分配给这些线程管理。

EventLoop 构造时调用 Loop，里面启动线程：


**实际源码：../EdoyunNet/EventLoop.cpp，第 78 行起（节选）**

```cpp
    for(uint32_t n = 0; n < num_threads_; n++)
    {
        std::shared_ptr<TaskScheduler> task_schduler_ptr(new EpollTaskScheduler(n));
        task_schdulers_.push_back(task_schduler_ptr);
        std::shared_ptr<std::thread> thread(new std::thread(&TaskScheduler::Start,task_schduler_ptr.get()));
        thread->native_handle();
        threads_.push_back(thread);
    }
}
```


线程入口是 TaskScheduler::Start。它不停处理定时器和 IO 事件：


**实际源码：../EdoyunNet/TaskScheduler.cpp，第 13 行起（节选）**

```cpp
void TaskScheduler::Start()
{
    is_shutdown_ = false;
    while (!is_shutdown_)
    {
        // 定时器和 IO 事件由同一个 Reactor 线程串行处理。
        this->timer_queue_.HandleTimerEvent();
        this->HandleEvent();
    }
    
}
```


Start 自己不是虚函数，但里面调用的 HandleEvent 是虚函数，因此实际进入 EpollTaskScheduler::HandleEvent。源文件名拼为 EpoolTaskScheduler.cpp，类名是 EpollTaskScheduler。

注册 Channel 时保存了地址：


**实际源码：../EdoyunNet/EpoolTaskScheduler.cpp，第 82 行起（节选）**

```cpp
    struct epoll_event event = {0};
    if(operation != EPOLL_CTL_DEL)
    {
        event.data.ptr = Channel.get();
        event.events = Channel->GetEvents();
    }
```


随后 epoll_ctl 把关注的 fd 和事件登记到内核。事件就绪后，epoll_wait 返回相应记录，程序取出之前保存的 Channel 地址：


**实际源码：../EdoyunNet/EpoolTaskScheduler.cpp，第 61 行起（节选）**

```cpp
    num_events = epoll_wait(epollfd_,events,512,0);
    if(num_events < 0)
    {
        if(errno != EINTR)
        {
            return false;
        }
    }
    for(int n = 0; n < num_events; n++)
    {
        if(events[n].data.ptr)
        {
            ((Channel*)events[n].data.ptr)->HandleEvent(events[n].events);
        }
    }
```


当前 timeout=0，是非阻塞轮询，并非长时间阻塞等待；这只是当前项目实现。内核返回就绪信息，真正执行 C++ 回调的仍是应用的 Scheduler 线程。

Channel 再按事件类型调用保存的函数：


**实际源码：../EdoyunNet/Channel.h，第 64 行起（节选）**

```cpp
	void HandleEvent(int events)
	{	
		if (events & (EVENT_PRI | EVENT_IN)) {
			read_callback_();
		}

		if (events & EVENT_OUT) {
			write_callback_();
		}
        
		if (events & EVENT_HUP) {
			close_callback_();
			return ;
		}

		if (events & (EVENT_ERR)) {
			error_callback_();
		}
	}
```


因此“层层返回”更准确地说是“层层调用”：

```text
工作线程
 → TaskScheduler::Start 的循环
 → EpollTaskScheduler::HandleEvent
 → epoll_wait 返回就绪记录
 → 用 data.ptr 找到 Channel
 → Channel::HandleEvent
 → read_callback_()
 → 注册时指定的成员函数
```

同一套事件分发机制可以服务不同 fd：监听 Channel 注册 OnAccept；客户端 Channel 注册 HandleRead。

**读完应能回答：**谁知道“可读之后调用哪个函数”？Channel 保存注册的回调；Scheduler 找到 Channel 并触发它。

## 5. TcpServer 与 RtmpServer：OnConnect 的重写在何时生效

TcpServer 构造时把下面的动作存进 Acceptor：


**实际源码：../EdoyunNet/TcpServer.cpp，第 11 行起（节选）**

```cpp
    acceptor_->SetNewConnectCallback([this](int fd){
        TcpConnection::Ptr conn = this->OnConnect(fd);
        if(conn)
        {
            this->AddConnection(fd,conn);
            conn->SetDisConnectCallback([this](TcpConnection::Ptr conn){
                int fd = conn->GetSocket();
                this->RemoveConnection(fd);
            });
        }
    });
```


构造阶段只是注册 lambda；真正的新连接事件发生时才执行它。那时对象已是构造完成的 RtmpServer，因此虚调用 this->OnConnect(fd) 进入派生实现。

如果使用基础 TcpServer，默认工厂创建 TcpConnection：


**实际源码：../EdoyunNet/TcpServer.cpp，第 63 行起（节选）**

```cpp
TcpConnection::Ptr TcpServer::OnConnect(int fd)
{
    // 默认创建基础 TcpConnection；业务 Server 可重写为自定义连接类型。
    return std::make_shared<TcpConnection>(loop_->GetTaskSchduler().get(),fd);
}
```


现在使用 RtmpServer，工厂改成：


**实际源码：RtmpServer.cpp，第 97 行起（节选）**

```cpp
TcpConnection::Ptr RtmpServer::OnConnect(int socket)
{
    return std::make_shared<RtmpConnection>(shared_from_this(),loop_->GetTaskSchduler().get(),socket);
}
```


这里不是创建 TCP Session，而是创建 TcpConnection 或其派生类 RtmpConnection。RtmpSession 是后面按直播流建立的业务分组，和这里的一次 TCP 建连不是一回事。

返回类型虽然是 shared_ptr<TcpConnection>，实际对象仍是 RtmpConnection。基类指针保存派生对象不会把派生对象“变回”基类对象。

**读完应能回答：**监听接收流程重写了吗？没有，沿用原流程，只在 OnConnect 这个工厂入口替换连接类型。

## 6. TcpConnection 与 RtmpConnection：网络沿用，业务由回调接入

RtmpConnection 的继承关系：


**实际源码：RtmpConnection.h，第 10 行起（节选）**

```cpp
class RtmpConnection : public TcpConnection , public RtmpSink
{
public:
```


下面是构造链：公开构造委托给另一个构造，后者调用 TcpConnection 基类构造。


**实际源码：RtmpConnection.cpp，第 5 行起（节选）**

```cpp
RtmpConnection::RtmpConnection(std::shared_ptr<RtmpServer> rtmp_server, TaskScheduler *scheduler, int socket)
    :RtmpConnection(scheduler,socket,rtmp_server.get())
{
    handshake_.reset(new RtmpHandshake(RtmpHandshake::HANDSHAKE_C0C1));
    rtmp_server_ = rtmp_server;
}

RtmpConnection::RtmpConnection(TaskScheduler* scheduler,int socket,Rtmp* rtmp)
    :TcpConnection(scheduler,socket)
    ,rtmp_chunk_(new RtmpChunk())
    ,state_(HANDSHAKE)
{
```


基类构造创建读缓冲区、写队列和客户端 Channel：


**实际源码：../EdoyunNet/TcpConnection.cpp，第 5 行起（节选）**

```cpp
TcpConnection::TcpConnection(TaskScheduler *task_schduler, int sockfd)
    :task_schduler_(task_schduler)
    ,read_buffer_(new BufferReader())
    ,write_buffer_(new BufferWriter(500))
    ,channel_(new Channel(sockfd))
{
```



**实际源码：../EdoyunNet/TcpConnection.cpp，第 19 行起（节选）**

```cpp
    channel_->SetReadCallback([this](){this->HandleRead();});
    channel_->SetWriteCallback([this](){this->HandleWrite();});
    channel_->SetCloseCallback([this](){this->HandleClose();});
    channel_->SetErrorCallback([this](){this->HandleError();});
```


RtmpConnection 没有重写基类 HandleRead；它注册了另一个业务回调：


**实际源码：RtmpConnection.cpp，第 25 行起（节选）**

```cpp
    this->SetReadCallback([this](std::shared_ptr<TcpConnection> conn,BufferReader& buffer){
        return this->OnRead(buffer);
    });

    //设置关闭回调，释放资源
    this->SetCloseCallback([this](std::shared_ptr<TcpConnection> conn){
        this->OnClose();
    });
```


同样叫 SetReadCallback，调用对象不同，设置的是不同回调槽：

| 注册代码 | 回调存在哪里 | 用途 |
|---|---|---|
| channel_->SetReadCallback(...) | Channel::read_callback_ | fd 就绪后调用 TcpConnection::HandleRead |
| this->SetReadCallback(...) | TcpConnection::readCb_ | 已收字节后调用 RtmpConnection::OnRead |

这就是你找不到 override 的原因：OnRead 并没有覆盖 HandleRead，而是 HandleRead 收完数据后通过 readCb_ 主动调用 OnRead。



**实际源码：../EdoyunNet/TcpConnection.cpp，第 70 行起（节选）**

```cpp
void TcpConnection::HandleRead()
{
    // 先从 socket 读入连接缓冲区，再交给业务层 ReadCallback 解析。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(is_closed_)
        {
            return;
   	    }
        int ret = read_buffer_->Read(channel_->GetSocket());
        if(ret < 0)
   	    {
            this->Close();
            return;
        } //这个锁会释放掉
    }
    if(readCb_)
    {
        bool ret = readCb_(shared_from_this(),*read_buffer_);
        if(!ret)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            this->Close();
        }
    }
}
```


HandleRead 的第一段调用 BufferReader::Read，内部 recv；第二段调用 readCb_。BufferReader& 引用让业务解析器直接消费连接原来的输入缓冲区。

```text
客户端 Channel 的可读事件
 → 基类 HandleRead（沿用）
 → recv 到 BufferReader
 → 基类执行 readCb_
 → lambda 调派生类 OnRead（普通函数）
```

this->Send 也沿用基类，RTMP 层只需要提供准备好的字节。Send 的两个参数形式是重载，不是 RtmpConnection 的重写。

**读完应能回答：**OnRead 不声明 virtual 为什么也会执行？因为显式注册的回调调用了它。

## 7. RtmpMessage、RtmpMessageHeader、rtmp.h：三个完全不同的东西

### 7.1 RtmpMessage：Chunk 与业务层共用的本地数据容器

它不是“只存原始音视频”的结构。可以存控制消息、AMF 命令、元数据、编码后的音视频。

| type_id | playload 里装什么 | 后续交给谁 |
|---|---|---|
| 1 | 新 Chunk Size 的4字节数值 | 修改接收块大小 |
| 5 | 确认窗口的4字节数值 | 流控相关处理 |
| 20 | AMF 编码的命令名和参数 | HandleInvoke |
| 18 | AMF 编码的元数据 | HandleNotify |
| 8/9 | 音频/视频消息体 | HandleAudio/HandleVideo |

接收时 Chunk 把解析结果放进它；发送时业务层把待发送内容放进它，交 Chunk 生成网络字节。RtmpMessage 对象本身不整体发到网络，它的指针和 index 都是本地状态。

### 7.2 RtmpMessageHeader：网络头字段的临时承接结构

它存放时间、消息长度、类型、MSID 的原始字节数组，帮助 Chunk 解析。它也不是整个 RTMP 消息，不包含 playload。

### 7.3 rtmp.h：C++ 头文件，不是“网络头部”

头文件这个“头”表示源代码组织；协议 header 的“头”表示网络字节结构。#include "rtmp.h" 是编译时引入声明和定义，不会把文件内容发送给客户端。



**实际源码：rtmp.h，第 7 行起（节选）**

```cpp
static const int RTMP_VERSION = 0X3;
static const int RTMP_SET_CHUNK_SIZE = 0x1;
static const int RTMP_ABORT_MESSAGE = 0x2;
static const int RTMP_ACK = 0x3;
static const int RTMP_ACK_SIZE = 0x5;
static const int RTMP_BANDWIDTH_SIZE = 0x6;
static const int RTMP_AUDIO = 0x08;
static const int RTMP_VIDEO = 0x09;
static const int RTMP_NOTIFY = 0x12;
static const int RTMP_INVOKE = 0x14;
```


例如 RTMP_VIDEO=0x09，只是给数字9起名字，便于代码写 `msg.type_id = RTMP_VIDEO`。发送时按封装逻辑写一个类型字节9，不是把常量表发出去。

这里的 static const int：const 表示不可通过普通赋值修改，命名空间作用域的 static 使这些名字具有内部链接；它不是在定义一个自动发送的全局消息。

rtmp.h 后面的 class Rtmp 保存配置和 URL 解析能力。RtmpServer 从它继承 SetChunkSize、GetPeerBandwidth 等函数；此时这些只是内存中的配置值。

### 7.4 配置、元数据、编码配置什么时候变成网络数据

| 概念 | 例子 | 何时发送 |
|---|---|---|
| 本地配置 | max_chunk_size_=60000 | 配置 setter 本身不发；HandleConnect 调用发送函数时变成 Type=1 消息 |
| 传输控制 | 确认窗口、Set Peer Bandwidth | 本项目在处理 connect 时发送；不是视频编码码率 |
| 元数据 | 宽、高、帧率、视频码率等描述 | 常见于发布开始后的 onMetaData，也可以更新 |
| 编码配置/序列头 | H.264 的 SPS/PPS、AAC 配置 | 解码普通媒体前需要；项目会缓存并向新观众补发 |
| 实际媒体 | 压缩视频数据、压缩音频数据 | 发布成功后持续发送 |

Set Peer Bandwidth 在 RTMP 中主要涉及发送窗口约束，不能直接等同于“编码器每秒产生多少视频数据”。这份代码也没有设置编码器码率的实现。

本地设置值如何变成网络消息，看下面的实际函数：


**实际源码：RtmpConnection.cpp，第 555 行起（节选）**

```cpp
void RtmpConnection::SetChunkSize()
{
    rtmp_chunk_->SetOutChunkSize(max_chunk_size_);
    std::shared_ptr<char> data(new char[4],std::default_delete<char[]>());
    WriteUint32BE(data.get(),max_chunk_size_);
    RtmpMessage rtmp_msg;
    rtmp_msg.type_id = RTMP_SET_CHUNK_SIZE;
    rtmp_msg.playload = data;
    rtmp_msg.lenght = 4;
    SendRtmpChunks(RTMP_CHUNK_CONTROL_ID,rtmp_msg);
}
```


这里先把数字编码为4字节，放入 Message 的 playload，再把 Message 交给分块器发送。你看到的常量、成员变量，只有经过这样的发送路径才会出现在网络上。

注意 main 中 rtmp_server->SetChunkSize(60000) 调用的是 Rtmp 基类的配置 setter；这里 RtmpConnection::SetChunkSize() 是另一个类的无参发送方法。两者不是继承链上的重写关系。

**读完应能回答：**rtmp.h 是网络包吗？不是。Message 只装视频吗？不是。设置成员变量会自动通知对端吗？不会。

## 8. AMF：谁来读懂 connect、publish、play

Chunk 解决“数据拼完整了没有”；AMF 解决“这串命令字节里是什么字符串、数字、对象”。AMF 不是线程，也不是 Connection，它是序列化格式；项目用 AmfEncoder/AmfDecoder 实现部分 AMF0 功能。

```text
命令名、事务编号、参数对象
 → AMF 编码为字节
 → 作为 Message.playload
 → Chunk 封装
 → TCP 发送

TCP 收到字节
 → Chunk 重组 Message
 → 看 type_id 是命令
 → AMF 解码 playload
 → 得到命令名并执行对应操作
```

HandleInvoke 先只解一个值，取出命令名：


**实际源码：RtmpConnection.cpp，第 134 行起（节选）**

```cpp
    amf_decoder_.reset(); //解析消息

    //解析消息名称
    int bytes_used =  amf_decoder_.decode(rtmp_msg.playload.get(),rtmp_msg.lenght,1);
    if(bytes_used < 0)
    {
        printf("bytes_used < 0\n");
        return false;
    }

    std::string method = amf_decoder_.getString();
```


这个 decode 最后的1表示先解一个值。bytes_used 记录该值消耗多少字节，后续从 playload+bytes_used 继续解释其他参数。

头里 type_id=20 只能说明是 AMF0 命令，不能说明一定是 publish。命令名在消息体里。音视频压缩数据通常直接按媒体消息体格式处理，不把整个媒体数据交给 AMF 解码。

**读完应能回答：**Chunk 为什么不直接调用 HandlePublish？它只负责传输封装，命令名要等上层 AMF 解释出来才知道。

## 9. RtmpConnection：握手之后到底按什么顺序通信

### 9.1 先看每一步改变什么

| 阶段 | 发送方→接收方 | 交换内容 | 结束后知道什么 |
|---|---|---|---|
| TCP 建连 | 客户端↔内核网络栈 | TCP 握手 | 有一条可传字节的连接 |
| RTMP 握手 | 客户端↔服务端 | C0C1、S0S1S2、C2 | RTMP 初始化交互完成 |
| connect | 客户端→服务端 | AMF命令，app 等参数，MSID=0 | 客户端要连接哪个应用 |
| connect 响应 | 服务端→客户端 | 控制参数、_result 成功 | 应用连接被接受 |
| createStream | 客户端→服务端 | 请求创建逻辑消息流 | 等待分配编号 |
| createStream 响应 | 服务端→客户端 | _result 携带 MSID | 后续操作用哪个消息流编号 |
| publish 或 play | 客户端→服务端 | 流名称、操作意图，使用分配的MSID | 客户端是哪个流的主播/观众 |
| 状态通知 | 服务端→客户端 | onStatus | 推流/播放阶段结果 |
| 媒体阶段 | 主播→服务器→观众 | 配置、元数据、音视频 | 实际传输内容 |

从 connect 开始，表中命令和控制消息也已经需要 Chunk 封装。不是只有视频才分块。常见客户端可能插入其他辅助命令；这里沿当前项目主路径讲。

### 9.2 OnRead 分流：握手完成才交 Chunk



**实际源码：RtmpConnection.cpp，第 39 行起（节选）**

```cpp
bool RtmpConnection::OnRead(BufferReader &buffer)
{
    bool ret = true;
    if(handshake_->IsCompleted())//是否握手完成，完成之后才能发送message
    {
        ret = HandleChunk(buffer);
    }
    else
    {
        std::shared_ptr<char> res(new char[4096],std::default_delete<char[]>());
        int res_size = handshake_->Parse(buffer,res.get(),4096);
        if(res_size < 0)
        {
            ret = false;
        }
        if(res_size > 0)
        {
            this->Send(res.get(),res_size);
        }
        if(handshake_->IsCompleted())
        {
            if(buffer.ReadableBytes() > 0)
            {
                ret = HandleChunk(buffer);
            }
        }
    }
    return ret;
}
```


握手 Parse 的返回值是响应长度；Chunk Parse 的返回值是消费字节数。名字相同但语义不同。握手后缓冲区有剩余字节，立即送去处理 Chunk。

### 9.3 connect：确定 app，回复参数与结果



**实际源码：RtmpConnection.cpp，第 307 行起（节选）**

```cpp
    AmfObject amfObj = amf_decoder_.getObject("app");
    app_ = amfObj.amf_string; //获取引用程序的值
    if(app_ == "")
    {
        return false;
    }

    SendAcknowlegement();
    SetPeerBandWidth();
    SetChunkSize();
```


例如 app="live"。随后 AMF 编码 _result，回显事务编号，并返回 NetConnection.Connect.Success。

函数名 SendAcknowlegement 容易误导：当前函数发送 Type=5 的确认窗口大小，不是 Type=3 的确认计数。SetPeerBandWidth 发 Type=6，SetChunkSize 发 Type=1。它们是三条独立控制消息，不是塞进视频头里。

### 9.4 createStream：返回逻辑流编号



**实际源码：RtmpConnection.cpp，第 342 行起（节选）**

```cpp
bool RtmpConnection::HandleCreateStream()
{

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
```


事务编号配对命令请求与响应；返回的 stream_id 才是后续 MSID。项目的编号取自共享计数，是实现策略；MSID 的协议作用仍是连接内的逻辑消息流，不是房间名。

### 9.5 publish/play：取流名，决定角色



**实际源码：RtmpConnection.cpp，第 159 行起（节选）**

```cpp
    else if(rtmp_msg.stream_id == stream_id_)
    {
        bytes_used += amf_decoder_.decode(rtmp_msg.playload.get() + bytes_used ,rtmp_msg.lenght - bytes_used,3);
        stream_name_ = amf_decoder_.getString();
        stream_path_ = "/" + app_ + "/" + stream_name_;
```


取得 streamName="room100"，组成 /live/room100，之后分发 HandlePublish 或 HandlePlay。

发布成功：查重 → 获取Session → 回复 Publish.Start → 标记 START_PUBLISH → AddSink。

播放：回复 Play.Reset/Play.Start 等 → 标记 START_PLAY → 获取Session → AddSink。

ConnectionState 枚举列出了很多名字，但不代表当前代码每一步都会给 state_ 赋对应值。阅读时看实际赋值语句，握手完成主要看 handshake_->IsCompleted()。

### 9.6 主播和观众不是一条连接的两个阶段

```text
主播自己的连接：TCP → RTMP握手 → connect → createStream → publish
观众自己的连接：TCP → RTMP握手 → connect → createStream → play
                                       ↓
                    相同 /app/streamName → 同一 Session
```

播放器不需要先 publish 再 play。主播和观众各自完成前置流程，各自获得 MSID。服务端通过内容路径把他们关联。

**读完应能回答：**握手完为什么还不知道房间？因为 app 和流名在后续命令里才传来。

## 10. RtmpSession 与 RtmpSink：找到同一路内容的所有观众

Session 是本服务器内部“一路内容的参与者集合”，不是 TCP 协议对象，也不负责 accept。Server 的 map 按路径管理它：


**实际源码：RtmpServer.h，第 29 行起（节选）**

```cpp
    std::unordered_map<std::string,RtmpSession::Ptr> rtmp_sessions_;
```



**实际源码：RtmpSession.cpp，第 14 行起（节选）**

```cpp
void RtmpSession::AddSink(std::shared_ptr<RtmpSink> sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    rtmp_sinks_[sink->GetId()] = sink;
    if(sink->IsPublisher()) //如果是主播，说明这个直播间刚刚创建，没有设置元数据
    {
        avc_sequence_header_ = nullptr;
        aac_sequence_header_ = nullptr;
        avc_sequence_header_size_ = 0;
        aac_sequence_header_size_ = 0;
        has_publisher_ = true;
        publisher_ = sink;
    }
    return ;
}
```


Session 登记连接时使用 RtmpSink 接口。RtmpConnection 多重继承了这个接口，因此可以被当作 Sink 使用。dynamic_pointer_cast<RtmpSink> 是把现有连接转换为接口视图，没有创建第二条网络连接。

```cpp
// 概念示意
session->AddSink(连接的RtmpSink视图);
// 后续通过接口虚调用，进入那个连接的 RtmpConnection::SendMediaData
sink->SendMediaData(...);
```

publisher_ 和 rtmp_sinks_ 使用 weak_ptr，Session 不靠这些成员强行延长所有连接的生命。连接也弱引用 Server/Session，避免强引用环。TcpServer 连接表主要持有连接的 shared_ptr；事件或定时器的临时 shared_ptr 也可能延长对象生命。

Session 转发时筛选 IsPlayer，给新观众尝试补发 AAC/AVC 配置：


**实际源码：RtmpSession.cpp，第 93 行起（节选）**

```cpp
            if(conn->IsPlayer())
            {
                if(!conn->IsPlaying()) //刚进直播间
                {
                    conn->SendMediaData(RTMP_AAC_SEQUENCE_HEADER,timestamp,aac_sequence_header_,aac_sequence_header_size_); //音频元数据
                    conn->SendMediaData(RTMP_AVC_SEQUENCE_HEADER,timestamp,avc_sequence_header_,avc_sequence_header_size_); //视频元数据
                }
                //观看中
                conn->SendMediaData(type,timestamp,data,size);
            }
```


序列头供解码器初始化，不是普通视频画面；关键帧提供合适的视频解码起点。项目缓存序列头，并尝试让观众等关键帧，但没有完整 GOP 回放，相关筛选实现也有缺陷。

**读完应能回答：**Session 调 SendMediaData 为何能发送到 socket？接口虚调用进入 RtmpConnection，再沿用 TcpConnection 的发送能力。

## 11. 一份视频经过哪些对象，最终怎么发送

```text
主播 socket
 → TcpConnection::HandleRead（沿用网络层）
 → RtmpConnection::OnRead（业务回调）
 → RtmpChunk::Parse（重组）
 → RtmpMessage（完整结果）
 → HandleMessage → HandleVideo
 → RtmpSession::SendMediaData（找观众）
 → 观众 RtmpConnection::SendMediaData（准备发送消息）
 → RtmpChunk::CreateChunk（重新封装）
 → TcpConnection::Send（沿用基类）
 → BufferWriter 队列 → send（系统调用）
```

观众连接创建自己的消息描述，包括自己的 MSID：


**实际源码：RtmpConnection.cpp，第 681 行起（节选）**

```cpp
    //收到关键帧之后开始发送message
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
```



**实际源码：RtmpConnection.cpp，第 608 行起（节选）**

```cpp
void RtmpConnection::SendRtmpChunks(uint32_t csid, RtmpMessage &rtmp_msg)
{
    uint32_t capacity = rtmp_msg.lenght + rtmp_msg.lenght / max_chunk_size_ * 5 + 1024; 
    std::shared_ptr<char> buffer(new char[capacity],std::default_delete<char[]>());
    int size = rtmp_chunk_->CreateChunk(csid,rtmp_msg,buffer.get(),capacity);
    if(size > 0)
    {
        this->Send(buffer.get(),size);
    }
}
```


CreateChunk 写出的多个 Chunk 已经按顺序放在一个缓冲区。Send 不需要再次拆块，只处理字节进度：


**实际源码：../EdoyunNet/BufferWriter.cpp，第 57 行起（节选）**

```cpp
        Packet &pkt = buffer_.front();
        ret = ::send(sockfd,pkt.data.get() + pkt.writeIndex,pkt.size - pkt.writeIndex,0);
        if(ret > 0)
        {
            pkt.writeIndex += ret;
            if(pkt.size == pkt.writeIndex)
            {
                count += 1;
                buffer_.pop();
            }
        }
```


队列的 Packet 是一次待发数据条目，可以包含多个 Chunk。writeIndex 记录已发字节，未发完就从剩余位置继续。TCP 发送/接收次数与 Chunk 数量没有一一对应关系。

线程也不会因为经过多个函数而自动改变：Session 在调用观众发送函数时，默认仍在当前调用线程。关注对方 socket 的可写事件之后，后续续发才可能由负责该连接的 Scheduler 线程执行。项目用锁保护部分共享路径，但这不等于所有跨线程行为都已验证正确。

**读完应能回答：**这份服务端有没有解码视频再重新编码？这条路径没有，主要转发已编码媒体并重新做 RTMP 封装。

## 12. 关闭、速查和下一次读代码的任务

### 12.1 关闭分三件事

- Connection 关闭：摘除客户端 Channel、触发业务关闭回调、从 TcpServer 连接表移除。
- Session 移除参与者：OnClose 调 HandleDeleteStream，通过短定时器 RemoveSink；主播移除时清除配置缓存。
- Server 清理空 Session：定时检查；停止整个服务器的监听和停止事件线程又是不同操作。

### 12.2 最容易混淆的六组名字

| 名称 | 区分 |
|---|---|
| C++头文件 / 协议Header | 编译期源码组织 / 网络字节结构 |
| TcpConnection / RtmpConnection | 基类网络能力 / 派生类RTMP业务，属于同一派生对象 |
| Connection / Thread | 一条连接的状态 / 执行代码的线程，一个线程可管理多条连接 |
| Channel可读回调 / Connection业务回调 | 就绪后去收字节 / 收到字节后去解释协议 |
| RtmpMessage / 原始视频 | 通用消息容器 / 只是它可能承载的一种内容，常为编码后数据 |
| 配置变量 / 控制消息 | 本地内存值 / 编码并调用Send后才传到对端 |

### 12.3 给自己走一遍，不必背字段

1. 在 main 找 Start，跳到 TcpServer::Start，确认它调用哪个 Listen。
2. 在 TcpServer 构造找到 new Acceptor，确认不是 main 漏写了接收器。
3. 在 Acceptor::Listen 找 SetReadCallback、EnableReading、UpdateChannel，分别理解“存动作、设事件、注册到调度器”。
4. 在 Scheduler 找 epoll_wait，再到 Channel::HandleEvent，看到回调真正被调用的位置。
5. 回到 OnAccept，看客户端 fd 怎样传给 new_connectCb_，再看 OnConnect 虚调用创建派生连接。
6. 到 TcpConnection 构造看客户端 Channel 的注册；到 RtmpConnection 构造看第二层业务回调注册。
7. 走 HandleRead → OnRead → HandleChunk → HandleMessage → HandleInvoke，解释每一步输入输出。
8. 用 connect/live、createStream/编号1、publish/room100 说明三个命令改变了什么。
9. 再创建一个观众连接走 play，解释为什么与主播加入同一个 Session。
10. 沿第11章把一条视频从接收跟到 send，最后说清楚哪里是重组，哪里是重新封装。

### 12.4 当前代码的边界

这份指南解释正常主线，不是代码已经完善的声明。当前存在扩展CSID计算/类型、时间戳继承、元数据转发使用错误成员、关键帧筛选等问题；读写错误处理和发送队列背压也不完整。不要把原注释里“设单例”“读写权限”等措辞直接当协议事实。

先掌握对象、继承、回调和命令顺序，再专门阅读这些实现缺陷。源码节选行号对应本次生成时的文件，后续编辑后可能变化。

