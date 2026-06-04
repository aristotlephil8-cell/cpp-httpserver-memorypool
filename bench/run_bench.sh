#!/usr/bin/env bash
# 性能压测 harness(计时版,绝不开 stats)。
# 用法: ./run_bench.sh <server_bin> <label> <outfile>
# 设计要点(对应测量方案):
#   - 服务端固定 4 IO 线程,taskset 钉核 0-4;wrk 客户端钉核 6-15(不相交,避免抢核)
#   - 每个 (workload, 并发) 组合:重启服务(冷态)-> 预热 8s 丢弃 -> 3 次 ×30s 计时
#   - 并发梯度 50/200/1000;两个 workload:plaintext(低分配)/ heavy(分配重)
set -u
export HTTP_LOG=fatal   # 压住 muduo 连接关闭的 ERROR 刷屏,避免日志 I/O 污染测量
BIN=$1; LABEL=$2; OUT=$3
PROJ=~/code/HttpServer
SERVER_CORES=0-4
CLIENT_CORES=6-15
PORT=9100
WARMUP=8
DURATION=30
REPS=3
CONNS="50 200 1000"
WRK_THREADS=4

: > "$OUT"
for wl in plaintext heavy; do
  if [ "$wl" = "plaintext" ]; then URL="http://127.0.0.1:$PORT/plaintext"; LUA="$PROJ/bench/plaintext.lua";
  else URL="http://127.0.0.1:$PORT/heavy"; LUA="$PROJ/bench/heavy.lua"; fi
  for c in $CONNS; do
    taskset -c $SERVER_CORES "$BIN" $PORT 4 >/dev/null 2>&1 &
    SRV=$!
    sleep 1
    # 预热(丢弃)
    taskset -c $CLIENT_CORES wrk -t$WRK_THREADS -c$c -d${WARMUP}s --latency -s "$LUA" "$URL" >/dev/null 2>&1
    # 计时(取多次)
    for r in $(seq 1 $REPS); do
      res=$(taskset -c $CLIENT_CORES wrk -t$WRK_THREADS -c$c -d${DURATION}s --latency -s "$LUA" "$URL" 2>/dev/null | grep '^RESULT')
      echo "$LABEL wl=$wl conns=$c rep=$r $res"
      echo "$LABEL wl=$wl conns=$c rep=$r $res" >> "$OUT"
    done
    kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
  done
done
echo "DONE $LABEL -> $OUT"
