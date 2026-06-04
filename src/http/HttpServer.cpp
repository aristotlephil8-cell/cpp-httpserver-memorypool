#include "HttpServer.h"
#include "HttpContext.h"

#include <boost/any.hpp>   // muduo 用 boost::any 做 TcpConnection 的“上下文槽”

using namespace httpserver;

HttpServer::HttpServer(int port, const std::string& name,
                       muduo::net::TcpServer::Option option)
    // server_ 需要:它要跑在哪个 EventLoop 上、监听地址、名字。
    : server_(&mainLoop_, muduo::net::InetAddress(port), name, option)
{
    // 把 muduo 的两个底层回调接到我们的成员函数上。
    // server_ 一旦有连接事件/数据事件,就会回调到这里。
    server_.setConnectionCallback(
        std::bind(&HttpServer::onConnection, this, std::placeholders::_1));
    server_.setMessageCallback(
        std::bind(&HttpServer::onMessage, this,
                  std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
}

void HttpServer::start()
{
    LOG_WARN << "HttpServer starts listening on " << server_.ipPort();
    server_.start();   // 开始监听 + accept
    mainLoop_.loop();  // 进入事件循环,阻塞在这里,不断 epoll_wait 分发事件
}

// 连接建立 / 断开都会回调这里。
void HttpServer::onConnection(const muduo::net::TcpConnectionPtr& conn)
{
    if (conn->connected())
    {
        // 关键:给这条新连接装一个空的 HttpContext,存进 muduo 提供的“上下文槽”。
        // 这样每条连接都有自己独立的解析状态,互不干扰(也是处理粘包/半包的载体)。
        conn->setContext(HttpContext());
    }
    // 断开时无需手动清理:context 随 TcpConnection 析构一起销毁。
}

// 该连接收到数据时回调。此刻数据已经被 muduo 读进 buf(输入 Buffer)。
void HttpServer::onMessage(const muduo::net::TcpConnectionPtr& conn,
                           muduo::net::Buffer* buf,
                           muduo::Timestamp receiveTime)
{
    // 取出这条连接专属的 HttpContext(就是 onConnection 里塞进去的那个)。
    HttpContext* context = boost::any_cast<HttpContext>(conn->getMutableContext());

    // 把 Buffer 交给状态机解析。注意:这一次不一定能解析出完整请求(半包),
    // 解析器会自己保留进度;parseRequest 返回 false 代表报文格式非法。
    if (!context->parseRequest(buf, receiveTime))
    {
        conn->send("HTTP/1.1 400 Bad Request\r\n\r\n");
        conn->shutdown();   // 半关闭:不再发送,触发对端感知
        return;
    }

    // 只有“收齐一整条请求”时才生成响应。
    if (context->gotAll())
    {
        onRequest(conn, context->request());
        context->reset();   // 复位,准备在同一条 keep-alive 连接上解析下一条请求
    }
}

void HttpServer::onRequest(const muduo::net::TcpConnectionPtr& conn, const HttpRequest& req)
{
    // 根据请求头算这条响应发完是否要关连接:
    //   显式 Connection: close,或 HTTP/1.0 且没有 Keep-Alive，都视为短连接。
    const std::string& connection = req.getHeader("Connection");
    bool close = (connection == "close") ||
                 (req.getVersion() == "HTTP/1.0" && connection != "Keep-Alive");

    HttpResponse response(close);
    if (httpCallback_)
    {
        httpCallback_(req, &response);   // 调业务回调,填好响应内容
    }

    // 把响应序列化进输出 Buffer,交给 muduo 发送。
    muduo::net::Buffer buf;
    response.appendToBuffer(&buf);
    conn->send(&buf);

    if (response.closeConnection())
    {
        conn->shutdown();
    }
}
