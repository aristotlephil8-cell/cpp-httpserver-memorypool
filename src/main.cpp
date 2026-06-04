//
// HTTP 服务入口。两个 workload 路由用于 P2 压测:
//   /plaintext —— 低分配(返回 13B 文本,命中 SSO,几乎不分配)
//   /heavy     —— 分配重(组装大 JSON + 多响应头,故意制造大量小分配)
//
#include "http/HttpServer.h"
#include <muduo/base/Logging.h>

#include <cstdio>
#include <map>
#include <string>

#ifdef ENABLE_MEMORY_POOL_STATS
#include "MemoryPoolStats.h"   // 仅 stats 诊断构建需要
#include <csignal>
#include <iostream>
#endif

using namespace httpserver;

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
        // 低分配 workload:body 13B 命中 std::string SSO,基本不触发堆分配。
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setContentType("text/plain");
        resp->setBody("Hello, World!");
    }
    else if (req.path() == "/heavy")
    {
        // 分配重 workload:模拟真实业务"组装响应对象"的分配压力。
        // 每请求制造:~50 个 map 红黑树节点 + ~100 个 string + 一个不断增长的 JSON 串
        // + 8 个响应头(又是 map 节点 + string)。这些都是池的目标:高频、小、短命。
        std::map<std::string, std::string> obj;
        for (int i = 0; i < 50; ++i)
        {
            obj["field_" + std::to_string(i)] = "value_" + std::to_string(i * 7919);
        }
        std::string json = "{";
        bool first = true;
        for (auto& kv : obj)
        {
            if (!first) json += ",";
            first = false;
            json += "\"" + kv.first + "\":\"" + kv.second + "\"";
        }
        // 如果带了请求体(长 body POST),把它的长度也写进去,顺带触发请求体 string 分配。
        json += ",\"body_len\":" + std::to_string(req.body().size()) + "}";

        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setContentType("application/json");
        for (int i = 0; i < 8; ++i)
        {
            resp->addHeader("X-Custom-" + std::to_string(i),
                            "header-value-" + std::to_string(i));
        }
        resp->setBody(json);
    }
    else
    {
        resp->setStatusCode(HttpResponse::k404NotFound);
        resp->setStatusMessage("Not Found");
        resp->setContentType("text/plain");
        resp->setBody("404 Not Found");
    }
}

#ifdef ENABLE_MEMORY_POOL_STATS
// 诊断专用:用信号控制统计窗口,把"预热污染"挡在窗口外。
//   SIGUSR1 -> reset(清零,预热后调,只统计稳态)
//   SIGUSR2 -> print(打印到 stderr)
// 注意:在信号处理函数里做 print 严格说非 async-signal-safe,这是一次性诊断的妥协做法,
//       不进入任何计时性能测;计时版根本不定义 ENABLE_MEMORY_POOL_STATS。
extern "C" void onSigUsr1(int) { Avery_memoryPool::MemoryPoolStats::reset(); }
extern "C" void onSigUsr2(int) { Avery_memoryPool::MemoryPoolStats::print(std::cerr); }
#endif

int main(int argc, char* argv[])
{
    int port      = (argc > 1) ? atoi(argv[1]) : 8080;
    int threadNum = (argc > 2) ? atoi(argv[2]) : 4;

    // 压测时把日志压到 FATAL:wrk 收尾会强制 RST 关连接,muduo 对每个连接关闭打 ERROR
    // (Connection reset by peer),既刷屏又是真实 I/O 开销,会污染性能测量。
    const char* lv = getenv("HTTP_LOG");
    if (lv && std::string(lv) == "fatal")
        muduo::Logger::setLogLevel(muduo::Logger::FATAL);
    else
        muduo::Logger::setLogLevel(muduo::Logger::WARN);

    HttpServer server(port, "HttpServer");
    server.setThreadNum(threadNum);
    server.setHttpCallback(handleRequest);

#ifdef USE_MEMORY_POOL
    fprintf(stderr, "[allocator] CUSTOM MEMORY POOL (global new/delete intercepted)\n");
#else
    fprintf(stderr, "[allocator] baseline glibc malloc\n");
#endif

#ifdef ENABLE_MEMORY_POOL_STATS
    Avery_memoryPool::MemoryPoolStats::setEnabled(true);
    signal(SIGUSR1, onSigUsr1);
    signal(SIGUSR2, onSigUsr2);
    fprintf(stderr, "[stats] ENABLED (SIGUSR1=reset, SIGUSR2=print)\n");
#endif

    fprintf(stderr, "[server] listening on port %d, io threads = %d\n", port, threadNum);
    fflush(stderr);

    server.start();
    return 0;
}
