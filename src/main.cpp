//
// P0 最小 HTTP 服务入口。
//
// 目标:curl localhost:8080/ 返回 200 + JSON;其它路径返回 404。
// 这里用“一个回调 + if 判 path”做最小路由,刻意不做路由表/正则/动态路由
// (那是 Kama 的重模块,超出本项目范围)。
//
#include "http/HttpServer.h"
#include <muduo/base/Logging.h>

using namespace httpserver;

// 业务回调:看 path 决定返回什么。
void handleRequest(const HttpRequest& req, HttpResponse* resp)
{
    if (req.path() == "/")
    {
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setContentType("application/json");
        resp->setBody(R"({"msg":"hello from memory-pool http server"})");
    }
    else if (req.path() == "/plaintext")
    {
        // 一个纯文本端点,P2 压测打这个,避免 JSON 字符串干扰对比。
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setContentType("text/plain");
        resp->setBody("Hello, World!");
    }
    else
    {
        resp->setStatusCode(HttpResponse::k404NotFound);
        resp->setStatusMessage("Not Found");
        resp->setContentType("text/plain");
        resp->setBody("404 Not Found");
    }
}

int main(int argc, char* argv[])
{
    int port        = (argc > 1) ? atoi(argv[1]) : 8080;
    int threadNum   = (argc > 2) ? atoi(argv[2]) : 4;   // IO 线程数,默认 4

    // muduo 日志默认 INFO 太吵,压测时设成 WARN。
    muduo::Logger::setLogLevel(muduo::Logger::WARN);

    HttpServer server(port, "P0-HttpServer");
    server.setThreadNum(threadNum);
    server.setHttpCallback(handleRequest);

    // 用无缓冲的 stderr 打横幅,确保即使被 kill 也能看到当前用的是哪种分配器(A/B 确认)。
#ifdef USE_MEMORY_POOL
    fprintf(stderr, "[allocator] CUSTOM MEMORY POOL (global new/delete intercepted)\n");
#else
    fprintf(stderr, "[allocator] baseline glibc malloc\n");
#endif
    fprintf(stderr, "[server] listening on port %d, io threads = %d\n", port, threadNum);
    fflush(stderr);
    server.start();
    return 0;
}
