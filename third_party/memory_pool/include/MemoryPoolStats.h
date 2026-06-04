#pragma once

#include "Common.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <iosfwd>

namespace Avery_memoryPool
{

struct MemoryPoolStatsSnapshot
{
    size_t allocateCalls = 0;
    size_t deallocateCalls = 0;
    size_t localHitCount = 0;
    size_t localMissCount = 0;
    size_t fetchFromCentralCount = 0;
    size_t returnToCentralCount = 0;
    size_t centralFetchRangeCalls = 0;
    size_t centralReturnRangeCalls = 0;
    size_t pageAllocateSpanCalls = 0;
    size_t pageDeallocateSpanCalls = 0;
    size_t systemAllocCalls = 0;
};

class MemoryPoolStats
{
public:
    static void setEnabled(bool enabled);
    static bool isEnabled();
    static void reset();
    static MemoryPoolStatsSnapshot snapshot();
    static void print(std::ostream& os);

    static void recordAllocate();
    static void recordDeallocate();
    static void recordLocalHit(size_t index);
    static void recordLocalMiss(size_t index);
    static void recordFetchFromCentral(size_t index);
    static void recordReturnToCentral(size_t index);
    static void recordCentralFetchRange(size_t index);
    static void recordCentralReturnRange(size_t index);
    static void recordPageAllocateSpan();
    static void recordPageDeallocateSpan();
    static void recordSystemAlloc();
};

#ifdef ENABLE_MEMORY_POOL_STATS
#define MEMORY_POOL_STATS_RECORD_ALLOCATE() \
    ::Avery_memoryPool::MemoryPoolStats::recordAllocate()
#define MEMORY_POOL_STATS_RECORD_DEALLOCATE() \
    ::Avery_memoryPool::MemoryPoolStats::recordDeallocate()
#define MEMORY_POOL_STATS_RECORD_LOCAL_HIT(index) \
    ::Avery_memoryPool::MemoryPoolStats::recordLocalHit(index)
#define MEMORY_POOL_STATS_RECORD_LOCAL_MISS(index) \
    ::Avery_memoryPool::MemoryPoolStats::recordLocalMiss(index)
#define MEMORY_POOL_STATS_RECORD_FETCH_FROM_CENTRAL(index) \
    ::Avery_memoryPool::MemoryPoolStats::recordFetchFromCentral(index)
#define MEMORY_POOL_STATS_RECORD_RETURN_TO_CENTRAL(index) \
    ::Avery_memoryPool::MemoryPoolStats::recordReturnToCentral(index)
#define MEMORY_POOL_STATS_RECORD_CENTRAL_FETCH_RANGE(index) \
    ::Avery_memoryPool::MemoryPoolStats::recordCentralFetchRange(index)
#define MEMORY_POOL_STATS_RECORD_CENTRAL_RETURN_RANGE(index) \
    ::Avery_memoryPool::MemoryPoolStats::recordCentralReturnRange(index)
#define MEMORY_POOL_STATS_RECORD_PAGE_ALLOCATE_SPAN() \
    ::Avery_memoryPool::MemoryPoolStats::recordPageAllocateSpan()
#define MEMORY_POOL_STATS_RECORD_PAGE_DEALLOCATE_SPAN() \
    ::Avery_memoryPool::MemoryPoolStats::recordPageDeallocateSpan()
#define MEMORY_POOL_STATS_RECORD_SYSTEM_ALLOC() \
    ::Avery_memoryPool::MemoryPoolStats::recordSystemAlloc()
#else
#define MEMORY_POOL_STATS_RECORD_ALLOCATE() ((void)0)
#define MEMORY_POOL_STATS_RECORD_DEALLOCATE() ((void)0)
#define MEMORY_POOL_STATS_RECORD_LOCAL_HIT(index) ((void)0)
#define MEMORY_POOL_STATS_RECORD_LOCAL_MISS(index) ((void)0)
#define MEMORY_POOL_STATS_RECORD_FETCH_FROM_CENTRAL(index) ((void)0)
#define MEMORY_POOL_STATS_RECORD_RETURN_TO_CENTRAL(index) ((void)0)
#define MEMORY_POOL_STATS_RECORD_CENTRAL_FETCH_RANGE(index) ((void)0)
#define MEMORY_POOL_STATS_RECORD_CENTRAL_RETURN_RANGE(index) ((void)0)
#define MEMORY_POOL_STATS_RECORD_PAGE_ALLOCATE_SPAN() ((void)0)
#define MEMORY_POOL_STATS_RECORD_PAGE_DEALLOCATE_SPAN() ((void)0)
#define MEMORY_POOL_STATS_RECORD_SYSTEM_ALLOC() ((void)0)
#endif

} // namespace Avery_memoryPool
