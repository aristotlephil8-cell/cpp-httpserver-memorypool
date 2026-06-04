#include "../include/MemoryPoolStats.h"

#include <algorithm>
#include <iostream>
#include <utility>
#include <vector>

namespace Avery_memoryPool
{
namespace
{

#ifdef ENABLE_MEMORY_POOL_STATS
struct StatsStorage
{
    std::atomic<bool> enabled{false};
    std::atomic<size_t> allocateCalls{0};
    std::atomic<size_t> deallocateCalls{0};
    std::atomic<size_t> localHitCount{0};
    std::atomic<size_t> localMissCount{0};
    std::atomic<size_t> fetchFromCentralCount{0};
    std::atomic<size_t> returnToCentralCount{0};
    std::atomic<size_t> centralFetchRangeCalls{0};
    std::atomic<size_t> centralReturnRangeCalls{0};
    std::atomic<size_t> pageAllocateSpanCalls{0};
    std::atomic<size_t> pageDeallocateSpanCalls{0};
    std::atomic<size_t> systemAllocCalls{0};
    std::array<std::atomic<size_t>, FREE_LIST_SIZE> perClassFetch{};
    std::array<std::atomic<size_t>, FREE_LIST_SIZE> perClassReturn{};
};

StatsStorage& stats()
{
    static StatsStorage instance;
    return instance;
}

void resetArray(std::array<std::atomic<size_t>, FREE_LIST_SIZE>& values)
{
    for (auto& value : values)
    {
        value.store(0, std::memory_order_relaxed);
    }
}

void printTopSizeClasses(std::ostream& os,
                         const char* label,
                         const std::array<std::atomic<size_t>, FREE_LIST_SIZE>& values)
{
    std::vector<std::pair<size_t, size_t>> nonZero;
    nonZero.reserve(16);
    for (size_t index = 0; index < values.size(); ++index)
    {
        size_t count = values[index].load(std::memory_order_relaxed);
        if (count > 0)
        {
            nonZero.emplace_back(index, count);
        }
    }

    std::sort(nonZero.begin(), nonZero.end(), [](const auto& left, const auto& right) {
        return left.second > right.second;
    });

    os << label;
    if (nonZero.empty())
    {
        os << " none\n";
        return;
    }

    size_t printed = std::min<size_t>(nonZero.size(), 8);
    for (size_t i = 0; i < printed; ++i)
    {
        size_t size = (nonZero[i].first + 1) * ALIGNMENT;
        os << ' ' << size << "B=" << nonZero[i].second;
    }
    os << '\n';
}
#endif

} // namespace

void MemoryPoolStats::setEnabled(bool enabled)
{
#ifdef ENABLE_MEMORY_POOL_STATS
    stats().enabled.store(enabled, std::memory_order_relaxed);
#else
    (void)enabled;
#endif
}

bool MemoryPoolStats::isEnabled()
{
#ifdef ENABLE_MEMORY_POOL_STATS
    return stats().enabled.load(std::memory_order_relaxed);
#else
    return false;
#endif
}

void MemoryPoolStats::reset()
{
#ifdef ENABLE_MEMORY_POOL_STATS
    auto& s = stats();
    s.allocateCalls.store(0, std::memory_order_relaxed);
    s.deallocateCalls.store(0, std::memory_order_relaxed);
    s.localHitCount.store(0, std::memory_order_relaxed);
    s.localMissCount.store(0, std::memory_order_relaxed);
    s.fetchFromCentralCount.store(0, std::memory_order_relaxed);
    s.returnToCentralCount.store(0, std::memory_order_relaxed);
    s.centralFetchRangeCalls.store(0, std::memory_order_relaxed);
    s.centralReturnRangeCalls.store(0, std::memory_order_relaxed);
    s.pageAllocateSpanCalls.store(0, std::memory_order_relaxed);
    s.pageDeallocateSpanCalls.store(0, std::memory_order_relaxed);
    s.systemAllocCalls.store(0, std::memory_order_relaxed);
    resetArray(s.perClassFetch);
    resetArray(s.perClassReturn);
#endif
}

MemoryPoolStatsSnapshot MemoryPoolStats::snapshot()
{
    MemoryPoolStatsSnapshot snapshot;
#ifdef ENABLE_MEMORY_POOL_STATS
    auto& s = stats();
    snapshot.allocateCalls = s.allocateCalls.load(std::memory_order_relaxed);
    snapshot.deallocateCalls = s.deallocateCalls.load(std::memory_order_relaxed);
    snapshot.localHitCount = s.localHitCount.load(std::memory_order_relaxed);
    snapshot.localMissCount = s.localMissCount.load(std::memory_order_relaxed);
    snapshot.fetchFromCentralCount = s.fetchFromCentralCount.load(std::memory_order_relaxed);
    snapshot.returnToCentralCount = s.returnToCentralCount.load(std::memory_order_relaxed);
    snapshot.centralFetchRangeCalls = s.centralFetchRangeCalls.load(std::memory_order_relaxed);
    snapshot.centralReturnRangeCalls = s.centralReturnRangeCalls.load(std::memory_order_relaxed);
    snapshot.pageAllocateSpanCalls = s.pageAllocateSpanCalls.load(std::memory_order_relaxed);
    snapshot.pageDeallocateSpanCalls = s.pageDeallocateSpanCalls.load(std::memory_order_relaxed);
    snapshot.systemAllocCalls = s.systemAllocCalls.load(std::memory_order_relaxed);
#endif
    return snapshot;
}

void MemoryPoolStats::print(std::ostream& os)
{
#ifdef ENABLE_MEMORY_POOL_STATS
    MemoryPoolStatsSnapshot s = snapshot();
    double hitRate = (s.localHitCount + s.localMissCount) == 0
        ? 0.0
        : static_cast<double>(s.localHitCount) /
            static_cast<double>(s.localHitCount + s.localMissCount);

    os << "MemoryPoolStats\n";
    os << "  ThreadCache allocateCalls=" << s.allocateCalls
       << " deallocateCalls=" << s.deallocateCalls
       << " localHitCount=" << s.localHitCount
       << " localMissCount=" << s.localMissCount
       << " hitRate=" << hitRate
       << " fetchFromCentralCount=" << s.fetchFromCentralCount
       << " returnToCentralCount=" << s.returnToCentralCount << '\n';
    os << "  CentralCache fetchRangeCalls=" << s.centralFetchRangeCalls
       << " returnRangeCalls=" << s.centralReturnRangeCalls << '\n';
    os << "  PageCache allocateSpanCalls=" << s.pageAllocateSpanCalls
       << " deallocateSpanCalls=" << s.pageDeallocateSpanCalls
       << " systemAllocCalls=" << s.systemAllocCalls << '\n';
    printTopSizeClasses(os, "  topFetchSizeClasses:", stats().perClassFetch);
    printTopSizeClasses(os, "  topReturnSizeClasses:", stats().perClassReturn);
#else
    os << "MemoryPoolStats disabled. Rebuild with ENABLE_MEMORY_POOL_STATS.\n";
#endif
}

void MemoryPoolStats::recordAllocate()
{
#ifdef ENABLE_MEMORY_POOL_STATS
    auto& s = stats();
    if (!s.enabled.load(std::memory_order_relaxed)) return;
    s.allocateCalls.fetch_add(1, std::memory_order_relaxed);
#endif
}

void MemoryPoolStats::recordDeallocate()
{
#ifdef ENABLE_MEMORY_POOL_STATS
    auto& s = stats();
    if (!s.enabled.load(std::memory_order_relaxed)) return;
    s.deallocateCalls.fetch_add(1, std::memory_order_relaxed);
#endif
}

void MemoryPoolStats::recordLocalHit(size_t)
{
#ifdef ENABLE_MEMORY_POOL_STATS
    auto& s = stats();
    if (!s.enabled.load(std::memory_order_relaxed)) return;
    s.localHitCount.fetch_add(1, std::memory_order_relaxed);
#endif
}

void MemoryPoolStats::recordLocalMiss(size_t)
{
#ifdef ENABLE_MEMORY_POOL_STATS
    auto& s = stats();
    if (!s.enabled.load(std::memory_order_relaxed)) return;
    s.localMissCount.fetch_add(1, std::memory_order_relaxed);
#endif
}

void MemoryPoolStats::recordFetchFromCentral(size_t index)
{
#ifdef ENABLE_MEMORY_POOL_STATS
    auto& s = stats();
    if (!s.enabled.load(std::memory_order_relaxed)) return;
    s.fetchFromCentralCount.fetch_add(1, std::memory_order_relaxed);
    if (index < s.perClassFetch.size())
    {
        s.perClassFetch[index].fetch_add(1, std::memory_order_relaxed);
    }
#else
    (void)index;
#endif
}

void MemoryPoolStats::recordReturnToCentral(size_t index)
{
#ifdef ENABLE_MEMORY_POOL_STATS
    auto& s = stats();
    if (!s.enabled.load(std::memory_order_relaxed)) return;
    s.returnToCentralCount.fetch_add(1, std::memory_order_relaxed);
    if (index < s.perClassReturn.size())
    {
        s.perClassReturn[index].fetch_add(1, std::memory_order_relaxed);
    }
#else
    (void)index;
#endif
}

void MemoryPoolStats::recordCentralFetchRange(size_t)
{
#ifdef ENABLE_MEMORY_POOL_STATS
    auto& s = stats();
    if (!s.enabled.load(std::memory_order_relaxed)) return;
    s.centralFetchRangeCalls.fetch_add(1, std::memory_order_relaxed);
#endif
}

void MemoryPoolStats::recordCentralReturnRange(size_t)
{
#ifdef ENABLE_MEMORY_POOL_STATS
    auto& s = stats();
    if (!s.enabled.load(std::memory_order_relaxed)) return;
    s.centralReturnRangeCalls.fetch_add(1, std::memory_order_relaxed);
#endif
}

void MemoryPoolStats::recordPageAllocateSpan()
{
#ifdef ENABLE_MEMORY_POOL_STATS
    auto& s = stats();
    if (!s.enabled.load(std::memory_order_relaxed)) return;
    s.pageAllocateSpanCalls.fetch_add(1, std::memory_order_relaxed);
#endif
}

void MemoryPoolStats::recordPageDeallocateSpan()
{
#ifdef ENABLE_MEMORY_POOL_STATS
    auto& s = stats();
    if (!s.enabled.load(std::memory_order_relaxed)) return;
    s.pageDeallocateSpanCalls.fetch_add(1, std::memory_order_relaxed);
#endif
}

void MemoryPoolStats::recordSystemAlloc()
{
#ifdef ENABLE_MEMORY_POOL_STATS
    auto& s = stats();
    if (!s.enabled.load(std::memory_order_relaxed)) return;
    s.systemAllocCalls.fetch_add(1, std::memory_order_relaxed);
#endif
}

} // namespace Avery_memoryPool
