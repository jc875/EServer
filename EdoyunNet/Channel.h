#ifndef _CHANNEL_H_
#define _CHANNEL_H_
#include <functional>
#include <memory>

enum EventType
{
	EVENT_NONE   = 0,
	EVENT_IN     = 1,
	EVENT_PRI    = 2,		
	EVENT_OUT    = 4,
	EVENT_ERR    = 8,
	EVENT_HUP    = 16,
};

// Channel 是 socket 与事件回调之间的适配层。
// 它不负责创建或关闭 socket，只记录 fd 当前关注的事件，
// 并在事件调度器发现事件后调用对应回调。
class Channel 
{
public:
	typedef std::function<void()> EventCallback;
	Channel(int sockfd) 
		: sockfd_(sockfd){}
		
	~Channel() {};
    
	// 注册可读事件回调，例如 Acceptor::OnAccept 或 TcpConnection::HandleRead。
	inline void SetReadCallback(const EventCallback& cb)
	{ read_callback_ = cb; }

	// 注册可写、关闭和错误事件回调。
	inline void SetWriteCallback(const EventCallback& cb)
	{ write_callback_ = cb; }

	inline void SetCloseCallback(const EventCallback& cb)
	{ close_callback_ = cb; }

	inline void SetErrorCallback(const EventCallback& cb)
	{ error_callback_ = cb; }

	inline int GetSocket() const { return sockfd_; }

	inline int  GetEvents() const { return events_; }
	inline void SetEvents(int events) { events_ = events; }
    
	inline void EnableReading() 
	{ events_ |= EVENT_IN; }

	inline void EnableWriting() 
	{ events_ |= EVENT_OUT; }
    
	inline void DisableReading() 
	{ events_ &= ~EVENT_IN; }
    
	inline void DisableWriting() 
	{ events_ &= ~EVENT_OUT; }
       
	inline bool IsNoneEvent() const { return events_ == EVENT_NONE; }
	inline bool IsWriting() const { return (events_ & EVENT_OUT) != 0; }
	inline bool IsReading() const { return (events_ & EVENT_IN) != 0; }
    
	// 由底层事件调度器调用，把 epoll 返回的事件分发给上层对象。
	void HandleEvent(int events)
	{	
		if (events & (EVENT_PRI | EVENT_IN)) {
			read_callback_();
		}

		if (events & EVENT_OUT) {
			write_callback_();
		}
        
		if (events & EVENT_HUP) {
			close_callback_();
			return ;
		}

		if (events & (EVENT_ERR)) {
			error_callback_();
		}
	}

private:
	EventCallback read_callback_  = []{};
	EventCallback write_callback_ = []{};
	EventCallback close_callback_ = []{};
	EventCallback error_callback_ = []{};
	int sockfd_ = 0;
	int events_ = 0;    
};

typedef std::shared_ptr<Channel> ChannelPtr;
#endif