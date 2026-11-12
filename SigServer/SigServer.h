#include "../EdoyunNet/TcpServer.h"

// ============================================================
// SigServer：信令服务器（继承 TcpServer 复用 epoll 框架）
// 职责：管理"控制端 ↔ 被控端"的配对与信令中转——
//   1. JOIN：客户端上报房间号注册身份；
//   2. OBTAINSTREAM：控制端打听目标房间状态、约流；
//   3. CREATESTREAM/PLAYSTREAM：安排被控端推流、把流地址下发控制端；
//   4. MOUSE/KEY/WHEEL 等：控制端输入事件转发给被控端。
// 与 RtmpServer 不同：SigServer 不搬媒体数据，只传"信令/控制信息"。
// 类比：一个"远程协助调度台"，帮两边牵线，牵完线后小动作(鼠标键盘)
// 也帮传话，但画面本身走的是 RTMP（流地址下发后双方直连/走 RtmpServer）。
// ============================================================
class SigServer : public TcpServer
{
public:
   static std::shared_ptr<SigServer> Create(EventLoop* eventloop);  //设单例
   ~SigServer();
private:
   EventLoop* loop_;   // 保存事件循环，OnConnect 时取它的任务调度器建连接
   SigServer(EventLoop* eventloop);
   // 框架回调：每来一个 TCP 连接，这里返回对应的连接对象（业务子类）
   virtual TcpConnection::Ptr OnConnect(int socket);
};