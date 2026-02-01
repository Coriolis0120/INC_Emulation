#!/bin/bash
# MPI 集合通信测试脚本

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HOSTFILE="$SCRIPT_DIR/hostfile"

# 默认参数
SIZE_KB=${1:-1024}
ITERATIONS=${2:-10}
TEST_TYPE=${3:-all}

echo "=== MPI Collective Test ==="
echo "Size: ${SIZE_KB} KB"
echo "Iterations: ${ITERATIONS}"
echo "Test type: ${TEST_TYPE}"
echo ""

mpirun --allow-run-as-root \
    -np 4 \
    -hostfile $HOSTFILE \
    --mca btl tcp,self \
    $SCRIPT_DIR/mpi_collective_test -s $SIZE_KB -i $ITERATIONS -t $TEST_TYPE
