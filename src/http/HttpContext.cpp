#include "HttpContext.h"
#include <muduo/net/Buffer.h>
#include <algorithm>   // std::search

using namespace httpserver;

// 解析“请求行”,例如:  GET /hello?name=bob HTTP/1.1
// 传进来的 [begin, end) 已经是“去掉结尾 \r\n 的一整行”,所以这里只做纯字符串切分,
// 不涉及任何“数据够不够”的判断——那是 parseRequest 的职责。
bool HttpContext::processRequestLine(const char* begin, const char* end)
{
    bool succeed = false;
    const char* start = begin;

    // 1) 方法:第一个空格之前。  "GET" / "POST" ...
    const char* space = std::find(start, end, ' ');
    if (space != end && request_.setMethod(start, space))
    {
        // 2) URL:两个空格之间。  "/hello?name=bob"
        start = space + 1;
        space = std::find(start, end, ' ');
        if (space != end)
        {
            // URL 里可能带查询串,用 '?' 切成 path 和 query 两部分。
            const char* question = std::find(start, space, '?');
            if (question != space)
            {
                request_.setPath(start, question);     // '?' 左边是路径
                request_.setQuery(question + 1, space); // '?' 右边是查询串
            }
            else
            {
                request_.setPath(start, space);        // 没有查询串
            }

            // 3) 版本:最后一段。  "HTTP/1.1"
            start = space + 1;
            // 只接受 HTTP/1.0 或 HTTP/1.1(P0 够用)。
            succeed = (end - start == 8) && std::equal(start, end - 1, "HTTP/1.");
            if (succeed)
            {
                request_.setVersion(std::string(start, end));
            }
        }
    }
    return succeed;
}

// 解析的主循环。注意它被设计成“可重入”:同一条连接的数据没到齐时先返回,
// 下次 onMessage 带着更多数据再次调用本函数,会从上次的 state_ 接着解析。
bool HttpContext::parseRequest(muduo::net::Buffer* buf, muduo::Timestamp receiveTime)
{
    bool ok = true;        // 报文格式是否合法
    bool hasMore = true;   // 是否还能继续往下解析(数据够 + 没出错 + 没解析完)

    while (hasMore)
    {
        if (state_ == kExpectRequestLine)
        {
            // findCRLF():在 Buffer 的可读区间里找 "\r\n"。
            //   找到 -> 返回指向 '\r' 的指针;找不到 -> 返回 nullptr。
            // 找不到 = 连一行请求行都还没收全(半包),退出循环等下次数据。
            const char* crlf = buf->findCRLF();
            if (crlf)
            {
                // buf->peek() 是“当前可读数据的起点”。[peek, crlf) 就是一整行(不含 \r\n)。
                ok = processRequestLine(buf->peek(), crlf);
                if (ok)
                {
                    request_.setReceiveTime(receiveTime);
                    // retrieveUntil:把读指针前移到 crlf+2,即“消费掉”这一行 + 结尾的 \r\n。
                    // 这是处理粘包的关键动作之一:只吃掉已确认完整的部分,剩下的留在 Buffer 里。
                    buf->retrieveUntil(crlf + 2);
                    state_ = kExpectHeaders;   // 进入下一步:解析请求头
                }
                else
                {
                    hasMore = false;           // 请求行格式错误,整体失败
                }
            }
            else
            {
                hasMore = false;               // 半包:请求行没收全,先返回
            }
        }
        else if (state_ == kExpectHeaders)
        {
            // 请求头是若干行 "Field: value",直到遇到一个“空行”(只有 \r\n)表示头结束。
            const char* crlf = buf->findCRLF();
            if (crlf)
            {
                const char* colon = std::find(buf->peek(), crlf, ':');
                if (colon != crlf)
                {
                    // 普通头部行:有冒号,正常添加。
                    request_.addHeader(buf->peek(), colon, crlf);
                }
                else
                {
                    // 没有冒号、又是一行 = 空行 = 请求头结束。
                    // 决定下一步:有 body 就去读 body,否则直接收齐。
                    std::string lenStr = request_.getHeader("Content-Length");
                    if (!lenStr.empty() && std::stol(lenStr) > 0)
                    {
                        state_ = kExpectBody;
                    }
                    else
                    {
                        state_ = kGotAll;      // 没有 body(典型的 GET),收齐
                        hasMore = false;
                    }
                }
                buf->retrieveUntil(crlf + 2);  // 不管哪种情况,这一行都已处理完,消费掉
            }
            else
            {
                hasMore = false;               // 半包:头还没收全
            }
        }
        else if (state_ == kExpectBody)
        {
            // body 的长度由 Content-Length 决定。
            // 只有当 Buffer 里“可读字节数 >= body 长度”时,body 才算到齐。
            // 否则就是半包:body 还在路上,先返回等下次。
            size_t contentLength = static_cast<size_t>(std::stol(request_.getHeader("Content-Length")));
            if (buf->readableBytes() >= contentLength)
            {
                request_.setBody(std::string(buf->peek(), buf->peek() + contentLength));
                buf->retrieve(contentLength);  // 消费掉 body
                state_ = kGotAll;
                hasMore = false;
            }
            else
            {
                hasMore = false;               // 半包:body 没到齐
            }
        }
        else // kGotAll
        {
            hasMore = false;
        }
    }
    return ok;
}
