#include "EpoolTaskScheduler.h"
#include <vector>

// EventLoop 是多个 TaskScheduler（多个 Reactor）的管理器。
// 它向上层提供统一的定时器和 Channel 操作接口，并负责创建、停止
// 以及回收各个事件处理线程。
class EventLoop
{
public:
    EventLoop(uint32_t num_threads = -1);
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator = (const EventLoop&) = delete;
    // 轮询选择一个 TaskScheduler，用于分配新建立的客户端连接。
    std::shared_ptr<TaskScheduler> GetTaskSchduler();
    TimerId AddTimer(const TimerEvent& event,uint32_t mesc);
    void RemvoTimer(TimerId timerId);
    // 将 Channel 注册或更新到默认的 TaskScheduler。
    // 当前实现中监听 socket 会进入 task_schdulers_[0]。
    void UpdateChannel(ChannelPtr channel);
    // 从默认的 TaskScheduler 移除 Channel。
    void RmoveChannel(ChannelPtr& channel);
    // 创建各个 TaskScheduler 及其工作线程。
    void Loop();
    // 请求所有 Scheduler 停止，并等待线程退出。
    void Quit();
private:
    uint32_t num_threads_ = 1;
    uint32_t index_ = 1;
    std::vector<std::shared_ptr<TaskScheduler>> task_schdulers_;
    std::vector<std::shared_ptr<std::thread>> threads_;
};