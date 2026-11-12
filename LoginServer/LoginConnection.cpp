#include "LoginConnection.h"
#include "ORMManager.h"
#include <chrono>
#define TIMEOUT 60   // 报文有效期（秒）：超过就判 REQUEST_TIMEOUT，防重放旧报文

// 信令服务器地址（登录成功后下发给客户端）：写死本机地址，部署时按环境改
static const char*   kSigServerIp   = "172.21.205.121";
static const uint16_t kSigServerPort = 6539;

// 取当前秒级时间戳
static long NowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// 构造函数：绑定读回调 + 关闭回调（关闭回调用于把用户置离线）
LoginConnection::LoginConnection(TaskScheduler *scheduler, int socket)
    :TcpConnection(scheduler,socket)
{
    //设置回调函数，处理读数据
    this->SetReadCallback([this](std::shared_ptr<TcpConnection> conn,BufferReader& buffer){
        return this->OnRead(buffer);
    });

    //设置关闭回调：客户端断开后把在线状态复位，否则该账号下次登录会被判 ALREADY_LOGIN
    this->SetCloseCallback([this](std::shared_ptr<TcpConnection> conn){
        this->DisConnected();
    });
}

LoginConnection::~LoginConnection()
{
    Clear();
}

// 框架读回调：有数据就整包解析
bool LoginConnection::OnRead(BufferReader &buffer)
{
    if(buffer.ReadableBytes() > 0)
    {
        HandleMessage(buffer);
    }
    return true;
}

// 报文解析入口：凑齐一个完整包（先 4B 头、再按 len 等 body），按 cmd 分派
void LoginConnection::HandleMessage(BufferReader &buffer)
{
    if(buffer.ReadableBytes() < sizeof(packet_head))
    {
        return ;    // 头都不够，等下次读
    }
    packet_head* head = (packet_head*)buffer.Peek();
    if(buffer.ReadableBytes() < head->len)
    {
        return ;    // body 未到齐，继续等
    }
    switch (head->cmd)
    {
    case Login:                      // 登录
        HandleLogin((packet_head*)buffer.Peek());
        break;
    case Register:                   // 注册
        HandleRegister((packet_head*)buffer.Peek());
        break;
    case Destory:                    // 注销
        HandleDestory((packet_head*)buffer.Peek());
        break;
    default:
        break;                       // 未知命令忽略
    }
    //更新buffer：消费掉这个包，留给下一个包
    buffer.Retrieve(head->len);
}

// 报文是否过期：|现在 - 报文中时间戳| > 60 秒。
// 注意用了 abs：未来 60 秒外也算超时（容忍时钟偏差，但严格说只应判"过去太久"）
bool LoginConnection::IsTimeout(uint64_t timestamp) //true就是超时
{
    //获取当前时间
    auto now = std::chrono::system_clock::now();
    auto nowTimestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    //计算差值
    int64_t time = nowTimestamp - timestamp;
    return abs(time) > TIMEOUT;
}

// 清理（当前为空实现；会话断开时的业务清理在 DisConnected 里做）
void LoginConnection::Clear()
{
}

// 断开回调：如果本连接登录过（code_ 非空），把数据库里的在线状态置 0。
// 这样账号可以再次登录，不会永久卡在 ALREADY_LOGIN。
void LoginConnection::DisConnected()
{
    printf("login connection disconnected, code=%s\n", code_.c_str());
    if(code_.empty())
        return;                                    // 没登录过，无需处理
    ORMManager::GetInstance()->SetOnline(code_, 0, NowSeconds(), "");
    code_.clear();
}

// ============================================================
// 注册处理：查 code 是否已注册 → 未注册就插入一行(online=0)，已注册回 ALREADY_REDISTERED
// 注意：这里不做密码哈希，存的就是客户端传来的字符串（见文件末尾"密码安全"说明）。
// ============================================================
void LoginConnection::HandleRegister(const packet_head *data)
{
    RegisterResult reply;                    // 回给客户端的应答（只有结果码）
    UserRegister* login = (UserRegister*)data;

    if(IsTimeout(login->timestamp))          // 报文过期
    {
        reply.resultCode = REQUEST_TIMEOUT;
        this->Send((const char*)&reply,reply.len);
        return;
    }

    ClientRow exist;
    if(ORMManager::GetInstance()->QueryClient(login->GetCode(), exist))  // 已存在
    {
        reply.resultCode = ALREADY_REDISTERED;
        this->Send((const char*)&reply,reply.len);
        return;
    }

    // 组装新用户行：online=0（未登录），记录创建时间与分配的信令服务器
    ClientRow row;
    row.name           = login->GetName();
    row.acount         = login->GetCount();
    row.passwd         = login->GetPasswd();
    row.code           = login->GetCode();
    row.online         = 0;
    row.recently_login = NowSeconds();
    row.svr_mount      = kSigServerIp;

    if(!ORMManager::GetInstance()->InsertClient(row))
    {
        reply.resultCode = SERVER_ERROR;     // 写库失败要如实返回错误，不能假装成功
        this->Send((const char*)&reply,reply.len);
        return;
    }
    reply.resultCode = S_OK;
    this->Send((const char*)&reply,reply.len);
}

// ============================================================
// 登录处理（改造后 6 步）：
//   ① 报文超时        → REQUEST_TIMEOUT
//   ② 用户不存在      → USER_DISAPPEAR
//   ③ 密码不匹配      → VERFICATE_FAILED
//   ④ 已在线          → ALREADY_LOGIN
//   ⑤ UPDATE 置在线失败 → SERVER_ERROR
//   ⑥ 成功 → 回 S_OK + 信令服务器地址，并记住 code_ 供断线复位
//
// 【密码安全，如实说明】
//   目前数据库存的是"客户端传来的原样字符串"，服务器端没有任何哈希。
//   define.h 注释写着 Md5，但项目里没有一行 MD5 计算代码——这只是一个"约定了长度"的
//   空标签。要真正加密需要：注册时服务器算 MD5/哈希存库、登录时比对哈希；
//   更安全的做法是 bcrypt/scrypt 加盐，或走 TLS 保护传输。
// ============================================================
void LoginConnection::HandleLogin(const packet_head *data)
{
    LoginResult reply;                       // 应答（带结果码 + 信令服务器 IP/port）
    UserLogin* login = (UserLogin*)data;

    if(IsTimeout(login->timestamp))          // ① 报文过期
    {
        reply.resultCode = REQUEST_TIMEOUT;
        this->Send((const char*)&reply,reply.len);
        return;
    }

    ClientRow row;                           // ② 查用户（拷出来的副本，不是悬垂 MYSQL_ROW）
    if(!ORMManager::GetInstance()->QueryClient(login->GetCode(), row))
    {
        reply.resultCode = USER_DISAPPEAR;   // 用"用户不存在"语义码，而不是笼统的 SERVER_ERROR
        this->Send((const char*)&reply,reply.len);
        return;
    }

    // ③ 密码校验：字符串直接比对（前提是注册与登录的密码格式一致，见 define.h 长度统一）
    if(row.passwd != login->GetPasswd())
    {
        printf("verify failed, code=%s\n", login->GetCode().c_str());
        reply.resultCode = VERFICATE_FAILED;
        this->Send((const char*)&reply,reply.len);
        return;
    }

    // ④ 已在线？拒绝重复登录（也可以改成"踢掉旧会话"，取决于产品策略）
    if(row.online)
    {
        printf("online \n");
        reply.resultCode = ALREADY_LOGIN;
        this->Send((const char*)&reply,reply.len);
        return;
    }

    // ⑤ 置在线：UPDATE ... WHERE USER_CODE=...，失败就不放行
    if(!ORMManager::GetInstance()->SetOnline(row.code, 1, NowSeconds(), kSigServerIp))
    {
        reply.resultCode = SERVER_ERROR;
        this->Send((const char*)&reply,reply.len);
        return;
    }

    // ⑥ 成功：记住 code_，下发信令服务器地址
    code_ = row.code;
    reply.resultCode = S_OK;
    reply.SetIp(kSigServerIp);
    reply.port       = kSigServerPort;
    printf("login ok, code=%s -> sig %s:%d\n",
           code_.c_str(), reply.GetIp().c_str(), reply.port);
    this->Send((const char*)&reply,reply.len);
}

// 注销处理：按 code 删除数据库记录
void LoginConnection::HandleDestory(const packet_head *data)
{
    UserDestory* destory = (UserDestory*)data;
    ORMManager::GetInstance()->DeleteClient(destory->GetCode());
    code_.clear();                           // 已删号，本连接不再需要断线复位
}
