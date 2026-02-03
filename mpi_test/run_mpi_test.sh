#!/bin/bash
# MPI Collective Test Script - 自动化部署和日志收集
#
# 用法: ./run_mpi_test.sh [SIZE_KB] [ITERATIONS] [TEST_TYPE] [RUNS]
# 示例: ./run_mpi_test.sh 1024 10 all 10

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG_DIR="$SCRIPT_DIR/logs"
HOSTFILE="$SCRIPT_DIR/hostfile"

# 清理函数 - 收集日志并停止进程
cleanup() {
    echo ""
    echo "=== Collecting Logs ==="
    ssh switch 'cat ~/soft_switch.log' > "$LOG_DIR/switch_soft.log" 2>/dev/null
    ssh vm1 'cat ~/soft_switch.log' > "$LOG_DIR/vm1_soft.log" 2>/dev/null
    ssh vm2 'cat ~/soft_switch.log' > "$LOG_DIR/vm2_soft.log" 2>/dev/null
    echo "Logs saved to $LOG_DIR/"

    echo ""
    echo "=== Stopping soft_switch ==="
    for node in switch vm1 vm2; do
        ssh $node 'pkill -9 soft_switch' 2>/dev/null
    done
    echo "Done."
}

# 捕获退出信号，确保收集日志
trap cleanup EXIT

# 参数
SIZE_KB=${1:-1024}
ITERATIONS=${2:-10}
TEST_TYPE=${3:-all}
RUNS=${4:-1}

echo "=== MPI Collective Test ==="
echo "Size: ${SIZE_KB} KB"
echo "Iterations: ${ITERATIONS}"
echo "Test type: ${TEST_TYPE}"
echo "Runs to average: ${RUNS}"
echo ""

# 创建日志目录
mkdir -p "$LOG_DIR"
rm -f "$LOG_DIR"/*.log

# ========== 1. 停止旧进程 ==========
echo "[1/5] Stopping old processes..."
for node in switch vm1 vm2; do
    ssh $node 'pkill -9 soft_switch; rm -f ~/soft_switch' 2>/dev/null
done
for node in pku1 pku2 pku3 pku4; do
    ssh $node 'pkill -9 mpi_collective; pkill -9 orted' 2>/dev/null
done
sleep 2

# ========== 2. 编译并部署 soft_switch ==========
echo "[2/5] Building and deploying soft_switch..."
cd "$SCRIPT_DIR"
gcc -o soft_switch soft_switch.c -lpcap -lpthread 2>/dev/null
if [ ! -f soft_switch ]; then
    echo "ERROR: Failed to compile soft_switch"
    exit 1
fi

scp -q soft_switch switch:~/
scp -q soft_switch vm1:~/
scp -q soft_switch vm2:~/

# ========== 3. 启动 soft_switch ==========
echo "[3/5] Starting soft_switch on all nodes..."

# 禁用 TSO/GSO 以避免大包问题
for node in switch vm1 vm2; do
    ssh -o ConnectTimeout=5 $node 'ethtool -K eth1 tso off gso off gro off 2>/dev/null; ethtool -K eth2 tso off gso off gro off 2>/dev/null' &
done
wait

ssh -o ConnectTimeout=5 -f switch 'cd ~ && ./soft_switch spine > soft_switch.log 2>&1'
ssh -o ConnectTimeout=5 -f vm1 'cd ~ && ./soft_switch leaf1 > soft_switch.log 2>&1'
ssh -o ConnectTimeout=5 -f vm2 'cd ~ && ./soft_switch leaf2 > soft_switch.log 2>&1'
sleep 2

# 验证 soft_switch 是否启动
SW_OK=1
for node in switch vm1 vm2; do
    echo "  Checking $node..."
    if ! ssh -o ConnectTimeout=5 $node 'pgrep soft_switch' >/dev/null 2>&1; then
        echo "WARNING: soft_switch not running on $node"
        SW_OK=0
    else
        echo "  $node: OK"
    fi
done

if [ $SW_OK -eq 0 ]; then
    echo "ERROR: soft_switch failed to start"
    ssh switch 'cat ~/soft_switch.log' > "$LOG_DIR/switch_soft.log" 2>/dev/null
    ssh vm1 'cat ~/soft_switch.log' > "$LOG_DIR/vm1_soft.log" 2>/dev/null
    ssh vm2 'cat ~/soft_switch.log' > "$LOG_DIR/vm2_soft.log" 2>/dev/null
    exit 1
fi
echo "  soft_switch started successfully"

# ========== 4. 检查网络连通性 ==========
echo "[4/5] Checking network connectivity..."
# 添加跨子网路由
ssh -o ConnectTimeout=5 pku1 'ip route add 10.2.1.0/24 via 10.1.1.25 dev eth1 2>/dev/null'
ssh -o ConnectTimeout=5 pku2 'ip route add 10.2.1.0/24 via 10.1.1.25 dev eth1 2>/dev/null'
ssh -o ConnectTimeout=5 pku3 'ip route add 10.1.1.0/24 via 10.2.1.18 dev eth1 2>/dev/null'
ssh -o ConnectTimeout=5 pku4 'ip route add 10.1.1.0/24 via 10.2.1.18 dev eth1 2>/dev/null'

echo "  Testing ping pku1 -> pku3..."
PING_OK=$(ssh -o ConnectTimeout=5 pku1 'ping -c 1 -W 3 10.2.1.7 2>/dev/null | grep -c "1 received"')
if [ "$PING_OK" = "1" ]; then
    echo "  Network OK: pku1 -> pku3"
else
    echo "  WARNING: Cross-subnet ping failed"
fi

# ========== 5. 运行 MPI 测试 ==========
echo "[5/5] Running MPI test..."

# 部署 mpi_collective_test 到 pku 节点
echo "  Deploying mpi_collective_test to pku nodes..."
for node in pku1 pku2 pku3 pku4; do
    scp -q $SCRIPT_DIR/mpi_collective_test $node:~/
done

# 部署 hostfile 到 pku1
scp -q $HOSTFILE pku1:~/hostfile

echo ""

# 在 pku1 上运行 mpirun
ssh pku1 "mpirun --allow-run-as-root \
    -np 4 \
    -hostfile ~/hostfile \
    --mca btl tcp,self \
    --mca btl_tcp_if_include eth1 \
    --mca orte_base_help_aggregate 0 \
    ~/mpi_collective_test -s $SIZE_KB -i $ITERATIONS -t $TEST_TYPE -r $RUNS" \
    2>&1 | tee "$LOG_DIR/mpi_output.log"

MPI_EXIT=${PIPESTATUS[0]}

echo ""
echo "=== Done (exit code: $MPI_EXIT) ==="
