//
// GlobalNewDelete.cpp —— 把自研内存池接管成进程级的 operator new / operator delete。
//
// 这个 TU 只有在 CMake 选项 USE_MEMORY_POOL=ON 时才被编译链接。
// 不链接它,程序就用默认的 glibc malloc 路径 —— 这正是我们 A/B 压测的开关:
// 【同一份服务代码,链不链入本文件 = 池版 vs 基线版】。
//
// 本文件要讲清楚三件面试必问的事:
//   (1) size 头:为什么需要,怎么放,怎么不破坏对齐;
//   (2) 16 字节对齐:为什么 operator new 必须保证它,我们怎么保证;
//   (3) Debug 重入断言:它怎么兜住“某处池内部分配漏改成 malloc”的疏漏。
//
#include "MemoryPool.h"   // Avery_memoryPool::MemoryPool::allocate / deallocate

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#ifndef NDEBUG
#include <cassert>
#endif

namespace
{

// ── size 头 ───────────────────────────────────────────────────────────────
// 【为什么需要 size 头】
// 内存池的 deallocate(ptr, size) **强制要 size**(它靠 size 算出在哪个 free-list 下标归还)。
// 但全局 `operator delete(void* p)` 的签名里**没有 size**。所以释放时我们必须能“凭 p 反查 size”。
// 做法:每次分配时,在用户指针前面藏一个头,记录“当初向池申请的字节数”;释放时读回来喂给池。
//
// 【为什么头是 16 字节而不是 8】
// 头里其实只需要存一个 size_t(8 字节)。但 operator new 返回的指针必须满足
// “适配任何类型”的最大基本对齐 alignof(max_align_t)=16(x86-64)。
// 把头设成 16 字节,用户指针 = 基地址 + 16,就能在“基地址是 16 对齐”时让用户指针也 16 对齐。
constexpr std::size_t kHeader = alignof(std::max_align_t);   // = 16 on x86-64
static_assert(kHeader >= sizeof(std::size_t), "header must hold a size_t");

// 向上取整到 16 的倍数。
inline std::size_t roundUp16(std::size_t n)
{
    return (n + (kHeader - 1)) & ~(kHeader - 1);
}

// ── Debug 重入断言 ────────────────────────────────────────────────────────
// 【它兜住什么】
// 方案①要求:池内部的一切分配(PageCache 的两个 std::map、new Span)都必须改走 malloc。
// 万一漏改了某一处,它会在“池正在分配的过程中”再次调用全局 operator new → 重入。
// 这个守卫在 **Debug 构建**下把重入当场 assert 炸出来,让你在测试阶段就发现漏网之鱼;
// **Release 构建**(定义了 NDEBUG)下整段编译消失,热路径零分支 —— 满足“release 零开销”。
#ifndef NDEBUG
thread_local bool tl_inPool = false;
struct ReentryGuard
{
    ReentryGuard()
    {
        // 若此处触发:说明仍有某个“池内部分配”走了全局 operator new,没改成 malloc。
        // 去 PageCache 等处把它换成 MallocAllocator / Span::operator new。
        assert(!tl_inPool &&
               "memory pool re-entered global operator new: "
               "some pool-internal allocation still routes through the hook; "
               "switch it to malloc (MallocAllocator / Span::operator new).");
        tl_inPool = true;
    }
    ~ReentryGuard() { tl_inPool = false; }
};
#define POOL_REENTRY_GUARD() ReentryGuard _pool_reentry_guard_
#else
#define POOL_REENTRY_GUARD() ((void)0)   // Release:彻底消失,无任何指令
#endif

// ── 核心:分配 / 释放 ──────────────────────────────────────────────────────
void* pool_alloc(std::size_t n)
{
    // 向池申请 = 用户要的 n + 头,并对齐到 16。
    // 存进头里的就是“向池申请的字节数”poolReq,释放时原样喂回池(保证 allocate/deallocate 同 size)。
    const std::size_t poolReq = roundUp16(n + kHeader);

    void* base;
    {
        POOL_REENTRY_GUARD();   // Debug:进入池期间若再调全局 new 就炸
        base = Avery_memoryPool::MemoryPool::allocate(poolReq);
    }
    if (!base) return nullptr;

    // 在头部记录 poolReq。
    // 对齐保证:poolReq 是 16 的倍数 → 池切出的块大小是 16 的倍数 → 块基地址 16 对齐
    //          (mmap 来的页 4096 对齐,块按 poolReq 的整数倍切分);
    //          大于 256KB 时池走 malloc,malloc 本就 16 对齐。
    //          于是 base 16 对齐,用户指针 = base + 16 仍 16 对齐。✓
    *reinterpret_cast<std::size_t*>(base) = poolReq;
    return reinterpret_cast<char*>(base) + kHeader;
}

void pool_free(void* p) noexcept
{
    if (!p) return;   // delete nullptr 是合法 no-op
    void* base = reinterpret_cast<char*>(p) - kHeader;
    const std::size_t poolReq = *reinterpret_cast<std::size_t*>(base);
    {
        POOL_REENTRY_GUARD();
        Avery_memoryPool::MemoryPool::deallocate(base, poolReq);
    }
}

} // namespace

// ── 替换全局 operator new/delete(非对齐族) ───────────────────────────────
// 注意:我们只接管“非过对齐(non-over-aligned)”这一族。
// 过对齐类型(alignas 超过 16)走的是带 std::align_val_t 的重载,我们**不**接管,
// 它们的 new 和 delete 都用默认实现 —— 自成一对、不会和我们的头错配。这样分区干净、无歧义。

void* operator new(std::size_t n)
{
    void* p = pool_alloc(n);
    if (!p) throw std::bad_alloc();   // 抛出版 new:失败必须抛 bad_alloc
    return p;
}

void* operator new[](std::size_t n)
{
    void* p = pool_alloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}

// nothrow 版:失败返回 nullptr,不抛异常。
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return pool_alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return pool_alloc(n); }

void operator delete(void* p) noexcept { pool_free(p); }
void operator delete[](void* p) noexcept { pool_free(p); }

// sized delete(C++14):编译器可能调用带 size 的版本。我们有自己的头,忽略传入的 size 即可。
void operator delete(void* p, std::size_t) noexcept { pool_free(p); }
void operator delete[](void* p, std::size_t) noexcept { pool_free(p); }

// nothrow delete(与 nothrow new 配对)。
void operator delete(void* p, const std::nothrow_t&) noexcept { pool_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { pool_free(p); }
