#pragma once
//
// MallocAllocator —— 一个“自包含(self-contained)”的 STL 分配器,内部只用 malloc/free。
//
// 【为什么需要它?面试要能脱稿讲:生产级分配器为什么要让自己的元数据自包含】
// 我们把这个内存池接成了进程的全局 operator new/delete。可是内存池**自己也要管理元数据**
// —— PageCache 用两个 std::map(freeSpans_/spanMap_)记录空闲与回收信息。
// 如果这两个 map 还用默认的 std::allocator,它们申请红黑树节点时会调 ::operator new,
// 而 ::operator new 已经被我们换成了“调用内存池” —— 于是:
//     池为了分配 → 要往 map 里记一笔 → map 申请节点 → ::operator new → 又进池 → 死循环递归。
// 解决之道:让池“自有的簿记数据结构”不要走它对外提供的那条分配路径,而是直接用系统 malloc。
// 这就是通用分配器(tcmalloc/jemalloc 等)的通用做法 —— **分配器的内部元数据必须自包含**,
// 不能依赖“它正在对外提供的同一条分配路径”,否则自举(bootstrap)阶段和重入都会出问题。
//
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>      // std::bad_alloc

namespace Avery_memoryPool
{

template <typename T>
struct MallocAllocator
{
    using value_type = T;

    MallocAllocator() noexcept = default;
    // 允许 rebind 拷贝构造(std::map 会把它 rebind 成“红黑树节点”的分配器)。
    template <typename U>
    MallocAllocator(const MallocAllocator<U>&) noexcept {}

    T* allocate(std::size_t n)
    {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_alloc();
        void* p = std::malloc(n * sizeof(T));   // 关键:直接 malloc,绕开全局 operator new 钩子
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t /*n*/) noexcept
    {
        std::free(p);
    }
};

// 无状态分配器:任意两个实例都相等(允许彼此 deallocate 对方分配的内存)。
template <typename T, typename U>
bool operator==(const MallocAllocator<T>&, const MallocAllocator<U>&) noexcept { return true; }
template <typename T, typename U>
bool operator!=(const MallocAllocator<T>&, const MallocAllocator<U>&) noexcept { return false; }

} // namespace Avery_memoryPool
