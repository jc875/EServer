#include <stdio.h>
#include "../EdoyunNet/EventLoop.h"
#include "LoginServer.h"
//#include <jsoncpp/json/json.h>

// LoginServer 入口：事件循环(单线程) → 创建 LoginServer → 绑定端口启动
int main()
{
    // Json::Value root;    // （注释掉的 jsoncpp 测试代码，当前未用）
    // root["name"] = "John";
    // root["age"] = 30;
    // root["married"] = true;
    // Json::Reader gh;
    // return 0;
    int count = std::thread::hardware_concurrency();
    EventLoop loop(1);
    auto login_server = LoginServer::Create(&loop);
    //if(!login_server->Start("192.168.31.30",6539))
    // 监听 IP 写死 172.21.205.121——本机/WSL 启动会 bind 失败，需改 "0.0.0.0"
    if(!login_server->Start("172.21.205.121",9867))   // 登录服务器端口 9867
    {
        printf("LoginServer server failed\n");
    }
    printf("LoginServer server success\n");
    getchar();    // 阻塞等待
    return 0;
    
}