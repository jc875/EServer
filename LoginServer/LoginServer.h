#include "../EdoyunNet/TcpServer.h"
#include "TcpClient.h"

// ============================================================
// LoginServer：登录/注册服务器（继承 TcpServer 复用 epoll 框架）
// 职责：
//   1. 对外(端口 9867)：处理客户端的 注册/登录/注销——账密校验走 MySQL(ORMManager)，
//      登录成功下发"信令服务器地址"(SigServer 172.21.205.121:6539) 让客户端去连；
//   2. 对内：作为 TcpClient 每 1 秒向负载服务器(172.21.205.121:8523)上报本机内存监控。
// 类比：登录服务器=大堂，负责发"房卡"(信令服务器地址)；负载上报=给物业报水电表。
// ============================================================
class LoginServer : public TcpServer
{
public:
   static std::shared_ptr<LoginServer> Create(EventLoop* eventloop);  // 工厂（单例风格）
   ~LoginServer();
private:
   TimerId id_;                    // 定时器句柄：每秒上报监控
   EventLoop* loop_;               // 事件循环（OnConnect 时取任务调度器）
   LoginServer(EventLoop* eventloop);
   virtual TcpConnection::Ptr OnConnect(int socket);  // 框架回调：accept 后建业务连接
   std::unique_ptr<TcpClient> client_;  //连接负载服务器的客户端（上报内存/端口）
};