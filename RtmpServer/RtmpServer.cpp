#include "RtmpServer.h"
#include "../EdoyunNet/EventLoop.h"
#include "RtmpConnection.h"

std::shared_ptr<RtmpServer> RtmpServer::Create(EventLoop *eventloop)
{
    // RtmpServer 的构造函数是 private，外部通过这个工厂创建 shared_ptr。
    // shared_ptr 的所有权很重要：RtmpConnection 会保存对服务器的 weak_ptr。
    std::shared_ptr<RtmpServer> server(new RtmpServer(eventloop));
    return server;
}

RtmpServer::RtmpServer(EventLoop* eventloop)
    :TcpServer(eventloop)
    ,loop_(eventloop)
    ,event_callbacks_(10)
{
    // 这里先调用 TcpServer(eventloop)，初始化监听服务器的网络基础设施。
    // RtmpServer 自己的 RTMP 参数使用 Rtmp 的成员默认值：
    // chunk size=128，acknowledgement/window 等参数由 rtmp.h 中的默认值提供。
    // main() 后续可以通过 SetChunkSize/SetPeerBandwidth 修改这些默认值。

    //定时更新session
    loop_->AddTimer([this](){
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto iter = rtmp_sessions_.begin(); iter != rtmp_sessions_.end();){
            if(iter->second->GetClients() == 0)
            {
                rtmp_sessions_.erase(iter++);
            }
            else
            {
                iter++;
            }
        }
        return true;
    },3000); //3秒执行一次，循环执行
}

RtmpServer::~RtmpServer()
{

}

void RtmpServer::SetEventCallback(const EventCallback &cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    event_callbacks_.push_back(cb);
}

void RtmpServer::AddSesion(std::string stream_path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(rtmp_sessions_.find(stream_path) == rtmp_sessions_.end())
    {
        rtmp_sessions_[stream_path] = std::make_shared<RtmpSession>();
    }
}

void RtmpServer::RemoveSession(std::string stream_path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    rtmp_sessions_.erase(stream_path);
}

RtmpSession::Ptr RtmpServer::GetSession(std::string stream_path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(rtmp_sessions_.find(stream_path) == rtmp_sessions_.end())
    {
        rtmp_sessions_[stream_path] = std::make_shared<RtmpSession>();
    }
    return rtmp_sessions_[stream_path];
}

bool RtmpServer::HasPublisher(std::string stream_path)
{
    auto session = GetSession(stream_path);
    if(!session)
    {
        return false;
    }
    return (session->GetPublisher() != nullptr);
}

bool RtmpServer::HasSession(std::string stream_path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return (rtmp_sessions_.find(stream_path) != rtmp_sessions_.end());
}

void RtmpServer::NotifyEvent(std::string type, std::string stream_path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for(auto event_cb : event_callbacks_)
    {
        if(event_cb)
        {
            event_cb(type,stream_path);
        }
    }
}

TcpConnection::Ptr RtmpServer::OnConnect(int socket)
{
    // TcpServer 接受一个客户端 Socket 后调用这里。
    // 一个已建立的客户端 TCP 连接对应一个 RtmpConnection；
    // 但它不立即对应一个直播 Session，Session 要等 publish/play 的 stream_path 确定后才加入。
    return std::make_shared<RtmpConnection>(shared_from_this(),loop_->GetTaskSchduler().get(),socket);
}
