# 内存池来源说明

本目录代码**整体拷贝自**开发者自有仓库 `cpp-memory-pool` 的 `v3/`,非本项目原创。

- 来源仓库:cpp-memory-pool
- 来源版本:`v3`(动态 batch + 已修复正确性 bug + stats counters)
- 来源 commit:`f35c8ca8010661199ff55aaa82b96bc57c69d07d`
  (2026-05-27 "Merge pull request #1 ... fix/threadcache-and-namespace-refactor")
- 命名空间:`Avery_memoryPool`
- 对外接口:`Avery_memoryPool::MemoryPool::allocate(size_t)` / `deallocate(void*, size_t)`
- 统计开关:编译期宏 `ENABLE_MEMORY_POOL_STATS`(默认关闭)

> 拷入而非 submodule 的理由:代码全部落在本仓库内,面试讲解与构建都更简单。
> 如需同步上游改动,以来源 commit 为基线手动 diff。

## 本项目对来源代码的修改(P1 接入需要)

为把内存池接管成进程级全局 `operator new/delete`,必须让“池自身的内部分配”不再走
全局 `operator new`(否则会与钩子无限递归)。为此做了如下**最小**改动:

1. 新增 `include/MallocAllocator.h`(原仓库没有):一个只用 malloc/free 的自包含 STL 分配器。
2. `include/PageCache.h`:
   - 两个 `std::map`(`freeSpans_` / `spanMap_`)的分配器参数改为 `MallocAllocator<...>`;
   - 内嵌 `struct Span` 增加成员 `operator new/operator delete`,改走 malloc/free
     (使所有 `new Span` 调用点自动绕开全局钩子,无需改调用点)。

> 三层已逐一审计:`ThreadCache` / `CentralCache` 仅用 `std::array`(内联,非堆)+ 大对象
> 直接 malloc,无隐式堆分配;`MemoryPoolStats::print()` 里的 `std::vector` 不在分配热路径上
> (仅手动诊断时调用,不会递归),保留未改。
> 另有 Debug-only 重入断言(见 `src/pool/GlobalNewDelete.cpp`)兜底:若仍有遗漏的池内部分配
> 走了全局 new,Debug 构建会当场 assert。
