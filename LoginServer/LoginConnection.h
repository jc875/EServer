#include "../EdoyunNet/TcpConnection.h"
#include "define.h"
#include <string>

// ============================================================
// LoginConnection：LoginServer 的"每连接业务对象"（继承 TcpConnection）
// 职责：解析登录服务器的三种业务报文——注册(Register)/登录(Login)/注销(Destory)，
//       每种都先做"时间戳防过期(TIMEOUT=60s)"校验，再查/写 MySQL（经 ORMManager），
//       最后回一个结果码报文给客户端。
// 类比：酒店前台办入住——注册=登记新客人，登录=核对身份后给房卡(下发信令服务器地址)，
//       注销=退房删记录。
// 【2026-11-05 改造】
//   1. 登录补齐密码校验（原来完全不校验）；
//   2. 不再使用 MYSQL_ROW，改读 ClientRow（见 ORMManager）；
//   3. 改在线状态用 UPDATE（原来用 INSERT）；
//   4. 增加 Close 回调：连接断开时把用户在线状态置 0，否则下次永远 ALREADY_LOGIN。
// ============================================================
class LoginConnection : public TcpConnection
{
public:
    LoginConnection(TaskScheduler* scheduler,int socket);
    ~LoginConnection();
protected:
	bool IsTimeout(uint64_t timestamp);   // 报文时间戳是否过期（防重放）
    bool OnRead(BufferReader& buffer);    // 框架读回调
    void HandleMessage(BufferReader& buffer);  // 按 cmd 分发
    void Clear();                         // 清理
    void DisConnected();                  // 断开回调：已登录过则把 online 置 0
private:
    void HandleRegister(const packet_head* data);  // 注册处理
    void HandleLogin(const packet_head* data);     // 登录处理
    void HandleDestory(const packet_head* data);   // 注销处理
    std::string code_;                    // 本连接已登录的用户 code（断线置离线用，空=未登录）
};
