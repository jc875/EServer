#include "../EdoyunNet/EventLoop.h"
#include "SigServer.h"

// SigServer 入口：创建事件循环 → 创建 SigServer → 绑定监听端口启动
int main()
{
    int count = std::thread::hardware_concurrency();
    EventLoop loop(1);                       // 单线程事件循环（信令量小，1 个够）
    auto sig_server = SigServer::Create(&loop);
    // 监听 IP 写死为 172.21.205.121（旧环境地址）——本机/WSL 启动会 bind 失败，
    // 需要改成 "0.0.0.0"（参考 RtmpServer\main.cpp 的改法）
    if(!sig_server->Start("172.21.205.121",6539))   // 信令端口 6539
    {
        printf("sig server failed\n");
    }
    printf("sig server success\n");
    getchar();                               // 阻塞等待，方便手动调试
    return 0;
}

