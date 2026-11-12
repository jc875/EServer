#include "TcpClient.h"
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

TcpClient::TcpClient()
    :sockfd_(-1)
    ,file_(nullptr)
    ,isConnect_(false)
{
}

// 初始化：打开内存信息文件、取系统信息、建 socket、填死本机 IP:port（应可配置）
void TcpClient::Create()
{
    //获取内存
    file_ = fopen("/proc/meminfo","r");
    if(!file_)
    {
        printf("open file fialed\n");
        return;
    }
    memset(&info_,0,sizeof(info_));

    //初始化这个info
    sysinfo(&info_);            // totalram 等系统总量
    //创建socket
    sockfd_ = ::socket(AF_INET,SOCK_STREAM,0);
    Monitor_info_.SetIp("172.21.205.121");   // 上报的本机 IP（写死，应可配置）
    Monitor_info_.port = 9867;               // 本机(LoginServer)监听端口
}

// 连接负载服务器（阻塞 connect）
bool TcpClient::Connect(std::string ip, uint16_t port)
{
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);            // 主机序转网络序（大端）
    addr.sin_addr.s_addr = inet_addr(ip.c_str());

    int len = sizeof(sockaddr_in);

    //连接
    if(::connect(sockfd_,(sockaddr*)&addr,len) == -1)
    {
        printf("connect error\n");
        return false;
    }
    isConnect_ = true;
    return true;
}

void TcpClient::Close()
{
    isConnect_ = false;
    fclose(file_);           // 关内存信息文件
    if(sockfd_)
    {
        ::close(sockfd_);    // 关 socket
    }
}

TcpClient::~TcpClient()
{
    Close();
}

// 上报入口：读内存 → 发送
void TcpClient::getMonitorInfo()
{
    get_mem_usage();
}

// 从 /proc/meminfo 读 MemAvailable，算占用率（(total-avail)/total），组包发送
void TcpClient::get_mem_usage()
{
    rewind(file_);

    size_t bytes_used = 0;
    ssize_t read = 0;
    char* line = nullptr;
    int avimem = 0;

    while ((read = getline(&line, &bytes_used, file_)) != -1)
    {
        if (strstr(line, "MemAvailable") != nullptr)
        {
            sscanf(line, "%*s%d%*s", &avimem);
            break;
        }
    }

    int total = info_.totalram / 1024;
    double mem = (total - avimem) * 100.0 / total;
    Monitor_info_.mem = static_cast<uint8_t>(mem);

    free(line);

    Send(
        reinterpret_cast<uint8_t*>(&Monitor_info_),
        Monitor_info_.len
    );
}
// 循环发送直到发完（半包重发）；失败即中断
void TcpClient::Send(uint8_t *data, uint32_t size)
{
    int len = 0;
    int index = 0;
    while(index != size)
    {
        len = ::send(sockfd_,data + index ,size - index,0);
        if(len < 0)
        {
            break;
        }
        index += len;
    }
}
