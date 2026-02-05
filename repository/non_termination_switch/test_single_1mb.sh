#!/bin/bash
# Single 1MB Reduce Test Script (with compile and deploy)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
MAIN_BUILD_DIR="$SCRIPT_DIR/../build"
LOG_DIR="$SCRIPT_DIR/logs/single_1mb_$(date +%Y%m%d_%H%M%S)"

WORLD_SIZE=4
CONTROLLER_IP="192.168.0.3"
MASTER_IP="192.168.0.14"
ROOT_RANK=0
DATA_COUNT=268435456
TIMEOUT=900

mkdir -p "$LOG_DIR"

echo "=========================================="
echo "Single 1MB Reduce Test (True Reduce)"
echo "Data count: $DATA_COUNT (1MB)"
echo "Timeout: ${TIMEOUT}s"
echo "Log dir: $LOG_DIR"
echo "=========================================="

# Step 0: Kill old processes first (to avoid "Text file busy" during deploy)
echo ""
echo "[Step 0] Killing old processes before compile..."
for host in controller switch vm1 vm2; do
    ssh $host "pkill -9 switch_reduce; pkill -9 controller" 2>/dev/null &
done
for host in pku1 pku2 pku3 pku4; do
    ssh $host "pkill -9 host_reduce_test_v2" 2>/dev/null &
done
wait
sleep 3
echo "Done"

# Step 0.5: Compile and deploy
echo ""
echo "[Step 0.5] Compiling and deploying..."

# Compile switch_reduce
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. > /dev/null 2>&1
make -j4 2>&1 | tail -3
if [ ! -f "$BUILD_DIR/switch_reduce" ]; then
    echo "ERROR: switch_reduce compilation failed"
    exit 1
fi

# Compile host_reduce_test_v2
cd "$MAIN_BUILD_DIR"
make host_reduce_test_v2 -j4 2>&1 | tail -3
if [ ! -f "$MAIN_BUILD_DIR/host_reduce_test_v2" ]; then
    echo "ERROR: host_reduce_test_v2 compilation failed"
    exit 1
fi

# Deploy to switches
echo "Deploying switch_reduce..."
for host in switch vm1 vm2; do
    scp "$BUILD_DIR/switch_reduce" "$host:/root/" &
done
wait

# Deploy to hosts
echo "Deploying host_reduce_test_v2..."
for host in pku1 pku2 pku3 pku4; do
    scp "$MAIN_BUILD_DIR/host_reduce_test_v2" "$host:/root/" &
done
wait
echo "Deployment complete."

cd "$SCRIPT_DIR"

# Step 1: Kill old processes
echo ""
echo "[Step 1] Killing old processes..."
for host in controller switch vm1 vm2; do
    ssh $host "pkill -9 switch_reduce; pkill -9 controller" 2>/dev/null &
done
for host in pku1 pku2 pku3 pku4; do
    ssh $host "pkill -9 host_reduce_test_v2" 2>/dev/null &
done
wait
sleep 3  # Wait longer for ports to be released
echo "Done"

# Step 2: Start Controller
echo ""
echo "[Step 2] Starting Controller..."
ssh controller "cd /root && stdbuf -oL ./controller > /root/controller.log 2>&1" &
sleep 2

# Step 3: Start Switches
echo ""
echo "[Step 3] Starting Switches..."
ssh switch "cd /root && stdbuf -oL ./switch_reduce $CONTROLLER_IP 0 > /root/switch.log 2>&1" &
sleep 1
ssh vm1 "cd /root && stdbuf -oL ./switch_reduce $CONTROLLER_IP 1 > /root/switch.log 2>&1" &
sleep 1
ssh vm2 "cd /root && stdbuf -oL ./switch_reduce $CONTROLLER_IP 2 > /root/switch.log 2>&1" &
sleep 2

# Step 4: Start Hosts
echo ""
echo "[Step 4] Starting Hosts..."
ssh pku1 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_reduce_test_v2 $WORLD_SIZE $MASTER_IP 0 $ROOT_RANK $DATA_COUNT > /root/host.log 2>&1" &
sleep 3  # Wait longer for rank0 to be ready
ssh pku2 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_reduce_test_v2 $WORLD_SIZE $MASTER_IP 1 $ROOT_RANK $DATA_COUNT > /root/host.log 2>&1" &
sleep 1
ssh pku3 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_reduce_test_v2 $WORLD_SIZE $MASTER_IP 2 $ROOT_RANK $DATA_COUNT > /root/host.log 2>&1" &
sleep 1
ssh pku4 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_reduce_test_v2 $WORLD_SIZE $MASTER_IP 3 $ROOT_RANK $DATA_COUNT > /root/host.log 2>&1" &

# Step 5: Wait for completion
echo ""
echo "[Step 5] Waiting for completion (timeout: ${TIMEOUT}s)..."
ELAPSED=0
COMPLETED=0
while [ $ELAPSED -lt $TIMEOUT ]; do
    sleep 2
    ELAPSED=$((ELAPSED + 2))

    H0=$(ssh pku1 "grep -q 'Test Complete' /root/host.log 2>/dev/null && echo 1 || echo 0")
    H1=$(ssh pku2 "grep -q 'Test Complete' /root/host.log 2>/dev/null && echo 1 || echo 0")
    H2=$(ssh pku3 "grep -q 'Test Complete' /root/host.log 2>/dev/null && echo 1 || echo 0")
    H3=$(ssh pku4 "grep -q 'Test Complete' /root/host.log 2>/dev/null && echo 1 || echo 0")

    echo "  ${ELAPSED}s: H0=$H0 H1=$H1 H2=$H2 H3=$H3"

    if [ "$H0" = "1" ] && [ "$H1" = "1" ] && [ "$H2" = "1" ] && [ "$H3" = "1" ]; then
        COMPLETED=1
        break
    fi
done

# Step 6: Stop processes
echo ""
echo "[Step 6] Stopping processes..."
jobs -p | xargs -r kill -9 2>/dev/null
for host in controller switch vm1 vm2; do
    ssh $host "pkill -9 switch_reduce; pkill -9 controller" 2>/dev/null
done
for host in pku1 pku2 pku3 pku4; do
    ssh $host "pkill -9 host_reduce_test_v2" 2>/dev/null
done

# Step 7: Collect logs
echo ""
echo "[Step 7] Collecting logs..."
ssh controller "cat /root/controller.log 2>/dev/null" > "$LOG_DIR/controller.log"
ssh switch "cat /root/switch.log 2>/dev/null" > "$LOG_DIR/switch0_spine.log"
ssh vm1 "cat /root/switch.log 2>/dev/null" > "$LOG_DIR/switch1_leaf1.log"
ssh vm2 "cat /root/switch.log 2>/dev/null" > "$LOG_DIR/switch2_leaf2.log"
ssh pku1 "cat /root/host.log 2>/dev/null" > "$LOG_DIR/host0_pku1.log"
ssh pku2 "cat /root/host.log 2>/dev/null" > "$LOG_DIR/host1_pku2.log"
ssh pku3 "cat /root/host.log 2>/dev/null" > "$LOG_DIR/host2_pku3.log"
ssh pku4 "cat /root/host.log 2>/dev/null" > "$LOG_DIR/host3_pku4.log"

# Step 8: Show results
echo ""
echo "=========================================="
echo "RESULTS"
echo "=========================================="

if [ $COMPLETED -eq 1 ]; then
    echo "Status: COMPLETED"
    grep -E "Throughput|PASS|FAIL|ERROR|TIMEOUT" "$LOG_DIR/host0_pku1.log"
else
    echo "Status: TIMEOUT after ${TIMEOUT}s"
    echo ""
    echo "--- Host 0 (root) last lines ---"
    tail -10 "$LOG_DIR/host0_pku1.log"
fi

echo ""
echo "--- Key statistics ---"
echo "Spine recv from vm1: $(grep -c 'ingress_conn=0.*pkt_len=1082' "$LOG_DIR/switch0_spine.log" 2>/dev/null || echo 0)"
echo "Spine recv from vm2: $(grep -c 'ingress_conn=1.*pkt_len=1082' "$LOG_DIR/switch0_spine.log" 2>/dev/null || echo 0)"
echo "vm2 recv from pku3: $(grep -c 'DATA packet.*port=1' "$LOG_DIR/switch2_leaf2.log" 2>/dev/null || echo 0)"
echo "vm2 recv from pku4: $(grep -c 'DATA packet.*port=2' "$LOG_DIR/switch2_leaf2.log" 2>/dev/null || echo 0)"

echo ""
echo "Logs saved to: $LOG_DIR"
echo "=========================================="
