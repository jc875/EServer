#include "ORMManager.h"
#include <chrono>
#include <cstdio>   // snprintf/printf 声明（原代码缺失导致编译失败，补此 include 不改逻辑）
#include <cstdlib>  // atoi/atol
#include <vector>

std::unique_ptr<ORMManager> ORMManager::instance_ = nullptr;

// 构造函数：连接本机 MySQL（账密/库名写死，需按自己环境改）
ORMManager::ORMManager()
{
    //初始化mysql
    mysql_init(&mysql_);

    //连接数据库
    if(mysql_real_connect(&mysql_,"127.0.0.1","root","123456","users",3306,NULL,0) == NULL)
    {
        printf("连接MySql服务器失败: %s\n",mysql_error(&mysql_));
        return;
        // 【不足】连接失败后对象照常构造成功，后续每条 SQL 都会在死连接上失败；
        // 且 call_once 保证只构造一次，不会重试。生产环境应加 mysql_options 超时/重连。
    }
    printf("连接成功\n");
}

ORMManager::~ORMManager()
{
    mysql_close(&mysql_);
}

// 单例：call_once 保证只建一次（构造函数会连数据库，失败也不重试）
ORMManager *ORMManager::GetInstance()
{
    static std::once_flag flag;
    std::call_once(flag,[&](){
        instance_.reset(new ORMManager()); //只会创建一次
    });
    return instance_.get();
}

// 执行无结果集 SQL（INSERT/UPDATE/DELETE）：返回是否成功。
// mysql_query 语义：0 成功，非 0 失败
bool ORMManager::Exec(const std::string& sql)
{
    if(mysql_query(&mysql_, sql.c_str()))
    {
        printf("Exec failed: %s | SQL: %s\n", mysql_error(&mysql_), sql.c_str());
        return false;
    }
    return true;
}

// ============================================================
// 查询一行用户：把每一列拷进 ClientRow，然后才 free_result。
// 这样彻底避开"MYSQL_ROW 在 free_result 后变悬垂指针"的 use-after-free。
// ============================================================
bool ORMManager::QueryClient(const std::string& code, ClientRow& out)
{
    char query[1024];
    // 显式写列名，不用 SELECT *（以后表加列也不会导致列下标错位）
    snprintf(query, sizeof(query),
        "SELECT USER_NAME, USER_ACOUNT, USER_PASSWD, USER_CODE, "
        "USER_ONLINE, USER_RECENTLY_LOGIN, USER_SVR_MOUNT "
        "FROM clients WHERE USER_CODE = '%s'", code.c_str());

    if(mysql_query(&mysql_, query))
    {
        printf("Query failed: %s\n", mysql_error(&mysql_));
        return false;
    }

    MYSQL_RES* res = mysql_store_result(&mysql_);
    if(!res)                     // NULL 有两种含义：出错 / 语句无结果集
        return false;

    MYSQL_ROW row = mysql_fetch_row(res);
    if(!row)                     // 查无此人
    {
        mysql_free_result(res);
        return false;
    }

    // ★ 关键：先把值拷出来（NULL 列给空串，避免解引用空指针）
    out.name           = row[0] ? row[0] : "";
    out.acount         = row[1] ? row[1] : "";
    out.passwd         = row[2] ? row[2] : "";
    out.code           = row[3] ? row[3] : "";
    out.online         = row[4] ? atoi(row[4]) : 0;
    out.recently_login = row[5] ? atol(row[5]) : 0;
    out.svr_mount      = row[6] ? row[6] : "";

    mysql_free_result(res);      // 现在释放是安全的
    return true;
}

// 插入一行（注册用）：online 一般传 0
bool ORMManager::InsertClient(const ClientRow& c)
{
    char query[1024];
    snprintf(query, sizeof(query),
        "INSERT INTO clients (USER_NAME, USER_ACOUNT, USER_PASSWD, USER_CODE, "
        "USER_ONLINE, USER_RECENTLY_LOGIN, USER_SVR_MOUNT) "
        "VALUES ('%s', '%s', '%s', '%s', %d, %ld, '%s')",
        c.name.c_str(), c.acount.c_str(), c.passwd.c_str(), c.code.c_str(),
        c.online, c.recently_login, c.svr_mount.c_str());
    return Exec(query);
}

// 更新在线状态（UPDATE 而不是 INSERT）：登录置 1、断线置 0 都走这里
bool ORMManager::SetOnline(const std::string& code, int online, long when, const std::string& svr)
{
    char query[1024];
    snprintf(query, sizeof(query),
        "UPDATE clients SET USER_ONLINE = %d, USER_RECENTLY_LOGIN = %ld, "
        "USER_SVR_MOUNT = '%s' WHERE USER_CODE = '%s'",
        online, when, svr.c_str(), code.c_str());
    return Exec(query);
}

// 注销：按 code 删除
bool ORMManager::DeleteClient(const std::string& code)
{
    char query[1024];
    snprintf(query, sizeof(query),
        "DELETE FROM clients WHERE USER_CODE = '%s'", code.c_str());
    return Exec(query);
}
