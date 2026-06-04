#pragma once
//
// HttpRequest —— 一个“纯数据类”，封装一条已经解析出来的 HTTP 请求。
//
// 它本身不做任何网络/解析工作，只负责“装数据”:方法、路径、查询串、版本、
// 请求头、请求体。真正把字节流解析进来的是 HttpContext(见 HttpContext.h)。
//
// 设计取舍:P0 刻意保持成 POD 风格的小对象,getter/setter 直来直去。
//   - 为什么不直接用一个大 struct? 用类 + 枚举能在面试里讲“方法用枚举避免字符串比较”。
//   - 为什么 header 用 std::map? P0 不追极致性能,map 有序、调试时好看;
//     真正的高频分配热点在 P1 我们会单独分析,不是这里。
//
#include <map>
#include <string>
#include <muduo/base/Timestamp.h>

namespace httpserver
{

class HttpRequest
{
public:
    // HTTP 方法。kInvalid 表示还没解析或解析失败。
    enum Method { kInvalid, kGet, kPost, kHead, kPut, kDelete };

    HttpRequest() : method_(kInvalid), version_("HTTP/1.1") {}

    // ---- 版本 ----
    void setVersion(std::string v) { version_ = std::move(v); }
    const std::string& getVersion() const { return version_; }

    // ---- 方法:从一段字符 [start, end) 解析,成功返回 true ----
    // 注意这里收的是裸指针区间,因为调用方(解析器)是从 Buffer 里直接切片的,
    // 还没构造成 std::string,避免一次多余的拷贝。
    bool setMethod(const char* start, const char* end)
    {
        std::string m(start, end);
        if      (m == "GET")    method_ = kGet;
        else if (m == "POST")   method_ = kPost;
        else if (m == "HEAD")   method_ = kHead;
        else if (m == "PUT")    method_ = kPut;
        else if (m == "DELETE") method_ = kDelete;
        else                    method_ = kInvalid;
        return method_ != kInvalid;
    }
    Method method() const { return method_; }
    const char* methodString() const
    {
        switch (method_)
        {
            case kGet:    return "GET";
            case kPost:   return "POST";
            case kHead:   return "HEAD";
            case kPut:    return "PUT";
            case kDelete: return "DELETE";
            default:      return "UNKNOWN";
        }
    }

    // ---- 路径与查询串 ----
    void setPath(const char* start, const char* end) { path_.assign(start, end); }
    const std::string& path() const { return path_; }
    void setQuery(const char* start, const char* end) { query_.assign(start, end); }
    const std::string& query() const { return query_; }

    // ---- 请求头:形如 "Host: localhost" ----
    void addHeader(const char* start, const char* colon, const char* end)
    {
        std::string field(start, colon);     // 冒号左边是字段名
        ++colon;                              // 跳过冒号
        while (colon < end && isspace(*colon)) ++colon;   // 去掉值前面的空格
        std::string value(colon, end);
        // 去掉值末尾的空格
        while (!value.empty() && isspace(value[value.size() - 1])) value.resize(value.size() - 1);
        headers_[field] = value;
    }
    // 取某个请求头,不存在返回空串。
    std::string getHeader(const std::string& field) const
    {
        auto it = headers_.find(field);
        return it != headers_.end() ? it->second : std::string();
    }
    const std::map<std::string, std::string>& headers() const { return headers_; }

    // ---- 请求体 ----
    void setBody(std::string body) { body_ = std::move(body); }
    const std::string& body() const { return body_; }

    // ---- 接收时间(muduo 在 onMessage 里给的时间戳,调试/统计用)----
    void setReceiveTime(muduo::Timestamp t) { receiveTime_ = t; }
    muduo::Timestamp receiveTime() const { return receiveTime_; }

    // 解析复用:HttpContext 解析完一条请求后会 reset 整个 context,
    // 这里提供 swap 便于把内部清空(经典 muduo 写法)。
    void swap(HttpRequest& that)
    {
        std::swap(method_, that.method_);
        version_.swap(that.version_);
        path_.swap(that.path_);
        query_.swap(that.query_);
        headers_.swap(that.headers_);
        body_.swap(that.body_);
        std::swap(receiveTime_, that.receiveTime_);
    }

private:
    Method                              method_;
    std::string                         version_;
    std::string                         path_;
    std::string                         query_;
    std::map<std::string, std::string>  headers_;
    std::string                         body_;
    muduo::Timestamp                    receiveTime_;
};

} // namespace httpserver
