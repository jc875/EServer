#ifndef _TCPSERVER_H_
#define _TCPSERVER_H_
#include <memory>
#include <string>
#include <unordered_map>
#include "TcpConnection.h"

class EventLoop;
class Acceptor;

// TCP 服务器的管理层。
// TcpServer 组合 Acceptor 和多个 TcpConnection，负责服务器启停、
// 新连接创建以及连接生命周期管理；具体协议由派生类实现。
class TcpServer
{
public:
    TcpServer(EventLoop* eventloop);
    ~TcpServer();
    // 启动监听。
    virtual bool Start(std::string ip,uint16_t port);
    // 停止监听并断开当前所有客户端。
    virtual void Stop();
    inline  std::string GetIPAddres()const{return ip_;}
    inline  uint16_t GetPort()const{return port_;}
protected:
    // 将 accept 得到的 client fd 转换为 TcpConnection。
    // 业务 Server 可重写此函数，创建自定义连接类型。
    virtual TcpConnection::Ptr OnConnect(int fd);
    // 维护 fd 到连接对象的映射。
    virtual void AddConnection(int fd,TcpConnection::Ptr conn);
    virtual void RemoveConnection(int fd);
private:
    EventLoop* loop_;
    uint16_t port_;
    std::string ip_;
    std::unique_ptr<Acceptor> acceptor_;
    bool is_stared_ = false;
    std::unordered_map<int,TcpConnection::Ptr> connects_;
};
#endif