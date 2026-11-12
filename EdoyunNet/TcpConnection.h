#ifndef _TCPCONNECTION_H_
#define _TCPCONNECTION_H_
#include "BufferReader.h"
#include "BufferWriter.h"
#include "Channel.h"
#include "TcpSocket.h"
#include "TaskScheduler.h"

// 表示一个已经建立的客户端 TCP 连接。
// 每个 TcpConnection 管理一个 client fd、一个 Channel 以及读写缓冲区，
// 负责网络 IO 和连接状态，不负责解析具体业务协议。
class TcpConnection : public std::enable_shared_from_this<TcpConnection>
{
public:
    using Ptr = std::shared_ptr<TcpConnection>;
    using DisConnectCallback = std::function<void(std::shared_ptr<TcpConnection>)>;
    using CloseCallback = std::function<void(std::shared_ptr<TcpConnection>)>;
    using ReadCallback = std::function<bool(std::shared_ptr<TcpConnection>,BufferReader& buffer)>;
    TcpConnection(TaskScheduler* task_schduler,int sockfd);
    virtual ~TcpConnection();

    inline TaskScheduler* GetTaskSchduler()const{return task_schduler_;}
    // 设置业务层的数据读取回调。
    inline void SetReadCallback(const ReadCallback& cb){readCb_ = cb;}
    // 设置连接关闭时的业务回调。
    inline void SetCloseCallback(const CloseCallback& cb){closeCb_ = cb;}
    inline bool IsClosed()const{return is_closed_;}
    inline int GetSocket()const{return channel_->GetSocket();}

    // 将数据放入写缓冲区并尝试发送；未发送完时会关注可写事件。
    void Send(std::shared_ptr<char> data,uint32_t size);
    void Send(const char* data,uint32_t size);
    // 主动关闭连接，并从所属的事件调度器移除 Channel。
    void DisConnect();
protected:
    virtual void HandleRead();
    virtual void HandleWrite();
    virtual void HandleClose();
    virtual void HandleError();
protected:
    friend class TcpServer;
    bool is_closed_;
    TaskScheduler* task_schduler_;
    // 由 TcpServer 设置，用于连接断开后从连接表中移除对象。
    void SetDisConnectCallback(const DisConnectCallback& cb) {disconnectCb_ = cb;}
    std::unique_ptr<BufferReader> read_buffer_;
    std::unique_ptr<BufferWriter> write_buffer_;
private:
    void Close();
    std::mutex mutex_;
    std::shared_ptr<Channel> channel_ = nullptr;
    DisConnectCallback disconnectCb_;
    CloseCallback closeCb_;
    ReadCallback readCb_;
};
#endif