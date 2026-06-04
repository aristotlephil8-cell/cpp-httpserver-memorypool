# DESIGN_NOTES —— 面试讲解手册

> 目的:拿着这份文档,能对面试官完整复述项目。每个阶段做完就往里加。

---

## 项目一句话

把一个自研三层缓存内存池(ThreadCache/CentralCache/PageCache)接入一个基于 Muduo 的高并发
HTTP 服务,替换请求路径上高频小对象的分配,并用严谨压测验证其效果。

---

## 阶段进度

- [x] **P0** 最小 Muduo HTTP 服务跑通(`curl` 能返回响应)
- [x] **P1** 内存池接入(全局 new/delete 接管 + 自包含元数据 + Debug 重入断言 + A/B 开关)
- [ ] P2 压测对比(cold/warm + 并发梯度)
- [ ] P3 文档定稿

---

## 环境与构建(可复现)

- 平台:WSL2 Ubuntu 24.04,g++ 13.3,cmake 3.28
- 工作副本:`~/code/HttpServer`(Linux 原生盘;不在 /mnt/f 上编译,避免 9p 跨盘 I/O 拖慢)
- 依赖:`libboost-dev`(Muduo 的 TcpConnection 上下文用 `boost::any`)、`zlib1g-dev`
- Muduo:源码编译装到本地 `~/code/muduo/build/release-install-cpp11`(非 /usr/local,免 sudo)
  - 打了一个小补丁:注释掉 Muduo CMakeLists 里的 `-Werror`(g++13 对其老代码报一堆告警,
    不应让告警阻断构建);并 `-DMUDUO_BUILD_EXAMPLES=OFF` 只编核心库。
- 内存池:拷入 `third_party/memory_pool/`,来源 commit 见该目录 `SOURCE.md`。

构建运行:
```bash
cd ~/code/HttpServer && mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && make -j
./http_server 8080 4        # 端口 8080,4 个 IO 线程
curl -i localhost:8080/     # 200 + JSON
```

---

## P0:数据流(能背下来这条链路 = 过了面试第一刀)

```
客户端 connect
  └─ TcpServer 内部 Acceptor 监听 listen fd,事件就绪时 accept 出新连接
     └─ 为该连接建一个 TcpConnection,分配给某个 IO 线程的 EventLoop
        └─ 回调 onConnection():给这条连接 setContext 一个空 HttpContext(它的解析状态)

客户端发字节
  └─ 该连接所在 EventLoop 的 epoll_wait 发现 socket 可读
     └─ muduo 把数据 read 进该连接的"输入 Buffer",回调 onMessage(conn, buf, time)
        └─ 取出该连接的 HttpContext,调 parseRequest(buf) 解析
           · TCP 是字节流,一次 onMessage 里 buf 可能是 半个/一个/多个请求
           · 解析器是状态机,只"切走"能确定完整的部分,不够就保留进度返回(处理半包)
        └─ gotAll() 为真 → onRequest():
           · 据 Connection 头/HTTP 版本算这条响应发完是否关连接(长/短连接)
           · 调业务回调 httpCallback_(req, &resp),按 path 填 HttpResponse
           · resp.appendToBuffer() 序列化成 HTTP 文本写进输出 Buffer → conn->send()
           · context.reset(),准备在同一条 keep-alive 连接上解析下一条请求
```

### Muduo 网络层概念(被问到要能解释)

| 概念 | 一句话 |
|------|--------|
| Reactor 模式 | "事件来了再处理",不为每个连接占一个线程死等 |
| EventLoop | 一个线程一个循环,反复 `epoll_wait` 等事件再分发;one loop per thread |
| Poller/epoll | EventLoop 底层调 epoll 的人;muduo 默认 **LT(水平触发)**,可读就一直通知,编程简单不易丢数据 |
| Channel | 一个 fd 的"事件登记表":关心读/写,事件来了调对应回调;不拥有 fd |
| TcpServer | 高层封装:Acceptor 管 accept,为每条连接建 TcpConnection 并分给某 IO 线程 |
| TcpConnection | 一条 TCP 连接:带 socket fd + Channel + 输入/输出 Buffer + 一个上下文槽(boost::any) |
| Buffer | 应用层读写缓冲;**存在的根因:TCP 是字节流,一次 read 不等于一个完整消息** |
| 回调三件套 | ConnectionCallback / MessageCallback / 业务 HttpCallback —— 写服务 = 填这几个回调 |
| one-loop-per-thread | `setThreadNum(n)`:主线程只 accept,n 个子线程各跑一个 EventLoop 处理连接。**一条连接固定属于一个 IO 线程**(P1 内存池线程安全的关键前提) |

### 半包/粘包:为什么需要 HttpContext(P0 的核心难点,务必脱稿)

- **问题**:TCP 是"字节流"不是"消息流"。一次 `onMessage` 中,输入 Buffer 里可能是
  ①半个请求(头没收全)②正好一个 ③一个半甚至多个挤在一起。
- **解法**:每条连接挂一个 `HttpContext` 状态机,状态 = 解析到了哪一步
  (请求行 → 请求头 → 请求体 → 收齐)。这个状态**跨多次 onMessage 保留**。
- **关键动作**:解析器每一步先用 `buf->findCRLF()` 找行尾;
  - 找到完整一段 → 处理它,再 `buf->retrieveUntil(crlf+2)` **从 Buffer 里消费掉这段**;
    剩下的字节留在 Buffer(这就是"粘包里多出来的部分"留待继续解析)。
  - 找不到(数据没到齐)→ 立刻返回,**不消费**,等下一次 onMessage 带来更多数据再从原状态接着解析(这就是处理"半包")。
- body 阶段同理:只有 `buf->readableBytes() >= Content-Length` 时才算 body 到齐,否则保留等待。
- 一句话总结:**"只吃能确定完整的那一段,吃不动就留着等下次"** —— 这套机制同时解决了粘包和半包。

### P0 的范围红线(主动说明,体现工程判断)

只做最小可用:HTTP/1.1 请求行+头+可选 body 的解析、`if(path)` 最小路由。
**刻意不做**:路由表/正则/动态路由、中间件、Session、HTTPS、数据库连接池
(这些是参考项目 Kama 的重模块,与"验证内存池效果"这一核心价值无关,做了是范围膨胀)。

---

## P1 内存池接入

### 先纠正一个直觉:每请求的高频分配不在"对象"上,而在 STL 容器内部

`HttpRequest` 是 `HttpContext` 的成员、靠 `reset()` 复用;`HttpContext` 每条连接只建一次。
所以**它们不是每请求 new**。真正每请求的高频小对象分配藏在 STL 内部:

| 来源 | 单次大小 | 频率 |
|------|---------|------|
| 请求头 `std::map` 红黑树节点(每个 header 一个) | ~80–112B | 每请求 ×N |
| header 的 `std::string`(超过 15B SSO 的值) | 几十 B | 每请求若干 |
| 响应头 map 节点 + body string(JSON 超 SSO) | 小 | 每请求 |
| **`onRequest` 里局部 `muduo::net::Buffer` 的 vector** | **~1KB** | **每请求 1 个**(单块最大) |
| `boost::any` 装 `HttpContext` | 较大 | 每**连接**一次(低频) |

### 决策:接在哪一层 / 池化哪些(选项 + 权衡)

候选三种,按"能抓到哪些分配 / 收益 / 难度"对比:

- **A 拦截全局 `operator new/delete`(✅采用)**:抓到所有走 `operator new` 的分配
  —— map 节点、string、那块 1KB Buffer、boost::any、甚至 muduo 内部全覆盖。覆盖最广、
  A/B 最干净(链不链入一个 TU 即可)。代价:要解决 size 头 + 重入两个工程问题(见下)。
  本质是把池当成 tcmalloc 式的 malloc 直替来和 glibc malloc 比 —— **诚实预期**:glibc malloc
  也有 per-thread arena,赢面取决于 workload,P2 如实报。
- **B 给热点容器换 pool-backed allocator(未采用,备选)**:只抓显式改类型的容器(请求/响应
  map);**抓不到那块每请求最大的 1KB Buffer**(muduo 的 `vector<char>` allocator 改不了);
  且改 `std::string`→自定义串类型侵入极大。精准但覆盖小、收益大概率不如 A。
  → 列为"若 A 有余力再做的对照组(只改 map)"。
- **C 池化整个对象(HttpContext/连接级,❌否决)**:HttpContext 经 `boost::any` 内部
  `new holder<>` 构造,连重载 `operator new` 都不一定命中,且是**连接级**低频,完全没碰每请求
  的 STL churn。**收益可忽略**,作为被分析否决的朴素方案记录在案。

### A 方案的两个工程问题(面试重点,能脱稿讲)

**(1) size 头 —— 为什么需要、怎么不破坏对齐**
- 池的 `deallocate(ptr, size)` **强制要 size**(靠它算 free-list 下标),但全局
  `operator delete(void* p)` 签名**没有 size**。
- 解法:每次分配在用户指针前藏一个头,记录"当初向池申请的字节数",释放时读回喂给池。
- 头设 **16 字节**(`alignof(max_align_t)`)而非 8:`operator new` 必须返回能适配任何类型的
  16 字节对齐指针。做法:向池申请的字节数 `roundUp16(n+16)` → 池切出的块大小是 16 的倍数
  → 块基址 16 对齐(mmap 页 4096 对齐,按块大小整数倍切分;>256KB 时池走 malloc 本就 16 对齐)
  → 用户指针 = 基址 + 16 仍 16 对齐。
- 只接管"非过对齐(non-over-aligned)"一族;`alignas>16` 的过对齐类型走带 `align_val_t` 的
  默认重载(new/delete 自成一对),不与我们的头错配。

**(2) 重入 —— 为什么"生产级分配器要自包含元数据"**
- 把池接成全局 new 后,**池自己管理元数据时也会分配**:`PageCache` 的两个 `std::map`
  (freeSpans_/spanMap_)申请红黑树节点 + `new Span`。这些若仍走 `::operator new` →
  "池为了分配 → 又调全局 new → 又进池" → **无限递归**。
- 解法(方案①,采用):让池的**自有元数据全部走 malloc**,与"它对外提供的分配路径"解耦:
  - 新增 `MallocAllocator`(只用 malloc/free 的自包含 allocator),给 PageCache 两个 map 用;
  - `Span` 加成员 `operator new/delete` → malloc/free(所有 `new Span` 调用点自动绕开钩子)。
  - **热路径零分支**:稳态分配从 ThreadCache freelist 直接拿,根本不碰这些元数据路径。
- 三层已逐一审计:ThreadCache/CentralCache 只用 `std::array`(内联非堆)+ 大对象直接 malloc,
  无隐式堆分配;stats 的 `print()` 用 `std::vector` 但不在分配热路径上,保留未改。

**(3) Debug-only 重入断言 —— 怎么兜住枚举遗漏**
- 万一漏改了某处池内部分配,它会在"池正在分配中"再次进全局 new。
- 在全局 new/delete 里加一个 **thread_local 守卫**:进池前 `assert(!inPool)`,进池中置位。
  漏网的嵌套分配会立刻让 assert 在测试阶段炸出来,精确指出"还有分配源没改成 malloc"。
- 用 `#ifndef NDEBUG` 包起来:**Release(定义 NDEBUG)整段编译消失,热路径零开销**;
  Debug 才生效。已验证:Debug 构建跑 200 次混合请求(含触发补货→map 路径)**断言未触发**
  → 枚举干净。

### 备选方案②:thread_local 递归守卫(考虑过,未采用)
另一条路是**不改 third_party**,只在全局钩子里常驻一个 thread_local 递归守卫:
`if (inPool) return malloc(size); else { inPool=true; 走池; inPool=false; }`。
- 优点:池源码一行不改,对池内任何分配(含将来新增)都自动安全、最鲁棒。
- 缺点:**每次 new/delete 都多一个 thread_local 分支**(即使稳态根本不会重入),热路径有常驻开销。
- **取舍("自包含元数据 vs 递归守卫")**:②把"防递归"做成运行时常驻开销;①把它做成
  编译期消失的 Debug 断言 + 一次性的元数据改造,**热路径真正零开销**,且"分配器自有元数据
  必须自包含"本就是生产级分配器(tcmalloc/jemalloc)的标准做法。故选①,②作为更鲁棒的备选记录。

### 线程安全(one-loop-per-thread 下的契合点)
- Muduo 一条连接固定属于一个 IO 线程,请求对象基本在所属 IO 线程内创建/销毁;
  池的 `ThreadCache` 是 `thread_local`,**每个 IO 线程一套本地自由链表,分配热路径无锁**
  —— 与线程模型天然契合。
- 潜在坑:**跨线程归还**(A 线程分配、B 线程释放)。全局 new 接管下,谁 free 就归到谁的
  ThreadCache;只要"分配的 size == 释放的 size"(我们靠 size 头保证),归还到正确大小类即可,
  正确性不依赖"同一线程归还"。跨线程会让块在不同 ThreadCache 间迁移,但不破坏正确性。
  CentralCache 用自旋锁、PageCache 用 mutex 保护跨线程共享部分。

### A/B 压测开关(给 P2 用)
- `cmake -DUSE_MEMORY_POOL=ON  ..` → 池版(链入 `GlobalNewDelete.cpp` + 池源码)
- `cmake -DUSE_MEMORY_POOL=OFF ..` → 基线版(默认 glibc malloc)
- `cmake -DMEMORY_POOL_STATS=ON ..` → 额外开启池统计计数器(命中率/各层调用次数)
- 同一份服务代码,启动横幅会打印当前用的是哪种分配器,便于确认。
- 已验证三态:Release-ON / Debug-ON(断言不触发)/ Release-OFF 均正常 `curl` 通。

## P2 压测（待定）

## 附:关键设计决策清单
（每条 = 选项 + 权衡 + 最终选择 + 理由,持续补充）
