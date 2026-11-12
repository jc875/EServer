#include "ConnectionManager.h"

// 静态成员定义：实例指针初始为空，GetInstance 时按需创建
std::unique_ptr<ConnectionManager> ConnectionManager::instance_ = nullptr;

ConnectionManager::ConnectionManager()
{
}

ConnectionManager::~ConnectionManager()
{
    Close();   // 析构时清空全部注册
}

ConnectionManager *ConnectionManager::GetInstance()
{
    static std::once_flag flag;
    std::call_once(flag,[&](){
        instance_.reset(new ConnectionManager()); //只会创建一次
    });
    return instance_.get();
}

void ConnectionManager::AddConn(const std::string &idefy, const TcpConnection::Ptr conn)
{
    if(idefy.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = connMaps_.find(idefy);
    if(it == connMaps_.end()) //说明这个连接器未加入
    {
        connMaps_.emplace(idefy,conn);
    }
    // 若房间号已存在则静默忽略（不覆盖）——重复 JOIN 在 SigConnection::HandleJion 已拦截
}

void ConnectionManager::RmvConn(const std::string &idefy)
{
    if(idefy.empty())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = connMaps_.find(idefy);
    if(it != connMaps_.end())//查询到
    {
        connMaps_.erase(idefy);
    }
}

TcpConnection::Ptr ConnectionManager::QueryConn(const std::string &idefy)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = connMaps_.find(idefy);
    if(it != connMaps_.end())
    {
        return it->second;   // 返回共享指针（复制一份，调用方可安全持有）
    }
    return nullptr;          // 未注册/已断开
}

void ConnectionManager::Close()
{
    connMaps_.clear();
}
