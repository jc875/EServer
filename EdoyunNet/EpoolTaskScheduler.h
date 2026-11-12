#include "TaskScheduler.h"

// 基于 Linux epoll 的 TaskScheduler。
// 每个实例拥有独立的 epoll fd，通常由一个线程执行 Start，
// 因此可以作为一个 Reactor 使用。
class EpollTaskScheduler : public TaskScheduler
{
public:
    EpollTaskScheduler(int id = 0);
    virtual ~EpollTaskScheduler();
    // 将 Channel 添加到 epoll，或更新其关注的事件。
    // 该接口同时服务于 Acceptor 和 TcpConnection，并非 Acceptor 专用。
    void UpdateChannel(ChannelPtr channel);
    // 从 epoll 中移除 Channel。
    void RmoveChannel(ChannelPtr& channel);
    // 等待 epoll 事件并分发给对应的 Channel。
    bool HandleEvent();
protected:
    // 执行具体的 epoll_ctl 操作。
    void Update(int operation,ChannelPtr& Channel);
private:
    int epollfd_ = -1;
    std::mutex mutex_;
    std::unordered_map<int,ChannelPtr> channels_;
};
