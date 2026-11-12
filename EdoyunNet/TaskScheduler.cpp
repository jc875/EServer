#include "TaskScheduler.h"

TaskScheduler::TaskScheduler(int id)
    :id_(id)
    ,is_shutdown_(false)
{
}

TaskScheduler::~TaskScheduler()
{
}

void TaskScheduler::Start()
{
    is_shutdown_ = false;
    while (!is_shutdown_)
    {
        // 定时器和 IO 事件由同一个 Reactor 线程串行处理。
        this->timer_queue_.HandleTimerEvent();
        this->HandleEvent();
    }
    
}

void TaskScheduler::Stop()
{
    is_shutdown_ = true;
}

TimerId TaskScheduler::AddTimer(const TimerEvent &event, uint32_t mesc)
{
    return timer_queue_.AddTimer(event,mesc);
}

void TaskScheduler::RemvoTimer(TimerId timerId)
{
    timer_queue_.RemoveTimer(timerId);
}
