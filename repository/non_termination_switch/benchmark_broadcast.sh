#!/bin/bash
# Broadcast 性能基准测试脚本

ROUNDS=10
RESULTS_FILE="./benchmark_results.csv"

echo "data_size_kb,round,time_ms,throughput_mbps" > "$RESULTS_FILE"

# 数据量: 4KB, 16KB, 64KB, 256KB, 1MB, 4MB, 16MB, 64MB, 256MB, 1GB
DATA_SIZES=(1024 4096 16384 65536 262144 1048576 4194304 16777216 67108864 268435456)
SIZE_NAMES=("4KB" "16KB" "64KB" "256KB" "1MB" "4MB" "16MB" "64MB" "256MB" "1GB")

for idx in "${!DATA_SIZES[@]}"; do
    DATA_COUNT=${DATA_SIZES[$idx]}
    SIZE_NAME=${SIZE_NAMES[$idx]}
    SIZE_KB=$((DATA_COUNT * 4 / 1024))

    echo "=== Testing: $SIZE_NAME ==="

    for round in $(seq 1 $ROUNDS); do
        if [ $DATA_COUNT -gt 67108864 ]; then TIMEOUT=120
        elif [ $DATA_COUNT -gt 16777216 ]; then TIMEOUT=60
        else TIMEOUT=30; fi

        OUTPUT=$(./test_broadcast.sh $DATA_COUNT $TIMEOUT 2>&1)

        if echo "$OUTPUT" | grep -q "Final Result: PASS"; then
            # 从 Host0 的输出中提取 "Broadcast completed, Time: X ms, Throughput: Y Mbps"
            HOST0_OUTPUT=$(echo "$OUTPUT" | sed -n '/--- Host0 ---/,/--- Host1 ---/p')
            TIME_MS=$(echo "$HOST0_OUTPUT" | grep "Broadcast completed" | sed 's/.*Time: \([0-9]*\) ms.*/\1/')
            THROUGHPUT=$(echo "$HOST0_OUTPUT" | grep "Broadcast completed" | sed 's/.*Throughput: \([0-9.]*\) Mbps.*/\1/')
            echo "  Round $round: ${TIME_MS}ms, ${THROUGHPUT} Mbps"
            echo "$SIZE_KB,$round,$TIME_MS,$THROUGHPUT" >> "$RESULTS_FILE"
        else
            echo "  Round $round: FAIL"
        fi
        sleep 1
    done
done

echo "=== Summary ==="
for idx in "${!DATA_SIZES[@]}"; do
    SIZE_KB=$((DATA_SIZES[$idx] * 4 / 1024))
    AVG=$(awk -F',' -v sk="$SIZE_KB" '$1==sk {sum+=$4; n++} END {if(n>0) printf "%.2f", sum/n}' "$RESULTS_FILE")
    echo "${SIZE_NAMES[$idx]}: ${AVG:-N/A} Mbps"
done
