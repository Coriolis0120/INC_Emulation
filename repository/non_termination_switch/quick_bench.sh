#!/bin/bash
# Benchmark using test_single_1mb.sh as base
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Modify DATA_COUNT in test script and run
run_size() {
    local SIZE=$1
    local NAME=$2
    local TIMEOUT=$3

    # Update test script
    sed -i "s/^DATA_COUNT=.*/DATA_COUNT=$SIZE/" test_single_1mb.sh
    sed -i "s/^TIMEOUT=.*/TIMEOUT=$TIMEOUT/" test_single_1mb.sh

    # Run and extract result
    ./test_single_1mb.sh 2>&1 | grep -E "Throughput:|PASS|FAIL" | head -2
}

echo "=== Benchmark Started $(date) ==="

# Test each size 10 times
for SIZE_INFO in "1024:4KB:30" "4096:16KB:30" "16384:64KB:30" "65536:256KB:60" "262144:1MB:60" "1048576:4MB:120" "4194304:16MB:180" "16777216:64MB:300" "67108864:256MB:600" "268435456:1GB:900"; do
    IFS=':' read -r SIZE NAME TIMEOUT <<< "$SIZE_INFO"
    echo ""
    echo "=== $NAME ==="

    for i in $(seq 1 10); do
        echo -n "  #$i: "
        run_size $SIZE $NAME $TIMEOUT
    done
done

echo ""
echo "=== Completed $(date) ==="
