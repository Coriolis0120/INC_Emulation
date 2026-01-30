#!/bin/bash
# INC System Stop Script - 1-2-4 Topology

echo "=== Stopping INC System (1-2-4 Topology) ==="

# 停止 Host 进程 (pku1-4)
echo "Stopping hosts..."
ssh pku1 "pkill -9 -f 'host' 2>/dev/null" || true
ssh pku2 "pkill -9 -f 'host' 2>/dev/null" || true
ssh pku3 "pkill -9 -f 'host' 2>/dev/null" || true
ssh pku4 "pkill -9 -f 'host' 2>/dev/null" || true

# 停止 Switch 进程 (switch, vm1, vm2)
echo "Stopping switches..."
ssh switch "pkill -9 -f 'switch' 2>/dev/null" || true
ssh vm1 "pkill -9 -f 'switch' 2>/dev/null" || true
ssh vm2 "pkill -9 -f 'switch' 2>/dev/null" || true

# 停止 Controller 进程
echo "Stopping controller..."
ssh controller "pkill -9 -f 'controller' 2>/dev/null" || true

sleep 5
echo "=== All processes stopped ==="
