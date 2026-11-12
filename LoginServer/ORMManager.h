#ifndef _ORMMANAGER_H_
#define _ORMMANAGER_H_
#include <memory>
#include <mutex>
#include <string>
#include <mysql/mysql.h>

// ============================================================
// ORMManager：MySQL 访问封装（单例），LoginServer 唯一的数据库入口
// 表：users 库的 clients 表，列：USER_NAME/USER_ACOUNT/USER_PASSWD/USER_CODE/
//     USER_ONLINE(是否在线)/USER_RECENTLY_LOGIN(最近登录时间)/USER_SVR_MOUNT(分配的服务器)
//
// 【2026-11-05 改造】不再对外交 MYSQL_ROW（行内存归 MYSQL_RES，free_result 后即悬垂）。
// 改为把一行拷进 ClientRow 再释放结果集，返回值统一用 bool：
//   QueryClient(code, out)  查：查到返回 true 并填 out
//   InsertClient(row)       注册：插入新行
//   SetOnline(code,online,when,svr)  更新在线状态（UPDATE，登录/断线共用）
//   DeleteClient(code)      注销：删除该行
// 注意：SQL 仍用 snprintf 拼接（未参数化）——客户端可控字段有注入风险，
//       彻底解决需换 mysql_stmt_prepare + mysql_stmt_bind_param。
// ============================================================

// 一行用户记录的 C++ 副本：生命周期归调用方，不再依赖 MYSQL_RES
struct ClientRow
{
    std::string name;            // USER_NAME
    std::string acount;          // USER_ACOUNT
    std::string passwd;          // USER_PASSWD（当前为明文/客户端传来的摘要，见 LoginConnection 注释）
    std::string code;            // USER_CODE（用户唯一编号，也是 SigServer 房间号）
    int         online = 0;      // USER_ONLINE 0/1
    long        recently_login = 0;  // USER_RECENTLY_LOGIN 秒级时间戳
    std::string svr_mount;       // USER_SVR_MOUNT 被分配的信令服务器地址
};

class ORMManager
{
public:
    ~ORMManager();
    static ORMManager* GetInstance(); //全局实例（call_once 只建一次）

    bool QueryClient(const std::string& code, ClientRow& out);   // 按 code 查一行
    bool InsertClient(const ClientRow& c);                       // 插入一行（注册）
    bool SetOnline(const std::string& code, int online, long when, const std::string& svr);  // 更新在线状态
    bool DeleteClient(const std::string& code);                  // 按 code 删除

private:
    ORMManager();                          // 私有构造：连接 MySQL(127.0.0.1:3306, root/123456, 库 users)
    bool Exec(const std::string& sql);     // 执行无结果集 SQL 的公共入口（返回是否成功）
    MYSQL mysql_;                          // MySQL 连接句柄
private:
    static std::unique_ptr<ORMManager> instance_;
};
#endif
