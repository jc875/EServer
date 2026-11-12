#include <unordered_map>
#include <memory>
#include <mutex>
#include "../EdoyunNet/TcpConnection.h"

// ============================================================
// ConnectionManager：全局"房间号(code) → 连接"注册表（单例）
// SigServer 的核心簿记：每个客户端 JOIN 时上报 10 字节房间号，
// 服务器把它登记在这里；后续任何"查询某房间号对应的连接"
// （DoObtainStream / Clear / AddCustom 关联查找）都走它。
// 类比：酒店的房态总台——每间房(房间号)对应一个住客(连接)。
// 线程安全：所有读写都用 mutex_ 保护（多线程 epoll 下安全）。
// ============================================================
class ConnectionManager
{
public:
    ~ConnectionManager();
    static ConnectionManager* GetInstance();   // 单例入口（call_once 只建一次）
public:
    void AddConn(const std::string& idefy,const TcpConnection::Ptr conn);  // 登记：房间号→连接
    void RmvConn(const std::string& idefy);    // 注销：连接断开/删除流时移出
    TcpConnection::Ptr QueryConn(const std::string& idefy);  // 按房间号查连接（找不到返回 nullptr）
    uint32_t Size()const{return connMaps_.size();}  // 当前在线房间数
private:
    ConnectionManager();
    void Close();
    std::mutex mutex_;          // 保护 connMaps_
    static std::unique_ptr<ConnectionManager> instance_;
    std::unordered_map<std::string,TcpConnection::Ptr> connMaps_;  // code → 连接
};