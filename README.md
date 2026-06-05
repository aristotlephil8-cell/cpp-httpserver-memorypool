# cpp-httpserver-memorypool

> 基于 **Muduo**(one-loop-per-thread Reactor)的最小 HTTP 服务,将一个**自研三层缓存内存池**
> 以**全局 `operator new/delete` 接管**的方式接入请求处理路径,并做**严谨、可辩护**的性能验证。

这个仓库的价值不在"功能多",而在**把一件事做扎实并讲清楚**:内存池接管的工程难点、
workload 依赖的**诚实**性能结论、以及对回退场景的**根因定位**。完整讲解见
[`DESIGN_NOTES.md`](./DESIGN_NOTES.md)(一份能照着从头讲到尾的设计文档)。

---

## 10 秒看点

- **不是"换了内存池就更快"的玩具**:用同一份服务代码做 A/B(链不链入一个 TU),钉核 +
  cold/warm 分离 + 并发梯度 + 多次取中位数,得到**有赢有平有退**的结论,并能解释每一个数字。
- **接管的工程难点真刀真枪**:解决了全局接管必然遇到的**递归(元数据自包含)**、
  **size 头 + 16B 对齐**、**过对齐类型分区**三个问题,并用 **Debug-only 重入断言**兜底、
  **Release 热路径零开销**。
- **诚实**:分配重负载下内存池**反而回退 7–11%**,我没藏——而是定位到根因
  (单一 size class 的中心缓存自旋锁竞争 + 16B size 头把 96B 的 map 节点顶进 112B class),
  并给出可执行的优化方向。

---

## 架构(请求数据流 + 内存池接入点)

```
 client ─TCP→ Acceptor accept → 建 TcpConnection,分给某 IO 线程的 EventLoop
            → onConnection(): setContext(空 HttpContext)          [→池] boost::any(每连接1次)
 client 发字节 → EventLoop(epoll,LT) 可读 → 读入「输入 Buffer」
            → onMessage(): HttpContext::parseRequest             ← 状态机跨 onMessage 保留进度
                 请求行→请求头→[体]   ← findCRLF/retrieveUntil   处理粘包·半包
                                       [→池] 请求头 map 节点+string;请求体 string
            → gotAll()? onRequest(): httpCallback_ 填 resp        [→池] 响应头 map+string;body
                          muduo::Buffer outBuf;                   [→池] ~1KB vector(每请求最大块)
                          appendToBuffer → conn->send()
            → context.reset()(keep-alive 复用)
   所有 [→池] 的分配 ──若 USE_MEMORY_POOL=ON── 走 ▼
══ 全局 operator new/delete (src/pool/GlobalNewDelete.cpp,USE_MEMORY_POOL=ON 才链入) ══
   new(n):  poolReq=roundUp16(n+16); base=MemoryPool::allocate; [16B size 头] 返回 base+16
   delete(p): base=p-16; 读回 poolReq; MemoryPool::deallocate(base,poolReq)
   Debug: 进池前 assert(!tl_inPool);   Release(NDEBUG): 守卫消失,热路径零分支
══ 三层缓存内存池  namespace Avery_memoryPool ══
   ① ThreadCache  thread_local,每 IO 线程一套 freeList[]        ◀ 热路径·无锁
        ↓ miss
   ② CentralCache 每个 size class 一把自旋锁,动态 batch 补货     ◀ 跨线程共享
        ↓ 空
   ③ PageCache    mutex;freeSpans_/spanMap_(std::map);合并相邻 span
        ↓ systemAlloc → mmap(4096 对齐页)   ◀ 16B 对齐保证的根:页对齐 + 块大小整数倍切分
   · >256KB 直接 malloc/free 绕过三层
   · 池自有元数据(map/Span)走 MallocAllocator/Span::operator new=malloc → 防递归(自包含)
   · 三层单例:ThreadCache=thread_local static;Central/Page=Meyers 函数内 static
```

---

## 工程难点(接管全局分配踩到的真问题)

1. **递归 → 元数据自包含**:把池接成全局 `new` 后,池**自己管理元数据时也会分配**
   (PageCache 的两个 `std::map` 红黑树节点 + `new Span`)。这些若仍走 `::operator new` 就会
   "池为了分配 → 又调全局 new → 又进池"无限递归。解法:让池**自有元数据全部走 `malloc`**
   (自定义 `MallocAllocator` + 给 `Span` 加 `operator new/delete`),与"对外提供的分配路径"解耦
   —— 这正是 tcmalloc/jemalloc 等通用分配器的标准做法。**热路径零分支**。
2. **`size` 头 + 16B 对齐**:池的 `deallocate(ptr, size)` 需要 size,但全局 `operator delete(void*)`
   没有 size。解法:每次分配在用户指针前藏 16B 头记录申请大小;头取 `alignof(max_align_t)=16` 并把
   申请量 `roundUp16`,使返回指针满足任意类型的 16B 对齐(对齐链:mmap 页对齐 + 块大小 16 倍数切分)。
3. **过对齐分区**:只接管"非过对齐"一族;`alignas>16` 的类型走带 `align_val_t` 的默认 new/delete,
   自成一对,不与我们的 size 头错配。
4. **Debug-only 重入断言**:全局钩子里加 `thread_local` 守卫,进池中若再进全局 new 就 `assert` 炸,
   **测试期当场抓出**任何漏改成 malloc 的池内部分配;`#ifndef NDEBUG` 包住 → Release 整段消失。

> 也评估过"常驻 thread_local 递归守卫"的更鲁棒备选,但它在热路径有常驻分支开销;
> 取舍理由见 DESIGN_NOTES §4.5。

---

## 压测:方法 + 诚实结果

**方法**(细节见 DESIGN_NOTES §5):服务端固定 4 IO 线程,`taskset` 把服务端(核 0-4)与 wrk 客户端
(核 6-15)**钉在不相交的核**上;每个 cell **重启冷态 → 预热 8s 丢弃 → 计时 3 次 ×30s 取中位数**;
指标 QPS + p99/p999(wrk `--latency`);**stats 计数器单独跑诊断,绝不进计时**。

**stats 诊断**(c=200 稳态):`/plaintext` ≈ **4 次分配/请求**、命中率 **99.999%**;
`/heavy` ≈ **138 次分配/请求**、命中率 **97.8%**,分配高度集中在 **112B** size class。

**A/B 结果**(3 次中位数;延迟 ms;loopback;`-DUSE_MEMORY_POOL=ON` vs `OFF`):

| workload | 并发 | RPS(池) | RPS(基线) | ΔRPS | p99(池) | p99(基线) |
|----------|------|----------|------------|------|---------|-----------|
| plaintext(低分配) | 200  | 812,132 | 785,738 | **+3.4%** | 0.744 | 0.765 |
| plaintext | 1000 | 760,672 | 712,994 | **+6.7%** | 3.267 | 3.448 |
| heavy(分配重) | 50   | 158,747 | 177,787 | **−10.7%** | 0.782 | 0.755 |
| heavy | 1000 | 158,256 | 159,436 | −0.7% | 10.282 | **11.912(尾延迟池更好)** |

(全部运行 `err_status=0` → 服务端从不返非 2xx;波动 CV:池 1–5%,基线 heavy 到 7.4%,故 heavy 差值保守读。)

**诚实结论**:
- **低分配 + 高并发:池小胜**(+3~7%,尾延迟降 3–5%)——几乎全走无锁 thread_local 快路径,跨核扩展性优于 glibc。
- **分配重 + 低/中并发:池小输 7–11%**。根因:2.2% miss × 138 分配/req = **510 万次中心缓存 fetch
  集中在单个 size class(112B),4 个 IO 线程争抢该 class 的同一把自旋锁**;且 **16B size 头把 96B 的
  map 节点顶进 112B class**,进一步集中、放大锁竞争。对手 glibc 的 per-thread tcache 处理这种单尺寸
  高频 churn 更好。
- **分配重 + 满并发:吞吐打平,但尾延迟更好**(p99 −13.7%、p999 −11.4%)。

---

## 构建 & 运行

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

---

## 已知限制 & 优化方向

诚实列出当前边界(详见 DESIGN_NOTES §6/§7):

- **畸形 `Content-Length` 未硬化**:解析用 `std::stol` 无 try/catch,`onMessage` 也无 →
  坏请求抛异常穿透 muduo 回调会**打挂进程**。→ 补校验 / try-catch 回 400。
- **请求体无上限**:超大 `Content-Length` 会让输入 Buffer 一直增长 → **内存 DoS**。→ 设 `maxBodySize` 回 413。
- **HTTP pipelining 未真正支持**:`onMessage` 是 `if (gotAll())` 非 `while`,一次读事件里的多个
  pipelined 请求只处理第一个。→ 改 `while` 循环排空 Buffer。
- **16B 对齐耦合于池的切块不变式**:依赖"span 页对齐 + 块大小 16 倍数";若池改切块策略会破坏对齐。
  → 加 `assert(ptr%16==0)` 兜底 / 文档化契约。
- **heavy 回退**:单 size class 自旋锁竞争 + size 头集中分配。→ 分片 central free list / 增大热 class
  batch / size 存带外避免 class 顶移。

---

## 来源 & 致谢(诚实注明)

- **网络层**:基于 [Muduo](https://github.com/chenshuo/muduo)(陈硕)。本项目只用其核心
  `muduo_net` / `muduo_base`,HTTP 解析/服务逻辑为自己实现(参考过 Kama-HTTPServer 的结构,未复刻其模块)。
- **内存池**:三层缓存内存池为本人另一仓库 `cpp-memory-pool` 的 v3,**整体拷入**
  `third_party/memory_pool/`,来源 commit 与本项目对其所做的最小改动(`MallocAllocator` +
  `PageCache` 元数据自包含)见 [`third_party/memory_pool/SOURCE.md`](./third_party/memory_pool/SOURCE.md)。
- 本项目为学习/求职作品,**性能结论均在 WSL2 + loopback 下测得**,绝对值不代表生产环境(见 DESIGN_NOTES §5.5 测量局限)。
