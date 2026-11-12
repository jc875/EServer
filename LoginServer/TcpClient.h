#include "define.h"

// ============================================================
// TcpClient：LoginServer 里"作为客户端连负载服务器"的简易 TCP 客户端
// 用途：每秒读取本机内存占用（/proc/meminfo 的 MemAvailable），组 Monitor_body
//       （mem + 本机 IP:port）发给负载服务器 172.21.205.121:8523，
//       供负载服务器做调度决策（哪台登录服务器空闲）。
// 注意：这是个同步/阻塞式最小实现（非事件驱动），定时器里直接 send，量大时慎用。
// ============================================================
class TcpClient
{
public:
	TcpClient();
	void Create();                     // 打开 /proc/meminfo + 创建 socket + 填本机信息
	bool Connect(std::string ip, uint16_t port);  // 连接负载服务器
	void Close();
	~TcpClient();
    void getMonitorInfo();             // 触发一次上报（读内存 → send）
protected:
    void get_mem_usage();              // 读 MemAvailable 算占用率并发送
	void Send(uint8_t* data, uint32_t size);   // 循环 send 直到发完
private:
	FILE* file_;                       // /proc/meminfo 句柄
	bool isConnect_;
	int sockfd_;
	struct sysinfo info_;              // 系统信息（totalram 用）
	Monitor_body Monitor_info_;        // 待发送的监控报文
};