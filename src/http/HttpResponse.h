#pragma once
//
// HttpResponse —— 封装一条要发回客户端的 HTTP 响应。
//
// 业务回调(我们的 handler)只管往这里 setStatusCode / setBody / addHeader,
// 最后由 appendToBuffer 把它“序列化”成符合 HTTP/1.1 文本格式的字节,写进 muduo::Buffer。
//
#include <map>
#include <string>
#include <muduo/net/Buffer.h>

namespace httpserver
{

class HttpResponse
{
public:
    // 只列 P0 用得到的状态码;需要更多再加。
    enum HttpStatusCode
    {
        kUnknown,
        k200Ok                  = 200,
        k400BadRequest          = 400,
        k404NotFound            = 404,
        k500InternalServerError = 500,
    };

    // closeConnection: 这条响应发完后是否要关闭连接(短连接)。
    // 由 HttpServer 根据请求的 Connection 头/HTTP 版本算出来后传进来。
    explicit HttpResponse(bool closeConnection)
        : statusCode_(kUnknown), closeConnection_(closeConnection) {}

    void setStatusCode(HttpStatusCode code) { statusCode_ = code; }
    void setStatusMessage(std::string msg)  { statusMessage_ = std::move(msg); }

    void setCloseConnection(bool on) { closeConnection_ = on; }
    bool closeConnection() const     { return closeConnection_; }

    void setContentType(const std::string& contentType) { addHeader("Content-Type", contentType); }
    void addHeader(const std::string& key, const std::string& value) { headers_[key] = value; }

    void setBody(const std::string& body) { body_ = body; }

    // 把整个响应拼成 HTTP 文本写进 Buffer。
    // 格式:状态行 CRLF, 各 header CRLF, 空行 CRLF, body
    void appendToBuffer(muduo::net::Buffer* output) const
    {
        char buf[64];
        snprintf(buf, sizeof buf, "HTTP/1.1 %d ", statusCode_);
        output->append(buf);
        output->append(statusMessage_);
        output->append("\r\n");

        // Content-Length 必须发,客户端靠它知道 body 多长;
        // 同时根据 closeConnection_ 决定 Connection 头。
        if (closeConnection_)
        {
            output->append("Connection: close\r\n");
        }
        else
        {
            snprintf(buf, sizeof buf, "Content-Length: %zd\r\n", body_.size());
            output->append(buf);
            output->append("Connection: Keep-Alive\r\n");
        }

        for (const auto& header : headers_)
        {
            output->append(header.first);
            output->append(": ");
            output->append(header.second);
            output->append("\r\n");
        }

        output->append("\r\n");   // header 与 body 之间的空行
        output->append(body_);
    }

private:
    HttpStatusCode                      statusCode_;
    std::string                         statusMessage_;
    std::map<std::string, std::string>  headers_;
    std::string                         body_;
    bool                                closeConnection_;
};

} // namespace httpserver
