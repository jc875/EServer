#ifndef _DEFINE_H_
#define _DEFINE_H_
#include <cstdint>
#include <string>
#include <array>
#include <sys/sysinfo.h>
#include <string.h>

// ============================================================
// LoginServer 协议定义：自定义二进制报文（1 字节对齐，结构体直接 cast 收发）
// 报文 = packet_head{len, cmd}(4B) + 各业务字段
// 命令集：Minotor(向负载服务器上报本机内存)/Login(登录)/Register(注册)/Destory(注销)
// 通用坑（见各 Set*/Get* 函数）：str.copy(...,0) 超长会截断且末尾不补 '\0'，
// GetXxx 用 std::string(data()) 读到无终止符数组会越界——字段定长是协议假设，
// 客户端必须保证不超长。
// ============================================================
#pragma pack(push,1)
enum Cmd : uint16_t
{
    Minotor,          // 上报监控（注意拼写，应为 Monitor：负载服务器订阅本机内存/端口）
    ERROR,            // 错误
    Login,            // 登录：客户端带账号/密码/code 来认身份
    Register,         // 注册：新用户上报 name/code/账号/密码
    Destory,          // 注销：删除用户
};

// 登录/注册的结果码
enum ResultCode
{
	S_OK = 0,               // 成功
	SERVER_ERROR ,          // 服务器/用户不存在错误
	REQUEST_TIMEOUT ,       // 报文里的时间戳太旧（超过 TIMEOUT 秒），防重放/过期
	ALREADY_REDISTERED ,    // 已注册（code 被占用）
	USER_DISAPPEAR,
	ALREADY_LOGIN,          // 该用户已在线，拒绝重复登录
	VERFICATE_FAILED
};

// 报文头：len=整个报文字节数，cmd=命令类型。所有业务结构体都继承它。
struct packet_head {
    packet_head()
        :len(-1)
        , cmd(-1) {}        // 默认填 -1（无符号 16 位下是 65535），子类构造时覆盖
    uint16_t len;
    uint16_t cmd;
};

// 注册报文：客户端 → 服务器。code=用户唯一标识(20B)，name=昵称，count=账号，passwd=密码
struct UserRegister : public packet_head
{
    UserRegister():packet_head()
    {
        cmd = Register;
        len = sizeof(UserRegister);
    }
    void SetCode(const std::string& str)
    {
        str.copy(code.data(),code.size(),0);   // str 超 20B 截断；不足 20B 不补 '\0'（隐患同下）
    }
    std::string GetCode()
    {
        return std::string(code.data());       // 依赖数组里有 '\0'；若 SetCode 存了整 20B 会越界读
    }
    void SetName(const std::string& str)
    {
        str.copy(name.data(),name.size(),0);
    }
    std::string GetName()
    {
        return std::string(name.data());
    }
    void SetCount(const std::string& str)
    {
        str.copy(count.data(),count.size(),0);
    }
    std::string GetCount()
    {
        return std::string(count.data());
    }
    void SetPasswd(const std::string& str)
    {
        str.copy(passwd.data(),passwd.size(),0);
    }
    std::string GetPasswd()
    {
        return std::string(passwd.data());
    }
    std::array<char,20> code;
    std::array<char,20> name;
    std::array<char,12> count;
    std::array<char,33> passwd;      // 【2026-11-05 改】20→33：与 UserLogin 统一长度，
                                     // 否则注册存短串、登录传 33B，比对必然失败
    uint64_t timestamp;          // 客户端发报时间（秒），服务器用来判超时
};

// 登录报文：客户端 → 服务器。passwd 33B 是为 MD5 摘要（32 字符 + 终止符）
// 【注意】"Md5"目前只是长度约定：项目里没有任何 MD5 计算代码，存库的是原样明文。
// 要真正加密，需由某一方计算哈希（服务端存哈希、登录时比对哈希），见 LoginConnection 说明。
struct UserLogin : public packet_head
{
    UserLogin():packet_head()
    {
        cmd = Login;
        len = sizeof(UserLogin);
    }
    void SetCode(const std::string& str)
    {
        str.copy(code.data(),code.size(),0);
    }
    std::string GetCode()
    {
        return std::string(code.data());
    }
    void SetCount(const std::string& str)
    {
        str.copy(count.data(),count.size(),0);
    }
    std::string GetCount()
    {
        return std::string(count.data());
    }
    void SetPasswd(const std::string& str)
    {
        str.copy(passwd.data(),passwd.size(),0);
    }
    std::string GetPasswd()
    {
        return std::string(passwd.data());
    }
    std::array<char,20> code;
    std::array<char,12> count;
    std::array<char,33> passwd; //Md5
    uint64_t timestamp;
};

// 注册应答：服务器 → 客户端，只有结果码
struct RegisterResult : public packet_head
{
    RegisterResult():packet_head()
    {
        cmd = Register;
        len = sizeof(RegisterResult);
    }
	ResultCode resultCode;
};

// 登录应答：服务器 → 客户端。成功时附带"被分配的信令服务器 IP:port"（客户端接着连 SigServer）
struct LoginResult : public packet_head
{
    LoginResult() : packet_head()
    {
        cmd = Login;
        len = sizeof(LoginResult);
    }
    void SetIp(const std::string& str)
    {
        //str.copy(ctrSvrIp.data(),ctrSvrIp.size()+1,0);
        strncpy(ctrSvrIp.data(), str.c_str(), ctrSvrIp.size() - 1);  // 最多拷 15B
        ctrSvrIp.back() = '\0'; // 强制最后一个字符为终止符（本文件里唯一安全的 SetIp 写法）
    }
    std::string GetIp()
    {
        return std::string(ctrSvrIp.data());
    }
	ResultCode resultCode;
	uint16_t port;
	std::array<char, 16> ctrSvrIp;   // 信令服务器地址（写死下发 172.21.205.121:6539）
};

// 注销报文：客户端 → 服务器。按 code 删数据库记录
struct UserDestory : public packet_head
{
    UserDestory(): packet_head()
    {
        cmd = Destory;
        len = sizeof(UserDestory);
    }
    void SeCode(const std::string& str)   // 注意拼写：SeCode，应为 SetCode（调用方别写错）
    {
        str.copy(code.data(),code.size(),0);
    }
    std::string GetCode()
    {
        return std::string(code.data());
    }
    std::array<char,20> code;
};

// 监控上报报文：LoginServer(作为 TcpClient) → 负载均衡服务器。
// 携带本机内存占用率 + 本机 IP:port（供负载服务器做调度/心跳）
struct Monitor_body : public packet_head {
    Monitor_body()
        :packet_head()
    {
        cmd = Minotor;
        len = sizeof(Monitor_body);
        ip.fill('\0');
    }
    void SetIp(const std::string& str)
    {
        str.copy(ip.data(), ip.size(), 0);
    }
    std::string GetIp()
    {
        return std::string(ip.data());
    }
    uint8_t mem;                 // 内存占用百分比（0-100，取整）
    std::array<char, 16> ip;
	uint16_t port;
};
#pragma pack(pop)
#endif