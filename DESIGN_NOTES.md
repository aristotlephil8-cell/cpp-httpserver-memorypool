# DESIGN_NOTES —— 面试讲解手册

> 目的:拿着这一份文档,能从头到尾把项目讲清楚——数据流 → Muduo 模型 → 内存池接管 →
> 压测方法 → 结论 → 已知限制。每一节都按"面试官会怎么追问"来组织。

## 目录
- [0. 项目一句话 + 进度](#0)
- [1. 全景图(先看这张)](#1)
- [2. 环境与构建(可复现)](#2)
- [3. 请求数据流详解(P0)](#3)
- [4. 内存池接管(P1)](#4)
- [5. 压测对比(P2)](#5)
- [6. 已知限制(集中弹药库)](#6)
- [7. 改进方向(future work)](#7)
- [8. 简历话术](#8)

---

<a id="0"></a>
## 0. 项目一句话 + 进度

把一个自研三层缓存内存池(ThreadCache / CentralCache / PageCache)以**全局 `operator new/delete`
接管**的方式,接入一个基于 Muduo(one-loop-per-thread)的高并发 HTTP 服务,替换请求路径上
高频小对象的分配,并用**严谨、可辩护**的压测验证其效果(诚实报告赢/平/退)。

- [x] **P0** 最小 Muduo HTTP 服务跑通(`curl` 能返回响应)
- [x] **P1** 内存池接入(全局 new/delete 接管 + 自包含元数据 + Debug 重入断言 + A/B 开关)
- [x] **P2** 压测对比(cold/warm + 并发梯度 + stats 诊断 + 诚实结论)
- [x] **P3** 文档统稿(本文件)

**范围红线**(主动说明,体现工程判断):只做"验证内存池效果"所必需的最小服务。
**刻意不做**路由表/正则/动态路由、中间件、Session、HTTPS、数据库连接池——这些是参考项目
Kama 的重模块,与核心价值无关,做了就是范围膨胀。

---

<a id="1"></a>
## 1. 全景图(先看这张)

```
══════════════════════════════════════════════════════════════════════════════
 请求数据流  (一条连接固定属于一个 IO 线程 = one loop per thread)
   右侧 [→池] = 该步触发的堆分配,若 USE_MEMORY_POOL=ON 就走下面的全局钩子→三层池
══════════════════════════════════════════════════════════════════════════════

 client ──TCP──▶ [Acceptor] accept
                     │ 新连接 → 建 TcpConnection,分给某 IO 线程的 EventLoop
                     ▼
              onConnection()  ── conn.setContext(空 HttpContext)
                     │           [→池] boost::any 装 HttpContext(每连接 1 次,低频)
                     │
 client 发字节 ─▶ 该连接所在 EventLoop(epoll, LT)发现 socket 可读 ─▶ 读入「输入 Buffer」
                     ▼
              onMessage(conn, buf, time)
                     │  HttpContext::parseRequest(buf)      ← 状态机,跨多次 onMessage 保留进度
                     │    请求行 → 请求头 → [请求体]         ← findCRLF / retrieveUntil 处理粘包·半包
                     │           [→池] 每个请求头 → map 红黑树节点 + string(每请求 ×N)
                     │           [→池] setBody → 请求体 string(POST 才有)
                     ▼  gotAll() ?
              onRequest()
                     │  httpCallback_(req,&resp)            ← 业务 handler 按 path 填 HttpResponse
                     │           [→池] 响应头 → map 节点 + string;body string(超 SSO 时)
                     │  muduo::net::Buffer outBuf;          ← 局部输出缓冲
                     │           [→池] outBuf 的 vector ~1KB(每请求 1 块,单块最大!)
                     │  resp.appendToBuffer(outBuf) → conn->send()
                     ▼  context.reset()                     ← keep-alive 复用,解析下一条
 client ◀──TCP── 「输出 Buffer」

 上面所有 [→池] 的分配——若 USE_MEMORY_POOL=ON——全部走 ▼

══════════════════════════════════════════════════════════════════════════════
 内存池接管点:全局 operator new / operator delete
   src/pool/GlobalNewDelete.cpp  (只有 USE_MEMORY_POOL=ON 才链入这个 TU)
──────────────────────────────────────────────────────────────────────────────
   new(n)    : poolReq = roundUp16(n + 16);  base = MemoryPool::allocate(poolReq);
               [在 base 处写 16B size 头 = poolReq]   返回 base+16  (16 字节对齐)
   delete(p) : base = p - 16;  poolReq = *(size_t*)base;
               MemoryPool::deallocate(base, poolReq)
   · Debug 构建:进池前 assert(!tl_inPool)  —— 兜住"漏改成 malloc 的池内部分配"
   · Release(NDEBUG):守卫整段消失,热路径零分支
══════════════════════════════════════════════════════════════════════════════
                     │  MemoryPool::allocate/deallocate(size)
                     ▼
══════════════════════════════════════════════════════════════════════════════
 三层缓存内存池  namespace Avery_memoryPool
──────────────────────────────────────────────────────────────────────────────
  ① ThreadCache    thread_local,每 IO 线程一套 freeList[FREE_LIST_SIZE]   ◀ 热路径·无锁
        │ 本地空(miss)                                    ▲ 命中:直接弹出返回
        ▼ fetchFromCentralCache(index)                     │
  ② CentralCache   每个 size class 一把自旋锁;批量(动态 batch)补货/回收 ◀ 跨线程共享
        │ 该 class 空                                       ▲
        ▼ fetchFromPageCache(size)                          │
  ③ PageCache      std::mutex 保护;freeSpans_ / spanMap_(std::map);合并相邻 span
        │                                                   ▲
        ▼ systemAlloc
      mmap(按页,4096 对齐)   ◀── 16B 对齐保证的根:页对齐起点 + 按块大小整数倍切分

  · >256KB(MAX_BYTES):ThreadCache 直接 malloc/free,绕过三层
  · 池「自有元数据」(②③ 的 map、Span)走 MallocAllocator / Span::operator new=malloc
      → 与全局钩子解耦,防递归(self-contained metadata)
  · 三层单例:ThreadCache = thread_local 函数内 static;CentralCache / PageCache = Meyers
      函数内 static(首次使用时惰性构造,C++11 起线程安全)→ 见 §4.4「静态初始化期分配为何安全」
══════════════════════════════════════════════════════════════════════════════
```

---

<a id="2"></a>
## 2. 环境与构建(可复现)

- 平台:WSL2 Ubuntu 24.04,g++ 13.3,cmake 3.28。
- 工作副本:`~/code/HttpServer`(Linux 原生盘;不在 `/mnt/f` 上编译,避免 9p 跨盘 I/O 拖慢)。
- 依赖:`libboost-dev`(Muduo 的 TcpConnection 上下文用 `boost::any`)、`zlib1g-dev`。
- Muduo:源码编译装到本地 `~/code/muduo/build/release-install-cpp11`(非 `/usr/local`,免 sudo)。
  小补丁:注释掉 Muduo CMakeLists 的 `-Werror`(g++13 对其老代码报一堆告警);`-DMUDUO_BUILD_EXAMPLES=OFF` 只编核心库。
- 内存池:整体拷入 `third_party/memory_pool/`,来源 commit 与本项目改动见该目录 `SOURCE.md`。

```bash
cd ~/code/HttpServer && mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && make -j
./http_server 8080 4        # 端口 8080,4 个 IO 线程
curl -i localhost:8080/     # 200 + JSON
```

**构建开关**(A/B 压测核心):
- `-DUSE_MEMORY_POOL=ON`  → 池版(链入 `GlobalNewDelete.cpp` + 池源码)
- `-DUSE_MEMORY_POOL=OFF` → 基线版(默认 glibc malloc)
- `-DMEMORY_POOL_STATS=ON` → 额外开启池统计计数器(只用于诊断,不用于计时)
- 启动时 stderr 横幅打印当前用的是哪种分配器,便于确认。

---

<a id="3"></a>
## 3. 请求数据流详解(P0)

### 3.1 Muduo 网络层概念(被问到要能解释)

| 概念 | 一句话 |
|------|--------|
| Reactor 模式 | "事件来了再处理",不为每个连接占一个线程死等 |
| EventLoop | 一个线程一个循环,反复 `epoll_wait` 等事件再分发;one loop per thread |
| Poller/epoll | EventLoop 底层调 epoll 的人;muduo 默认 **LT(水平触发)**,可读就一直通知,编程简单不易丢数据 |
| Channel | 一个 fd 的"事件登记表":关心读/写,事件来了调对应回调;不拥有 fd |
| TcpServer | 高层封装:Acceptor 管 accept,为每条连接建 TcpConnection 并分给某 IO 线程 |
| TcpConnection | 一条 TCP 连接:socket fd + Channel + 输入/输出 Buffer + 一个上下文槽(`boost::any`) |
| Buffer | 应用层读写缓冲;**存在的根因:TCP 是字节流,一次 read 不等于一个完整消息** |
| 回调三件套 | ConnectionCallback / MessageCallback / 业务 HttpCallback —— 写服务 = 填这几个回调 |
| one-loop-per-thread | `setThreadNum(n)`:主线程只 accept,n 个子线程各跑一个 EventLoop 处理连接。**一条连接固定属于一个 IO 线程**(内存池线程安全的关键前提) |

### 3.2 半包/粘包:为什么需要 HttpContext(核心难点,务必脱稿)

- **问题**:TCP 是"字节流"不是"消息流"。一次 `onMessage` 中,输入 Buffer 里可能是
  ① 半个请求(头没收全)② 正好一个 ③ 一个半甚至多个挤在一起。
- **解法**:每条连接挂一个 `HttpContext` 状态机,状态 = 解析到哪一步
  (请求行 → 请求头 → 请求体 → 收齐),**跨多次 onMessage 保留**。
- **关键动作**:每步先 `buf->findCRLF()` 找行尾——
  - 找到完整一段 → 处理它,再 `buf->retrieveUntil(crlf+2)` **从 Buffer 消费掉这段**;
    剩下的字节留在 Buffer(这正是"粘包多出来的部分",留待继续解析)。
  - 找不到(数据没到齐)→ 立刻返回**不消费**,等下次 onMessage 带更多数据再从原状态接着解析(处理半包)。
- body 阶段同理:只有 `buf->readableBytes() >= Content-Length` 时才算 body 到齐。
- 一句话:**"只吃能确定完整的那一段,吃不动就留着等下次"** —— 同时解决粘包与半包。

---

<a id="4"></a>
## 4. 内存池接管(P1)

### 4.1 先纠正一个直觉:每请求的高频分配不在"对象"上,而在 STL 容器内部

`HttpRequest` 是 `HttpContext` 的成员、靠 `reset()` 复用;`HttpContext` 每条连接只建一次。
所以**它们不是每请求 new**。真正每请求的高频小对象分配藏在 STL 内部:

| 来源 | 单次大小 | 频率 |
|------|---------|------|
| 请求头 `std::map` 红黑树节点(每个 header 一个) | ~80–112B | 每请求 ×N |
| header 的 `std::string`(超过 15B SSO 的值) | 几十 B | 每请求若干 |
| 响应头 map 节点 + body string(JSON 超 SSO) | 小 | 每请求 |
| **`onRequest` 里局部 `muduo::net::Buffer` 的 vector** | **~1KB** | **每请求 1 个**(单块最大) |
| `boost::any` 装 `HttpContext` | 较大 | 每**连接**一次(低频) |

### 4.2 决策:接在哪一层 / 池化哪些(选项 + 权衡)

- **A 拦截全局 `operator new/delete`(✅采用)**:抓到所有走 `operator new` 的分配——map 节点、
  string、那块 1KB Buffer、boost::any、甚至 muduo 内部全覆盖。覆盖最广、A/B 最干净(链不链入
  一个 TU 即可)。代价:要解决 size 头 + 重入两个工程问题(§4.3/§4.4)。本质是把池当 tcmalloc 式
  malloc 直替来和 glibc 比——诚实预期:glibc 也有 per-thread arena,赢面取决于 workload。
- **B 给热点容器换 pool-backed allocator(未采用,备选)**:只抓显式改类型的容器(请求/响应
  map);**抓不到那块每请求最大的 1KB Buffer**(muduo 的 `vector<char>` allocator 改不了);改
  `std::string`→自定义串类型侵入极大。精准但覆盖小、收益大概率不如 A。
- **C 池化整个对象(HttpContext/连接级,❌否决)**:HttpContext 经 `boost::any` 内部
  `new holder<>` 构造,连重载 `operator new` 都不一定命中,且是**连接级**低频,完全没碰每请求的
  STL churn。收益可忽略,作为被分析否决的朴素方案记录在案。

### 4.3 工程问题①:size 头 + 16B 对齐

- 池的 `deallocate(ptr, size)` **强制要 size**(靠它算 free-list 下标),但全局
  `operator delete(void* p)` 签名**没有 size**。
- 解法:每次分配在用户指针前藏 16B 头,记录"向池申请的字节数 poolReq",释放时读回喂给池。
- 头设 **16 字节**(`alignof(max_align_t)`):`operator new` 必须返回适配任何类型的 16B 对齐指针。
  做法 `poolReq = roundUp16(n+16)` → 池切出的块大小是 16 的倍数 → 块基址 16 对齐(mmap 页 4096
  对齐 + 按块大小整数倍切分;>256KB 走 malloc 本就 16 对齐)→ 用户指针 = 基址+16 仍 16 对齐。
- 只接管"非过对齐"一族;`alignas>16` 的过对齐类型走带 `align_val_t` 的默认重载(new/delete
  自成一对),不与我们的头错配。
- ⚠️ 这个 16B 对齐保证**耦合于池的"页对齐起点 + 按块大小整数倍切分"不变式**(见 §6 限制 4)。

### 4.4 工程问题②:重入 + 自包含元数据

- 把池接成全局 new 后,**池自己管元数据时也会分配**:`PageCache` 的两个 `std::map`
  (freeSpans_/spanMap_)申请红黑树节点 + `new Span`。这些若仍走 `::operator new` →
  "池为了分配 → 又调全局 new → 又进池" → **无限递归**。
- 解法(方案①,采用):让池**自有元数据全部走 malloc**,与"对外提供的分配路径"解耦——
  - 新增 `MallocAllocator`(只用 malloc/free 的自包含 allocator),给 PageCache 两个 map;
  - `Span` 加成员 `operator new/delete`→malloc/free(所有 `new Span` 调用点自动绕开钩子)。
  - **热路径零分支**:稳态分配从 ThreadCache freelist 直接拿,根本不碰这些元数据路径。
- 三层已逐一审计:ThreadCache/CentralCache 只用 `std::array`(内联非堆)+ 大对象直接 malloc,
  无隐式堆分配;stats 的 `print()` 用 `std::vector` 但不在分配热路径上,保留未改。

**Debug-only 重入断言(兜住枚举遗漏)**:全局 new/delete 里加 thread_local 守卫,进池前
`assert(!tl_inPool)`、进池中置位。漏网的嵌套分配会在测试阶段当场炸出来。`#ifndef NDEBUG`
包住 → Release 整段消失、热路径零开销。已验证:Debug 跑混合请求(含触发补货→map 路径)**断言未触发** → 枚举干净。

**静态初始化期分配为何安全(Meyers 单例)**:三层都是函数内 static——
`ThreadCache::getInstance(){ static thread_local ThreadCache instance; }`、
`CentralCache/PageCache::getInstance(){ static X instance; }`。函数内 static **首次使用时惰性
构造**且 C++11 起线程安全,因此**没有"全局对象构造顺序"问题**:即便某次 `operator new` 发生在
`main` 之前的动态初始化期,首次调用会就地懒构造出单例;又因为池自有元数据走 malloc(不再触发
全局 new),不会在自举(bootstrap)阶段递归。两点合起来 → 静态初始化期的分配也安全。

### 4.5 备选方案②:thread_local 递归守卫(考虑过,未采用)

不改 third_party,只在全局钩子常驻一个 thread_local 递归守卫:
`if (inPool) return malloc(size); else { inPool=true; 走池; inPool=false; }`。
- 优点:池源码一行不改,对池内任何分配(含将来新增)都自动安全、最鲁棒。
- 缺点:**每次 new/delete 都多一个 thread_local 分支**(即使稳态不会重入),热路径常驻开销。
- **取舍("自包含元数据 vs 递归守卫")**:②把防递归做成运行时常驻开销;①做成编译期消失的
  Debug 断言 + 一次性元数据改造,**热路径真正零开销**,且"分配器自有元数据必须自包含"本就是
  tcmalloc/jemalloc 的标准做法。故选①,②作为更鲁棒备选记录。

### 4.6 线程安全(one-loop-per-thread 下的契合点)

- Muduo 一条连接固定属于一个 IO 线程,请求对象基本在所属 IO 线程内创建/销毁;池的
  `ThreadCache` 是 thread_local,**每个 IO 线程一套本地自由链表,分配热路径无锁** —— 天然契合。
- 潜在坑:**跨线程归还**(A 线程分配、B 线程释放)。全局 new 接管下谁 free 就归到谁的
  ThreadCache;只要"分配 size == 释放 size"(靠 size 头保证),归还到正确大小类即可,正确性
  **不依赖**同一线程归还(只是块会在不同 ThreadCache 间迁移)。CentralCache 用自旋锁、PageCache
  用 mutex 保护跨线程共享部分。

---

<a id="5"></a>
## 5. 压测对比(P2)

### 5.1 测量方法(可辩护优先,不追求好看的数)
- **两个 workload**:`/plaintext`(低分配,~4 次/请求)+ `/heavy`(分配重,~138 次/请求,POST
  2KB body + 多请求头 → 大 JSON + 多响应头)。覆盖分配强度两端(差 ~35 倍)。
- **并发梯度**:服务端固定 4 IO 线程,wrk 连接数扫 50/200/1000。
- **钉核**:`taskset` 服务端钉 `0-4`、wrk 客户端钉 `6-15`(不相交,避免抢核)。
- **cold/warm**:每个 cell 重启服务(冷态)→ 预热 8s 丢弃 → 计时 3 次 ×30s 取中位数。
- **指标**:QPS + p99/p999(wrk `--latency`,Lua `done()` 取百分位)。
- **stats 单独跑**(开 `MEMORY_POOL_STATS`),SIGUSR1 清零挡预热污染、SIGUSR2 打印;**绝不在
  计时性能测里开 stats**(计时版根本不定义该宏)。
- 脚本:`bench/run_bench.sh`(计时)、`bench/run_stats.sh`(诊断)、`bench/analyze.py`(中位数+CV)。

### 5.2 stats 诊断(c=200,稳态)
| workload | 每请求分配次数 | 本地命中率 | 中心缓存 fetch | 主力 size class |
|----------|--------------|-----------|---------------|----------------|
| /plaintext | ~4 | 99.999% | 528 次(整段) | 1056B(每请求那块 onRequest Buffer 的 vector) |
| /heavy | ~138 | 97.8% | 510 万次 | **112B**(field/value 串 + map 节点) |

### 5.3 A/B 结果(3 次中位数;延迟 ms;loopback)
| workload | 并发 | RPS_on | RPS_off | ΔRPS | p99_on | p99_off | Δp99 |
|----------|------|--------|---------|------|--------|---------|------|
| plaintext | 50  | 660685 | 660372 | +0.0% | 0.307 | 0.285 | +7.7% |
| plaintext | 200 | 812132 | 785738 | **+3.4%** | 0.744 | 0.765 | −2.7% |
| plaintext | 1000| 760672 | 712994 | **+6.7%** | 3.267 | 3.448 | −5.2% |
| heavy | 50  | 158747 | 177787 | **−10.7%** | 0.782 | 0.755 | +3.6% |
| heavy | 200 | 163079 | 175158 | **−6.9%** | 2.642 | 2.682 | −1.5% |
| heavy | 1000| 158256 | 159436 | −0.7% | 10.282 | 11.912 | **−13.7%** |

(全部 36 次运行 `err_status=0` → 服务端从不返非 2xx;高并发下 <0.1% 连接超时。CV:ON 1–5%,
OFF 的 heavy 到 7.4%,故 heavy 差值保守读。)

### 5.4 结论:效果由 workload 决定,**不是一边倒的胜利**(诚实报告)
1. **低分配 + 高并发(plaintext c200/c1000):池小胜**(+3.4% / +6.7%,尾延迟降 3–5%)。机制:
   命中率 99.999%,几乎全走**无锁 thread_local 快路径**,跨核扩展性优于 glibc;中心缓存几乎不参与。
2. **分配重 + 低/中并发(heavy c50/c200):池小输**(−7%~−11%)。机制(最该讲):2.2% miss ×
   138 分配/req = **510 万次中心缓存 fetch 高度集中在单个 size class(112B)**,4 个 IO 线程争抢
   该 class 的**同一把自旋锁** → 锁竞争成瓶颈;且 **16B size 头把 96B map 节点顶进 112B 类**,
   进一步集中、放大竞争。glibc 的 per-thread tcache 处理这种单尺寸高频 churn 更好。
3. **分配重 + 满并发(heavy c1000):吞吐打平,尾延迟明显更好**(p99 −13.7%、p999 −11.4%)。
   饱和时 thread_local 命中让延迟更可预测(此项 OFF 波动大,作提示性结论)。

### 5.5 测量局限(必须如实说)
- **loopback**:无真实网络,测的是 CPU/syscall/分配器路径;真实网络下分配器差异会被网络稀释。
- **WSL2 + P/E 异构核**:虚拟化、核编号不保证对应物理 P/E;ON/OFF 钉同一组核保证对照公平。
- **wrk(非 wrk2)有 coordinated omission**:尾延迟绝对值偏乐观,应读 ON-vs-OFF 相对值。
- 中位数 n=3,heavy 的 OFF 波动较大。

---

<a id="6"></a>
## 6. 已知限制(集中弹药库——面试主动抛,体现你知道边界在哪)

> 这些都是**真实代码现状**(已逐条对着源码核实),不是泛泛而谈。主动讲 = 加分。

1. **畸形 Content-Length 会让进程崩溃(无异常防护)**
   - 现象/根因:`HttpContext.cpp` 解析 body 用 `std::stol(Content-Length)`,**无 try/catch**;
     非数字/超 `long` 范围的值会抛 `std::invalid_argument`/`out_of_range`。而 `HttpServer::onMessage`
     **也没有 try/catch**,异常会穿透 muduo 的回调(muduo 默认不捕获回调异常)→ `std::terminate`
     → **整个进程挂掉**(一个坏请求打挂全服务)。
   - 位置:`src/http/HttpContext.cpp`(两处 `std::stol`)、`src/http/HttpServer.cpp::onMessage`。
   - 可改进:解析前校验全数字 + 范围;或 onMessage 包 try/catch,坏请求回 400 并关连接。

2. **请求体无上限 → 内存 DoS**
   - 现象/根因:body 阶段只等 `readableBytes() >= Content-Length` 才算到齐;一个声明超大
     Content-Length(如 10GB)的请求会让 muduo 把输入 Buffer **一直增长缓冲直到 OOM**,无需真发完。
   - 位置:`src/http/HttpContext.cpp` 的 `kExpectBody` 分支。
   - 可改进:设置 `maxBodySize` 上限,超限直接回 413 Payload Too Large 并关连接。

3. **HTTP pipelining 未真正支持(`if` 非 `while`)**
   - 现象/根因:`onMessage` 是 `if (context->gotAll())` 而非 `while`。若一次读事件里 Buffer 含
     **多个** pipelined 请求,本次只处理**第一个**,其余留在 Buffer,要**等下一次读事件**才继续。
     对普通 keep-alive(发一条等一条响应再发下一条)无影响;对真正的 pipelining 会延迟/停顿。
   - 位置:`src/http/HttpServer.cpp::onMessage`。
   - 可改进:改成 `while (parseRequest && gotAll) { onRequest; reset; }` 循环排空 Buffer。

4. **`operator new` 的 16B 对齐保证耦合于池的切块不变式**
   - 现象/根因:16B 对齐(见 §4.3)依赖两条池的不变式——**span 起点页对齐**(mmap 4096)+
     **size class 块大小是 16 的倍数**(由 `poolReq=roundUp16(...)` 保证)。若将来池改变切块策略
     (块大小非 16 倍数、span 起点非页对齐、或加入非对齐的块头),用户指针的 16B 对齐就会被破坏
     → 过对齐 / SIMD 类型出现 UB。这是个**跨模块的隐式契约**,不是局部就能保证的。
   - 位置:`src/pool/GlobalNewDelete.cpp`(`roundUp16` + 写头)+ `third_party/.../CentralCache.cpp`(切块)。
   - 可改进:在 GlobalNewDelete 里加 `assert((uintptr_t)userptr % 16 == 0)` 兜底;或文档化该契约。

5. **heavy 负载吞吐回退(已定位根因)**
   - 现象/根因:见 §5.4 第 2 点——**单一 size class(112B)中心缓存自旋锁竞争** + **16B size 头把
     96B 顶进 112B 类导致分配集中**。
   - 可改进:见 §7。

---

<a id="7"></a>
## 7. 改进方向(future work,体现深度)

- **size 头导致 class 集中**:把大小存到带外(side table:页/块 → size 的映射)或用更紧凑编码,
  避免把 96B 顶进 112B 类。
- **中心缓存单 class 自旋锁竞争**:对热 class 增大 batch、分片 central free list(多锁)、或抬高
  thread cache 保留阈值(让热 class 更少回中心缓存),降低锁争用。
- **plaintext 每请求最大那块是 1KB 的 onRequest 局部 Buffer**:改用 thread_local 复用 Buffer
  可直接砍掉这块分配。
- **健壮性**:补 §6 的 1/2/3(Content-Length 校验、body 上限、pipelining 循环)。

---

<a id="8"></a>
## 8. 简历话术(可直接用)

> 将自研三层缓存内存池(thread-local 无锁快路径 + 中心缓存 + mmap 页缓存)以全局
> `operator new/delete` 接管接入基于 Muduo(one-loop-per-thread)的 HTTP 服务,解决了接管后
> **分配器元数据自包含**(防与全局钩子递归)、**size 头与 16B 对齐**、**Debug 重入断言兜底**等
> 工程问题。设计 cold/warm 分离、并发梯度(50/200/1000)、钉核的严谨压测,用池自带 stats 量化
> 每请求分配次数与命中率。**诚实结论**:低分配高并发路径吞吐 +3~7%、尾延迟降 3–5%;分配重且
> 集中于单一 size class 时,中心缓存自旋锁竞争使吞吐回退 7–11%,并定位到根因(单 class 锁争用 +
> size 头导致的 class 集中),给出可执行的优化方向。

> 更短的钩子:*"自研内存池接管 Muduo HTTP 服务的全局分配,严谨压测得出 workload 依赖的结论——
> 并能讲清它为何在某些场景回退。"*
