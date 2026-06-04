-- workload: 分配重。POST /heavy,带长 body(2KB)+ 多个请求头。
-- 触发:请求体 string 分配(解析端) + 大 JSON 组装 + 多响应头(响应端)。
wrk.method = "POST"
wrk.body = string.rep("x", 2048)
wrk.headers["Content-Type"] = "text/plain"
wrk.headers["X-H1"] = "header-value-one-aaaaaaaaaaaaaaaaaaaa"
wrk.headers["X-H2"] = "header-value-two-bbbbbbbbbbbbbbbbbbbb"
wrk.headers["X-H3"] = "header-value-three-cccccccccccccccccc"
wrk.headers["X-H4"] = "header-value-four-dddddddddddddddddddd"

done = function(summary, latency, requests)
  local rps = summary.requests / (summary.duration / 1e6)
  io.write(string.format(
    "RESULT rps=%.1f p50=%.3f p90=%.3f p99=%.3f p999=%.3f max=%.3f reqs=%d dur_s=%.1f err_connect=%d err_read=%d err_write=%d err_status=%d err_timeout=%d\n",
    rps,
    latency:percentile(50)/1000.0,
    latency:percentile(90)/1000.0,
    latency:percentile(99)/1000.0,
    latency:percentile(99.9)/1000.0,
    latency:percentile(100)/1000.0,
    summary.requests, summary.duration/1e6,
    summary.errors.connect, summary.errors.read, summary.errors.write,
    summary.errors.status, summary.errors.timeout))
end
