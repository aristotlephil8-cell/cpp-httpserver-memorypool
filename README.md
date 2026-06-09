# cpp-httpserver-memorypool

基于 Muduo(one-loop-per-thread Reactor)的最小 HTTP 服务。把一个自研的三层缓存内存池,以全局 `operator new/delete` 接管的方式接入请求处理路径,然后做 A/B 性能验证。

重点不是功能多,而是把内存池接管这一件事做完整:接管时的工程问题、依赖 workload 的性能结论(有赢、有平、有退),以及对回退场景的根因定位。

## 范围

只做"验证内存池效果"所需的最小服务。没有路由表/正则路由、中间件、Session、HTTPS、数据库连接池——这些与内存池无关,做了只是扩大范围。

几点结论先说在前面:

- 用同一份服务代码做 A/B(是否链入内存池那个 TU),配合钉核、cold/warm 分离、并发梯度、多次取中位数,得到有赢有平有退的结果,每个数字都能解释。
- 全局接管要解决三个问题:递归(元数据自包含)、size 头加 16B 对齐、过对齐类型分区。用 Debug-only 重入断言兜底,Release 热路径零开销。
- 分配重负载下内存池反而回退 7–11%。根因是单一 size class 的中心缓存自旋锁竞争,加上 16B size 头把 96B 的 map 节点顶进了 112B 这个 class。下面给出优化方向。

## 架构(请求数据流 + 内存池接入点)

```mermaid
flowchart LR
    NET["Muduo 网络层<br/>EventLoop · HttpContext 解析"] -->|每请求分配| H["operator new/delete<br/>16B size 头"]
    H --> TC["ThreadCache<br/>无锁"]
    TC -->|miss| CC["CentralCache<br/>自旋锁"]
    CC -->|空| PC["PageCache<br/>mutex"]
    PC -->|systemAlloc| MM["mmap"]

    classDef net  fill:#dbeafe,stroke:#3b82f6,color:#1e3a8a;
    classDef hook fill:#fef3c7,stroke:#f59e0b,color:#78350f;
    classDef pool fill:#dcfce7,stroke:#22c55e,color:#14532d;
    classDef sys  fill:#f3e8ff,stroke:#a855f7,color:#581c87;
    class NET net;
    class H hook;
    class TC,CC,PC pool;
    class MM sys;
```

蓝色是 Muduo 网络层:一条连接固定属于某个 IO 线程的 EventLoop,`HttpContext` 状态机跨多次读事件解析(处理粘包/半包),`onRequest` 填好响应再发出。琥珀色是接管点:请求路径上的小对象分配(请求/响应头的 map 节点和 string、约 1KB 的输出 Buffer 等),在 `USE_MEMORY_POOL=ON` 时全部被全局 `operator new/delete` 接住,指针前藏 16B size 头。绿色是三层池:ThreadCache 无锁热路径命中,miss 就向 CentralCache 取,再空就向 PageCache 要 span,最后 `mmap`(紫色)向系统拿页。两条规则没画进图里:池自有元数据走 `malloc` 防递归,>256KB 绕过三层直接 `malloc`。

## 工程问题(接管全局分配时遇到的)

1. 递归,用元数据自包含解决。把池接成全局 `new` 后,池自己管理元数据时也会分配(PageCache 的两个 `std::map` 红黑树节点,以及 `new Span`)。这些若仍走 `::operator new`,就会"池为了分配 → 又调全局 new → 又进池",无限递归。解法是让池自有元数据全部走 `malloc`(自定义 `MallocAllocator`,并给 `Span` 加 `operator new/delete`),与对外提供的分配路径解耦。这也是 tcmalloc/jemalloc 这类通用分配器的标准做法。稳态热路径不碰这条元数据路径。

2. size 头加 16B 对齐。池的 `deallocate(ptr, size)` 需要 size,但全局 `operator delete(void*)` 没有 size。解法是每次分配在用户指针前藏一个 16B 头记录申请大小,释放时读回。头取 `alignof(max_align_t)=16`,并把申请量 `roundUp16`,使返回指针满足任意类型的 16B 对齐。对齐链是:mmap 页对齐,加上块大小按 16 倍数切分。

3. 过对齐分区。只接管非过对齐一族;`alignas>16` 的类型走带 `align_val_t` 的默认 new/delete,自成一对,不与 size 头错配。

4. Debug-only 重入断言。全局钩子里加一个 `thread_local` 守卫,进池中若再进全局 new 就 `assert`。这样能在测试期当场抓出任何漏改成 malloc 的池内部分配。用 `#ifndef NDEBUG` 包住,Release 整段消失。

也评估过常驻 `thread_local` 递归守卫这个更鲁棒的备选,但它在热路径有常驻分支开销,所以没用。

## 压测:方法与结果

方法:服务端固定 4 IO 线程,用 `taskset` 把服务端(核 0-4)和 wrk 客户端(核 6-15)钉在不相交的核上。每个 cell 先重启到冷态,预热 8s 丢弃,再计时 3 次 ×30s 取中位数。指标是 QPS 加 p99/p999(wrk `--latency`)。stats 计数器单独跑诊断,不进计时。

stats 诊断(c=200 稳态):`/plaintext` 约 4 次分配/请求,命中率 99.999%;`/heavy` 约 138 次分配/请求,命中率 97.8%,分配高度集中在 112B 这个 size class。

A/B 结果(3 次中位数;延迟 ms;loopback;`-DUSE_MEMORY_POOL=ON` vs `OFF`):

| workload | 并发 | RPS(池) | RPS(基线) | ΔRPS | p99(池) | p99(基线) |
|----------|------|----------|------------|------|---------|-----------|
| plaintext(低分配) | 200  | 812,132 | 785,738 | +3.4% | 0.744 | 0.765 |
| plaintext | 1000 | 760,672 | 712,994 | +6.7% | 3.267 | 3.448 |
| heavy(分配重) | 50   | 158,747 | 177,787 | −10.7% | 0.782 | 0.755 |
| heavy | 1000 | 158,256 | 159,436 | −0.7% | 10.282 | 11.912(尾延迟池更好) |

全部运行 `err_status=0`,服务端从不返非 2xx。波动 CV:池 1–5%,基线 heavy 到 7.4%,所以 heavy 的差值按保守读。

结论:

- 低分配加高并发,池小胜(+3~7%,尾延迟降 3–5%)。几乎全走无锁 thread_local 快路径,跨核扩展性优于 glibc。
- 分配重加低/中并发,池小输 7–11%。根因:2.2% miss × 138 分配/req = 510 万次中心缓存 fetch,集中在单个 size class(112B),4 个 IO 线程争抢该 class 的同一把自旋锁;而且 16B size 头把 96B 的 map 节点顶进 112B class,进一步集中、放大了锁竞争。glibc 的 per-thread tcache 处理这种单尺寸高频 churn 更好。
- 分配重加满并发,吞吐打平,但尾延迟更好(p99 −13.7%、p999 −11.4%)。

## 构建与运行

```bash
# 依赖:Linux(用了 mmap),g++(C++17),cmake,boost,zlib;Muduo 装到本地后用 -DMUDUO_PREFIX 指向它
git clone <this repo> && cd cpp-httpserver-memorypool
mkdir build && cd build

cmake -DCMAKE_BUILD_TYPE=Release -DUSE_MEMORY_POOL=ON  ..   # 池版
# cmake -DCMAKE_BUILD_TYPE=Release -DUSE_MEMORY_POOL=OFF ..  # 基线版(默认 glibc malloc)
# cmake -DCMAKE_BUILD_TYPE=Release -DUSE_MEMORY_POOL=ON -DMEMORY_POOL_STATS=ON ..  # 诊断版
make -j

./http_server 8080 4          # 端口 8080,4 个 IO 线程(启动横幅会打印当前分配器)
curl -i localhost:8080/       # 200 + JSON;另有 /plaintext、/heavy(压测路由)
```

压测脚本在 `bench/`:`run_bench.sh`(计时 A/B)、`run_stats.sh`(stats 诊断)、`analyze.py`(中位数+CV)。

## 已知限制与优化方向

当前边界:

- 畸形 `Content-Length` 未硬化:解析用 `std::stol` 无 try/catch,`onMessage` 也没有,坏请求抛异常穿透 muduo 回调会打挂进程。要补校验或 try-catch 回 400。
- 请求体无上限:超大 `Content-Length` 会让输入 Buffer 一直增长,造成内存 DoS。要设 `maxBodySize` 回 413。
- HTTP pipelining 未真正支持:`onMessage` 是 `if (gotAll())` 不是 `while`,一次读事件里的多个 pipelined 请求只处理第一个。要改成 `while` 循环排空 Buffer。
- 16B 对齐耦合于池的切块不变式:依赖 span 页对齐加块大小 16 倍数;若池改切块策略会破坏对齐。要加 `assert(ptr%16==0)` 兜底,或把契约文档化。
- heavy 回退:单 size class 自旋锁竞争加 size 头集中分配。优化方向:分片 central free list、增大热 class 的 batch、把 size 存到带外避免 class 顶移。

## 来源与致谢

- 网络层:基于 [Muduo](https://github.com/chenshuo/muduo)(陈硕)。本项目只用其核心 `muduo_net` / `muduo_base`,HTTP 解析和服务逻辑自己实现(参考过 Kama-HTTPServer 的结构,未复刻其模块)。
- 内存池:三层缓存内存池为本人另一仓库 `cpp-memory-pool` 的 v3,整体拷入 `third_party/memory_pool/`。来源 commit 与本项目对其所做的最小改动(`MallocAllocator` 加 `PageCache` 元数据自包含)见 [`third_party/memory_pool/SOURCE.md`](./third_party/memory_pool/SOURCE.md)。
- 本项目为学习/求职作品,性能结论均在 WSL2 + loopback 下测得,绝对值不代表生产环境。
