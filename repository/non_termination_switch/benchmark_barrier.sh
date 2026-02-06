#!/bin/bash
# Barrier 性能基准测试脚本

ROUNDS=10
RESULTS_FILE="./benchmark_barrier_results.csv"

echo "round,time_ms" > "$RESULTS_FILE"

echo "=== Barrier Benchmark ==="
echo "Rounds: $ROUNDS"
echo ""

for round in $(seq 1 $ROUNDS); do
    OUTPUT=$(./test_barrier.sh 30 2>&1)

    if echo "$OUTPUT" | grep -q "Final Result: PASS"; then
        HOST0_OUTPUT=$(echo "$OUTPUT" | sed -n '/--- Host0 ---/,/--- Host1 ---/p')
        TIME_MS=$(echo "$HOST0_OUTPUT" | grep "Barrier completed" | sed 's/.*Time: \([0-9.]*\) ms.*/\1/')
        echo "  Round $round: ${TIME_MS} ms"
        echo "$round,$TIME_MS" >> "$RESULTS_FILE"
    else
        echo "  Round $round: FAIL"
    fi
    sleep 1
done

echo ""
echo "=== Summary ==="
AVG=$(awk -F',' 'NR>1 {sum+=$2; n++} END {if(n>0) printf "%.3f", sum/n}' "$RESULTS_FILE")
echo "Average Barrier Time: ${AVG:-N/A} ms"
