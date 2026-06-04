#!/usr/bin/env bash
# stats 诊断(单独跑,绝不和计时性能测同时进行)。
# 流程:预热 -> SIGUSR1 清零(挡掉预热污染)-> 计时窗口打负载 -> SIGUSR2 打印稳态 stats。
# 再用 allocateCalls / wrk请求数 = 每请求分配次数。
set -u
export HTTP_LOG=fatal   # 只让 stderr 出现 stats 打印,挡住 muduo 连接关闭刷屏
BIN=~/code/HttpServer/build_stats/http_server
PROJ=~/code/HttpServer
SERVER_CORES=0-4
CLIENT_CORES=6-15
PORT=9300
C=200
WARMUP=8
DURATION=20

for wl in plaintext heavy; do
  if [ "$wl" = plaintext ]; then URL="http://127.0.0.1:$PORT/plaintext"; LUA="$PROJ/bench/plaintext.lua";
  else URL="http://127.0.0.1:$PORT/heavy"; LUA="$PROJ/bench/heavy.lua"; fi
  LOG=/tmp/stats_$wl.log
  # 服务端 stdout(muduo 日志)丢弃;stderr(我们的 banner + stats 打印)留到 LOG。
  taskset -c $SERVER_CORES "$BIN" $PORT 4 >/dev/null 2>"$LOG" &
  SRV=$!; sleep 1
  taskset -c $CLIENT_CORES wrk -t4 -c$C -d${WARMUP}s -s "$LUA" "$URL" >/dev/null 2>&1
  kill -USR1 $SRV    # 预热后清零
  RES=$(taskset -c $CLIENT_CORES wrk -t4 -c$C -d${DURATION}s --latency -s "$LUA" "$URL" 2>/dev/null | grep '^RESULT')
  kill -USR2 $SRV; sleep 0.5
  echo "### workload=$wl conns=$C dur=${DURATION}s"
  echo "wrk: $RES"
  grep -A6 "MemoryPoolStats" "$LOG" | tail -7
  echo
  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
done
