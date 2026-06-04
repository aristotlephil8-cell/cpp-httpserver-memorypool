-- workload: 低分配。GET /plaintext,带一个请求头。
wrk.method = "GET"
wrk.headers["X-Test"] = "plaintext"

-- done():压测结束时打印一行可解析结果(QPS + 百分位延迟[ms] + 错误数)。
-- 用 wrk 聚合后的 latency 直方图取 P99.9。
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
