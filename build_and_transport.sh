#!/bin/bash
# INC Build and Transport Script - 1-2-4 Topology
set -e

cd /root/NetLab/new/INC_Emulation/repository

# 确保build目录存在
mkdir -p build
cd build
cmake ..

# 编译目标
echo "=== Building targets ==="
cmake --build . --target switch_v2
cmake --build . --target controller
cmake --build . --target host_simple_test
cmake --build . --target host_reduce
cmake --build . --target host_broadcast
cmake --build . --target host_barrier

cd /root/NetLab/new/INC_Emulation

echo ""
echo "=== Deploying to nodes ==="

# Controller
echo "[1/7] Deploying controller..."
ssh controller "rm -f /root/controller"
rsync -av repository/build/controller controller:/root/

# Top Switch (switch)
echo "[2/7] Deploying top switch..."
ssh switch "rm -f /root/switch_v2"
rsync -av repository/build/switch_v2 switch:/root/

# Leaf Switch 1 (vm1)
echo "[3/7] Deploying leaf switch 1 (vm1)..."
ssh vm1 "rm -f /root/switch_v2"
rsync -av repository/build/switch_v2 vm1:/root/

# Leaf Switch 2 (vm2)
echo "[4/7] Deploying leaf switch 2 (vm2)..."
ssh vm2 "rm -f /root/switch_v2"
rsync -av repository/build/switch_v2 vm2:/root/

# Host programs to pku1-4
echo "[5/7] Deploying to pku1..."
ssh pku1 "rm -f /root/host_simple_test /root/host_reduce /root/host_broadcast /root/host_barrier"
rsync -av repository/build/host_simple_test pku1:/root/
rsync -av repository/build/host_reduce pku1:/root/
rsync -av repository/build/host_broadcast pku1:/root/
rsync -av repository/build/host_barrier pku1:/root/

echo "[6/7] Deploying to pku2..."
ssh pku2 "rm -f /root/host_simple_test /root/host_reduce /root/host_broadcast /root/host_barrier"
rsync -av repository/build/host_simple_test pku2:/root/
rsync -av repository/build/host_reduce pku2:/root/
rsync -av repository/build/host_broadcast pku2:/root/
rsync -av repository/build/host_barrier pku2:/root/

echo "[7/7] Deploying to pku3 and pku4..."
ssh pku3 "rm -f /root/host_simple_test /root/host_reduce /root/host_broadcast /root/host_barrier"
rsync -av repository/build/host_simple_test pku3:/root/
rsync -av repository/build/host_reduce pku3:/root/
rsync -av repository/build/host_broadcast pku3:/root/
rsync -av repository/build/host_barrier pku3:/root/
ssh pku4 "rm -f /root/host_simple_test /root/host_reduce /root/host_broadcast /root/host_barrier"
rsync -av repository/build/host_simple_test pku4:/root/
rsync -av repository/build/host_reduce pku4:/root/
rsync -av repository/build/host_broadcast pku4:/root/
rsync -av repository/build/host_barrier pku4:/root/

echo ""
echo "=== Build and transport completed ==="
