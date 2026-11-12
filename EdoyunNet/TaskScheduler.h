#ifndef _TASKSCHDULER_H_
#define _TASKSCHDULER_H_
#include <cstdint>
#include "Timer.h"
#include "Channel.h"
#include <atomic>
#include <mutex>

// 一个 TaskScheduler 表示一个事件循环执行单元。
// 它负责在自己的线程中处理定时器和 IO 事件；具体的 IO 多路复用
// 由派生类实现，例如 EpollTaskScheduler 使用 Linux epoll。
class TaskScheduler
{
public:
    TaskScheduler(int id = 1);
    virtual ~TaskScheduler();
    // 线程入口：持续处理定时器和 IO 事件，直到收到停止请求。
    void Start();
    // 请求事件循环停止。实际线程回收由 EventLoop::Quit 完成。
    void Stop();
    TimerId AddTimer(const TimerEvent& event,uint32_t mesc);
    void RemvoTimer(TimerId timerId);
    // 注册或更新一个 Channel 关注的 IO 事件。
    virtual void UpdateChannel(ChannelPtr channel){};
    // 从底层 IO 多路复用器中移除一个 Channel。
    virtual void RmoveChannel(ChannelPtr& channel){};
    // 等待并分发 IO 事件，由具体的调度器实现。
    virtual bool HandleEvent(){return false;}
    inline int GetId()const{return id_;}
private:
    int id_ = 0;
    std::mutex mutex_;
    TimerQueue timer_queue_;
    std::atomic_bool is_shutdown_;
};
#endif