#include "LoginServer.h"
#include "../EdoyunNet/EventLoop.h"
#include "LoginConnection.h"

// 工厂方法：new 一个 LoginServer 包成 shared_ptr（private 构造外部不能直接 new）
std::shared_ptr<LoginServer> LoginServer::Create(EventLoop *eventloop)
{
    std::shared_ptr<LoginServer> server(new LoginServer(eventloop));
    return server;
}

// 构造函数：透传 eventloop 给基类，并启动"向负载服务器上报监控"的定时任务
LoginServer::LoginServer(EventLoop* eventloop)
    :TcpServer(eventloop)
    ,loop_(eventloop)
    ,client_(nullptr)
{
    client_.reset(new TcpClient());
    client_->Create();                       // 初始化 socket + 读取本机内存信息
    if(client_->Connect("172.21.205.121",8523))   // 连负载服务器（IP 写死，应可配置）
    {
        client_->getMonitorInfo();           // 立即上报一次
        id_ = loop_->AddTimer([this](){      // 然后每 1000ms 定时上报
            client_->getMonitorInfo();
            return true;                     // 返回 true = 继续循环（框架定时器语义）
        },1000);
    }
}

LoginServer::~LoginServer()
{
}

// 框架回调：每个新连接包装成 LoginConnection（带注册/登录/注销业务）
TcpConnection::Ptr LoginServer::OnConnect(int socket)
{
    return std::make_shared<LoginConnection>(loop_->GetTaskSchduler().get(),socket);
}
