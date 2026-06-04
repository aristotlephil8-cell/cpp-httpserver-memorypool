#pragma once
#include "Common.h"
#include "MallocAllocator.h"   // 池自有元数据用的“自包含”分配器(防与全局 new 钩子递归)
#include <map>
#include <mutex>
#include <cstdlib>     // malloc/free
#include <new>         // std::bad_alloc
#include <functional>  // std::less
#include <utility>     // std::pair

namespace Avery_memoryPool
{

class PageCache
{
public:
    static const size_t PAGE_SIZE = 4096; // 4K页大小

    static PageCache& getInstance()
    {
        static PageCache instance;
        return instance;
    }

    // 分配指定页数的span
    void* allocateSpan(size_t numPages);

    // 释放span
    void deallocateSpan(void* ptr, size_t numPages);

private:
    PageCache() = default;

    // 向系统申请内存
    void* systemAlloc(size_t numPages);
private:
    struct Span
    {
        void*  pageAddr; // 页起始地址
        size_t numPages; // 页数
        Span*  next;     // 链表指针

        // 让 `new Span` / `delete span` 直接走 malloc/free,绕开全局 operator new 钩子。
        // 否则:池为了分配 → new Span → 全局 new → 又进池 → 递归。
        // 加成员 operator new/delete 后,所有 `new Span` 调用点都自动走这里,无需改调用点。
        static void* operator new(std::size_t sz)
        {
            void* p = std::malloc(sz);
            if (!p) throw std::bad_alloc();
            return p;
        }
        static void operator delete(void* p) noexcept { std::free(p); }
    };

    // 按页数管理空闲span，不同页数对应不同Span链表。
    // 关键:第 4 个模板参数把红黑树节点的分配换成 MallocAllocator,使 map 的内部分配
    //       不经过全局 operator new 钩子(防递归)。这就是“池的元数据自包含”。
    std::map<size_t, Span*, std::less<size_t>,
             MallocAllocator<std::pair<const size_t, Span*>>> freeSpans_;
    // 页地址到span的映射，用于回收(同样换 MallocAllocator)
    std::map<void*, Span*, std::less<void*>,
             MallocAllocator<std::pair<void* const, Span*>>> spanMap_;
    std::mutex mutex_;
};

} // namespace memoryPool