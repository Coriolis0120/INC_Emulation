#!/bin/bash
# Barrier 性能基准测试脚本
# 指标: requests/second (每秒请求数)

ROUNDS=10
RESULTS_FILE="./benchmark_barrier_results.csv"

echo "round,time_ms,requests_per_sec" > "$RESULTS_FILE"

echo "=== Barrier Benchmark ==="
echo "Rounds: $ROUNDS"
echo "Metric: requests/second"
echo ""

for round in $(seq 1 $ROUNDS); do
    OUTPUT=$(./test_barrier.sh 30 2>&1)

    if echo "$OUTPUT" | grep -q "Final Result: PASS"; then
        HOST0_OUTPUT=$(echo "$OUTPUT" | sed -n '/--- Host0 ---/,/--- Host1 ---/p')
        TIME_MS=$(echo "$HOST0_OUTPUT" | grep "Barrier completed" | sed 's/.*Time: \([0-9.]*\) ms.*/\1/')
        # 计算 requests/second: 1000 / time_ms
        RPS=$(echo "scale=2; 1000 / $TIME_MS" | bc)
        echo "  Round $round: ${TIME_MS} ms, ${RPS} req/s"
        echo "$round,$TIME_MS,$RPS" >> "$RESULTS_FILE"
    else
        echo "  Round $round: FAIL"
    fi
    sleep 1
done

echo ""
echo "=== Summary ==="
AVG_TIME=$(awk -F',' 'NR>1 {sum+=$2; n++} END {if(n>0) printf "%.3f", sum/n}' "$RESULTS_FILE")
AVG_RPS=$(awk -F',' 'NR>1 {sum+=$3; n++} END {if(n>0) printf "%.2f", sum/n}' "$RESULTS_FILE")
echo "Average Barrier Time: ${AVG_TIME:-N/A} ms"
echo "Average Requests/Second: ${AVG_RPS:-N/A} req/s"
