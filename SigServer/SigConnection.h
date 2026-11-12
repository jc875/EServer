#include <map>
#include <vector>
#include <cstdint>
#include "define.h"
#include "../EdoyunNet/TcpConnection.h"

// ============================================================
// SigConnection：SigServer 的"每连接业务对象"（继承 TcpConnection）
// 状态机（RoleState）：NONE(未注册) → IDLE(已注册空闲) ⇄ PULLER(拉流/控制端)
//                                                     ⇄ PUSHER(推流/被控端) → CLOSE(断开)
// 职责：
//   1. 解析信令报文（packet_head + body，见 define.h）；
//   2. 处理 JOIN / OBTAINSTREAM / CREATESTREAM / DELETESTREAM / 输入事件转发；
//   3. 维护与自身关联的"对方房间号列表"（objectes_，双向 AddCustom 建立）；
//   4. 断开时向所有关联者广播删除流通知并从注册表移除自己。
// 类比：每个客户端在服务器上开的"办事窗口"——登记房间号、被约、被放流、
//       帮忙传鼠标键盘动作，都走这个窗口。
// ============================================================
class SigConnection : public TcpConnection
{
public:
    SigConnection(TaskScheduler* scheduler,int socket);
    ~SigConnection();
public:
    // 状态查询辅助（供 DoObtainStream 的分支判断用）
    bool IsAlive(){return state_ != CLOSE;}
    bool IsNoJion(){return state_ == NONE;}        // 还没 JOIN（没房间号）
    bool IsIdle(){return state_ == IDLE;};          // 空闲，可被约去推/拉
    bool IsBusy(){return (state_ == PUSHER || state_ == PULLER);};  // 忙（在推或拉）
    void DisConnected();                            // 断开清理（关闭回调）
    void AddCustom(const std::string& code);        // 记录一个关联方（对方房间号）
    void RmoveCustom(const std::string& code);      // 移除一个关联方
    RoleState GetRoleState()const{return state_;};  // 查询自己的状态（供对方 GetRoleState 判断）
    std::string GetCode()const{return code_;};      // 自己的房间号
    std::string GetStreamAddres()const{return streamAddres_;};  // 自己推流时的流地址
protected:
    bool OnRead(BufferReader& buffer);              // 框架读回调：有数据就解析
    void HandleMessage(BufferReader& buffer);       // 按 cmd 分发报文
    void Clear();                                   // 清理：通知关联者 + 注销自己
private:
    // 信令处理器（按报文 cmd 分派）
    void HandleJion(const packet_head* data);       // JOIN：注册房间号
    void HandleObtainStream(const packet_head* data);// OBTAINSTREAM：打听目标状态/约流
    void HandleCreateStream(const packet_head* data); // CREATESTREAM：被控端上报流地址
    void HandleDeleteStream(const packet_head* data); // DELETESTREAM：删除流关系
    void HandleOtherMessage(const packet_head* data); // MOUSE/KEY/WHEEL：输入事件转发
private:
    // 核心业务实现
    void DoObtainStream(const packet_head* data);   // 约流主逻辑（分目标状态 5 支）
    void DoCreateStream(const packet_head* data);   // 收到被控端流地址后广播给控制端
private:
    RoleState state_;               // 自己的角色状态机
    std::string code_;              // 自己的房间号（JOIN 时登记）
    std::string streamAddres_;      // 自己推流时的流地址（CREATESTREAM 上报后保存）
    TcpConnection::Ptr conn_ = nullptr;  // PULLER 时指向被控端连接，用于转发输入事件
    std::vector<std::string> objectes_;  // 关联方房间号列表（关心自己的人/自己关心的人）
};