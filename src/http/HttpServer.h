#pragma once
//
// HttpServer —— 把 muduo 的网络层(TcpServer)包成一个“只懂 HTTP 的服务”。
//
// 【Muduo 网络层速览:面试第一刀,务必能口述这条链路】
//
//   EventLoop      “一个线程一个循环”。循环里反复做一件事:调 epoll_wait 等待 fd 事件,
//                  事件来了就分发给对应的回调。这就是 Reactor 模式的“反应堆”。
//                  one loop per thread:每个 IO 线程独占一个 EventLoop,互不加锁。
//
//   Poller/epoll   EventLoop 底层真正调用 epoll 的人,负责“哪些 fd 有事件”。muduo 默认用
//                  epoll 的 LT(水平触发)模式:只要可读就一直通知,编程更简单、不易丢数据。
//
//   Channel        一个 fd 的“事件登记表”。它不拥有 fd,只记录“我关心读/写事件,
//                  事件来了请调这几个回调”。EventLoop 通过 Poller 拿到就绪事件后,
//                  找到对应 Channel,调它的回调。
//
//   TcpServer      高层封装:内部有一个 Acceptor(监听 listen fd),新连接到来时
//                  accept,并为每条连接创建一个 TcpConnection,再把它分给某个 IO 线程的 EventLoop。
//
//   TcpConnection  代表“一条已建立的 TCP 连接”。它带:
//                    - 一个内核 socket fd 和对应的 Channel
//                    - 一个输入 Buffer、一个输出 Buffer(应用层缓冲,解决 TCP 字节流问题)
//                    - 一个“上下文槽”context(boost::any),我们用它存每条连接的 HttpContext
//
//   回调三件套     我们写服务,本质就是“填回调”:
//                    ConnectionCallback —— 连接建立/断开时调
//                    MessageCallback    —— 该连接收到数据时调(数据已在输入 Buffer 里)
//                    业务回调 HttpCallback —— 我们自定义的:解析出完整请求后调,生成响应
//
// 数据流(P0 必须能背):
//   客户端 connect
//     -> Acceptor accept,TcpServer 建 TcpConnection,分给某 IO 线程的 EventLoop
//     -> onConnection():给这条连接装一个空的 HttpContext
//   客户端发字节
//     -> EventLoop(epoll) 发现该连接可读 -> 读进输入 Buffer -> onMessage()
//     -> 取出该连接的 HttpContext,parseRequest() 解析(可能跨多次 onMessage,见 HttpContext)
//     -> gotAll() 为真时 onRequest():算出短/长连接,调 httpCallback_ 生成 HttpResponse
//     -> response.appendToBuffer() 写进输出 Buffer -> conn->send() 发回客户端
//
#include <functional>
#include <string>

#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include <muduo/base/Logging.h>

#include "HttpRequest.h"
#include "HttpResponse.h"

namespace httpserver
{

class HttpServer : muduo::noncopyable
{
public:
    // 业务回调签名:给我一个解析好的请求,你往 response 里填内容。
    using HttpCallback = std::function<void(const HttpRequest&, HttpResponse*)>;

    HttpServer(int port, const std::string& name,
               muduo::net::TcpServer::Option option = muduo::net::TcpServer::kNoReusePort);

    // 设置 IO 线程数。0 = 所有连接都在主线程(单 Reactor);
    // n>0 = 主线程只管 accept,n 个子线程各跑一个 EventLoop 处理连接(主从 Reactor)。
    // 【这点 P1 内存池的 thread_local 线程安全会直接用到,先记住:一条连接固定属于一个 IO 线程。】
    void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }

    // 注册业务回调。P0 用一个回调 + 内部 if 判 path 做“最小路由”,不做路由表。
    void setHttpCallback(const HttpCallback& cb) { httpCallback_ = cb; }

    void start();

private:
    void onConnection(const muduo::net::TcpConnectionPtr& conn);
    void onMessage(const muduo::net::TcpConnectionPtr& conn,
                   muduo::net::Buffer* buf,
                   muduo::Timestamp receiveTime);
    void onRequest(const muduo::net::TcpConnectionPtr& conn, const HttpRequest& req);

    muduo::net::EventLoop  mainLoop_;   // 主 Reactor 的 EventLoop(必须先于 server_ 声明)
    muduo::net::TcpServer  server_;
    HttpCallback           httpCallback_;
};

} // namespace httpserver
