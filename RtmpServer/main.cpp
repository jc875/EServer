#include <iostream>
#include "../EdoyunNet/EventLoop.h"
#include "RtmpServer.h"

int main()
{
    int count = std::thread::hardware_concurrency();
    EventLoop loop(2);
    // 1. 创建事件循环；2. 创建 RtmpServer；3. 设置服务器级 RTMP 默认参数。
    // RtmpServer::Create 内部会调用其 private 构造函数和 TcpServer(eventloop)。
    auto rtmp_server = RtmpServer::Create(&loop);
    rtmp_server->SetChunkSize(60000);
    rtmp_server->SetEventCallback([](std::string type,std::string stream_path){
        printf("[Event]%s,stream_path%s\n",type.c_str(),stream_path.c_str());
    });
    // Start 才真正建立监听 Socket、注册监听事件；之前只是创建了对象。
    // 监听 IP 用 0.0.0.0（任意本机网卡），否则写死旧环境的 IP 会 bind 失败（2026-11-01 启动测试时从 172.21.205.121 改来）。
    if(!rtmp_server->Start("0.0.0.0",1935))
    {
        printf("rtmp server failed\n");
    }
    printf("rtmp server success\n");
    //getchar();
    	while (1) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
    return 0;
}