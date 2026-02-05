#!/bin/bash
# 检查各节点的丢包统计

echo "=== Network Interface Drop Stats ==="
for host in pku1 pku2 pku3 pku4 vm1 vm2 switch; do
    echo "--- $host ---"
    ssh $host "cat /sys/class/net/eth1/statistics/rx_dropped 2>/dev/null | xargs echo 'rx_dropped:'"
    ssh $host "cat /sys/class/net/eth1/statistics/rx_errors 2>/dev/null | xargs echo 'rx_errors:'"
    ssh $host "cat /sys/class/net/eth1/statistics/rx_missed_errors 2>/dev/null | xargs echo 'rx_missed:'"
done

echo ""
echo "=== Soft-RoCE Stats (pku1) ==="
ssh pku1 "ls /sys/class/infiniband/rxe_eth1/ports/1/hw_counters/ 2>/dev/null"
ssh pku1 "cat /sys/class/infiniband/rxe_eth1/ports/1/hw_counters/* 2>/dev/null | head -20"

echo ""
echo "=== RDMA Device Stats (pku1) ==="
ssh pku1 "rdma statistic show 2>/dev/null | head -20"
