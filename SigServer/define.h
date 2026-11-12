#include <cstdint>
#include <array>
#include <string>

// ============================================================
// SigServer 自定义二进制协议定义（信令协议）
// 与 RTMP 无关：这是 SigServer 自己定义的"控制端<->服务器<->被控端"
// 之间的私有报文格式。报文 = packet_head(4B) + 各种 body。
// 注意 #pragma pack(1)：全部结构体 1 字节对齐，按字节序直接
// 把结构体指针 cast 到缓冲区上读写（memcpy 风格，无序列化开销）。
// ============================================================

//需要1字节对齐
#pragma pack(push,1)


//cmd命令 —— 每种报文用 cmd 字段区分
enum Cmd : uint16_t
{
    JOIN = 5,          // 注册"房间号"(code)：客户端上线报家门，建立身份
    OBTAINSTREAM,      // 获取流：控制端请求拉某个房间的流（要先知道对方在不在、忙不忙）
    CREATESTREAM,      // 创建流：服务器通知被控端"有人要看你的流了，去推流"
    PLAYSTREAM,        // 播放流：把被控端的流地址下发给控制端，让它去拉
    DELETESTREAM,      // 删除流：推流结束/断开时通知关联者
    MOUSE,             // 鼠标按下/松开
    MOUSEMOVE,         // 鼠标移动（x/y 用比值表示）
    KEY,               // 键盘按键
    WHEEL,             // 滚轮
};

//应答结果码 —— 各种回复报文里的 result 字段取值
enum ResultCode
{
    SUCCESSFUL,        // 成功
    ERROR,             // 失败
    REQUEST_TIMEOUT,   // 请求超时
    ALREADY_REDISTERED,// 重复注册（房间号已存在）
    USER_DISAPPEAR,    // 用户不存在
    ALREADY_LOGIN,     // 已登录
    VERFICATE_FAILED,  // 验证失败
};

//客户端状态 —— SigConnection 状态机（角色）
enum RoleState
{
    IDLE, //说明客户端已经创建房间，而且没有去拉流也没有推流（空闲，可被约）
    NONE, //S=说明当前客户端没有创建房间（刚连上还没 JOIN）
    CLOSE,//客户端断开连接
    PULLER,//客户端进入拉流模式（控制端：要看别人的画面）
    PUSHER,//客户端开始推流（被控端：把自己的画面给别人看）
};

//所有报文的公共头：len=整包长度(含头)，cmd=命令类型
struct packet_head
{
    packet_head():len(-1),cmd(-1){}
    uint16_t len;
    uint16_t cmd;
};

//包体
//创建房间：客户端 JOIN 时上报 10 字节的房间号(code)
struct Join_body : public packet_head
{
    Join_body() : packet_head()
    {
        cmd = JOIN;
        len = sizeof(Join_body);
        id.fill('\0');
    }
    void SetId(const std::string& str)
    {
        str.copy(id.data(),id.size(),0);   // 最多拷 10 字节；若 str 超长会被截断且末尾无 '\0'（隐患）
    }
    std::string GetId()
    {
        return std::string(id.data());      // 依赖 id 里的 '\0' 结尾
    }
    std::array<char,10> id;   // 房间号（如设备编码），不足 10 字节用 '\0' 填充
};
//创建房间应答：告诉客户端 JOIN 成功/失败
struct JoinReply_body : public packet_head
{
    JoinReply_body() : packet_head()
    {
        cmd = JOIN;
        len = sizeof(JoinReply_body);
        result = ERROR;    // 默认失败，成功时显式 SetCode(SUCCESSFUL)
    }
    //设置结果
    void SetCode(const ResultCode code)
    {
        result = code;
    }
    ResultCode result;
};
//获取流：控制端想拉"房间号 id"的流，向服务器打听目标状态
struct ObtainStream_body : public packet_head
{
    ObtainStream_body() : packet_head()
    {
        cmd = OBTAINSTREAM;
        len = sizeof(ObtainStream_body);
        id.fill('\0');
    }
    void SetId(const std::string& str)
    {
        str.copy(id.data(),id.size(),0);
    }
    std::string GetId()
    {
        return std::string(id.data());
    }
    std::array<char,10> id;   // 目标被控端的房间号
};
//获取流应答：服务器回目标状态/结果
struct ObtainStreamReply_body : public packet_head
{
    ObtainStreamReply_body() : packet_head()
    {
        cmd = OBTAINSTREAM;
        len = sizeof(ObtainStreamReply_body);
        result = ERROR;
    }
    void SetCode(const ResultCode code)
    {
        result = code;
    }
    ResultCode result;
};
//创建流：服务器 → 被控端，"有人要看你的流，请开始推流"（空包，无内容）
struct CreateStream_body : public packet_head
{
    CreateStream_body() : packet_head()
    {
        cmd = CREATESTREAM;
        len = sizeof(CreateStream_body);
    }
};
//创建流应答 流地址和结果：被控端推流后回传自己的流地址（70 字节），服务器再广播给控制端
struct CreateStreamReply_body : public packet_head
{
    CreateStreamReply_body() : packet_head()
    {
        cmd = CREATESTREAM;
        len = sizeof(CreateStreamReply_body);
        result = ERROR;
        streamAddres.fill('\0');
    }
    void SetstreamAddres(const std::string& str)
    {
        str.copy(streamAddres.data(),streamAddres.size(),0);
    }
    std::string GetstreamAddres()
    {
        return std::string(streamAddres.data());
    }
    void SetCode(const ResultCode code)
    {
        result = code;
    }
    ResultCode result;
    std::array<char,70> streamAddres;   // 流地址（如 rtmp://.../live/xxx）
};

//播放流 提供播放流地址：服务器 → 控制端，"目标已经在推流，去这个地址拉流"
struct PlayStream_body : public packet_head
{
    PlayStream_body() : packet_head()
    {
        cmd = PLAYSTREAM;
        len = sizeof(PlayStream_body);
        result = ERROR;
        streamAddres.fill('\0');
    }
    void SetstreamAddres(const std::string& str)
    {
        str.copy(streamAddres.data(),streamAddres.size(),0);
    }
    std::string GetstreamAddres()
    {
        return std::string(streamAddres.data());
    }
    void SetCode(const ResultCode code)
    {
        result = code;
    }
    ResultCode result;
    std::array<char,70> streamAddres;
};

// 播放流应答（本服务器代码未使用，仅协议占位）
struct PlayStreamReplay_body : public packet_head
{
    PlayStreamReplay_body() : packet_head()
    {
        cmd = PLAYSTREAM;
        len = sizeof(PlayStreamReplay_body);
        result = ERROR;
    }
    void SetCode(const ResultCode code)
    {
        result = code;
    }
    ResultCode result;
};

//删除流：断开/结束推流时广播给所有关心者；streamCount=当前还有多少拉流者
struct DeleteStream_body : public packet_head
{
    DeleteStream_body():packet_head()
    {
        cmd = DELETESTREAM;
        streamCount = -1;
        len = sizeof(DeleteStream_body);
    }
    void SetStreamCount(const int count)
    {
        streamCount = count;
    }
    int streamCount;//推流的时候，如果发现拉流数量为0,我们就需要停止推流，如果流数量不为0，就说明还有客户端连接，不能停止推流
};

//鼠标键盘信息 —— 以下 4 种是"操作转发"报文：控制端的输入事件 → 服务器 → 被控端
//鼠标按键类型
enum MouseType : uint8_t
{
    NoButton = 0,
    LeftButton = 1,
    RightButton = 2,
    MiddleButton = 4,
    XButton1 = 8,
    XButton2 = 16,
};

//鼠标键盘按下还是松开
enum MouseKeyType : uint8_t
{
    PRESS,
    RELESE,
};

//键盘消息：键值 + 按下/松开
struct Key_body : public packet_head
{
    Key_body():packet_head(){
        cmd = KEY;
        len = sizeof(Key_body);
    }
    //键值和类型
    uint16_t key;
    MouseKeyType type;
};

//滚轮消息
struct Wheel_body : public packet_head
{
    Wheel_body():packet_head(){
        cmd = WHEEL;
        len = sizeof(Wheel_body);
    }
    //值
    uint8_t wheel;
};

//鼠标移动：用 4 个 8bit 比值表示位移（省字节，不用 float）
struct MouseMove_body : public packet_head
{
    MouseMove_body():packet_head(){
        cmd = MOUSEMOVE;
        len = sizeof(MouseMove_body);
    }
    //需要x,y比值
    uint8_t xl_ratio;
    uint8_t xr_ratio;
    uint8_t yl_ratio;
    uint8_t yr_ratio;
};

//鼠标点击：按键类型 + 按下/松开
struct Mouse_body : public packet_head
{
    Mouse_body():packet_head()
    {
        cmd = MOUSE;
        len = sizeof(Mouse_body);
    }
    MouseKeyType type;
    MouseType mouseType;
};

#pragma pack(pop)