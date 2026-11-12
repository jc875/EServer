#include "SigServer.h"
#include "SigConnection.h"
#include "../EdoyunNet/EventLoop.h"

// 工厂方法：new 一个 SigServer 包成 shared_ptr（private 构造函数外部不能直接 new）
std::shared_ptr<SigServer> SigServer::Create(EventLoop *eventloop)
{
    std::shared_ptr<SigServer> server(new SigServer(eventloop));
    return server;
}

// 构造函数：把 eventloop 透传给基类 TcpServer，并自己留一份
SigServer::SigServer(EventLoop *eventloop)
    :TcpServer(eventloop)
    ,loop_(eventloop)
{
}

SigServer::~SigServer()
{
}

// 框架回调（虚函数覆写）：TcpServer 每 accept 一个新连接就调这里，
// 业务上把连接包装成 SigConnection（带信令状态机），塞进框架的事件循环
TcpConnection::Ptr SigServer::OnConnect(int socket)
{
    return std::make_shared<SigConnection>(loop_->GetTaskSchduler().get(),socket);
}
