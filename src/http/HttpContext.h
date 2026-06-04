#pragma once
//
// HttpContext —— “一条连接的解析状态”。
//
// 【为什么需要它?这是面试必考点,先建立直觉】
// TCP 是“字节流”,不是“消息流”。内核不保证“你发一个 HTTP 请求,我就恰好收到一个”。
// 一次 onMessage 回调里,muduo 的 Buffer 中可能是:
//    ① 半个请求(请求头还没收全)            —— 叫“半包 / 不完整包”
//    ② 正好一个完整请求
//    ③ 一个半 / 两个请求挤在一起             —— 叫“粘包”
// 所以必须有一个“跨多次 onMessage 还能接着上次进度解析”的状态机,这就是 HttpContext。
// muduo 给每个 TcpConnection 提供了一个“上下文槽”(setContext/getContext),
// 我们就把每条连接的 HttpContext 存进那个槽里,天然做到“一连接一状态”。
//
// 状态机思路:一条 HTTP 请求由四部分顺序组成
//    请求行 (GET /path?q HTTP/1.1)\r\n
//    请求头 (Field: value)\r\n ... 一直到一个空行 \r\n
//    [请求体]  (长度由 Content-Length 决定)
// 我们就按这个顺序设四个状态,每次只从 Buffer 里“切走能确定完整的那一段”,
// 切不动了(数据没到齐)就停下,return 等下一次 onMessage 再继续——这就是处理半包的关键。
//
#include "HttpRequest.h"

namespace muduo { namespace net { class Buffer; } }   // 前置声明,少包含一个头

namespace httpserver
{

class HttpContext
{
public:
    // 解析进行到哪一步了。kGotAll = 收齐了一整条请求,可以交给业务处理。
    enum HttpRequestParseState
    {
        kExpectRequestLine,   // 期待“请求行”
        kExpectHeaders,       // 期待“请求头”
        kExpectBody,          // 期待“请求体”
        kGotAll,              // 全部到齐
    };

    HttpContext() : state_(kExpectRequestLine) {}

    // 核心:从 buf 里尽量解析。返回 false 表示报文格式错误(调用方该回 400 并断开)。
    // 解析成功但数据没到齐时也返回 true(只是 gotAll() 仍为 false,等更多数据)。
    bool parseRequest(muduo::net::Buffer* buf, muduo::Timestamp receiveTime);

    bool gotAll() const { return state_ == kGotAll; }

    // 处理完一条请求后复位,以便在同一条 keep-alive 连接上解析下一条。
    void reset()
    {
        state_ = kExpectRequestLine;
        HttpRequest dummy;
        request_.swap(dummy);   // 用一个空对象交换,等价于清空 request_
    }

    const HttpRequest& request() const { return request_; }
    HttpRequest& request() { return request_; }

private:
    // 解析“请求行”这一行(不含结尾 CRLF)。成功返回 true。
    bool processRequestLine(const char* begin, const char* end);

    HttpRequestParseState state_;     // 当前解析到哪一步(跨 onMessage 保留)
    HttpRequest           request_;   // 边解析边往里填
};

} // namespace httpserver
