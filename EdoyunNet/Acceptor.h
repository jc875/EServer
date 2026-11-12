#include <functional>
#include <memory>
#include "Channel.h"
#include "TcpSocket.h"

class EventLoop;

typedef std::function<void(int)> NewConnectCallback;

// Acceptor 封装服务器监听 socket。
// 它只负责 bind、listen 和 accept；accept 得到的客户端 fd
// 通过 new_connectCb_ 交给 TcpServer 创建 TcpConnection。
class Acceptor
{
public:
    Acceptor(EventLoop* eventloop);
    ~Acceptor();
    // 设置新连接到达后的通知回调。
    inline void SetNewConnectCallback(const NewConnectCallback& cb) {new_connectCb_ = cb;};
    // 创建监听 socket，并将监听 Channel 注册到 EventLoop。
    int Listen(std::string ip,uint16_t port);
    // 停止监听并从 EventLoop 移除监听 Channel。
    void Close();
private:
    // 监听 fd 可读时由 Channel 调用，接收一个客户端连接。
    void OnAccept();
    EventLoop* loop_ = nullptr;
    ChannelPtr channelPtr_ = nullptr;;
    std::shared_ptr<TcpSocket> tcp_socket_;
    NewConnectCallback new_connectCb_;
};