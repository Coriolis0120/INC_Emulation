# INC系统部署完成

## 已完成的配置修改

### 1. MAC地址更新
- vm1 eth1: `fa:16:3e:87:09:4c`
- vm2 eth1: `fa:16:3e:7b:25:36`

### 2. 网络配置更新
- Switch管理IP: `192.168.0.19`
- Switch接口名称: eth1, eth2
- YAML输出路径: `/root/topology.yaml`

### 3. 已部署的文件
- controller节点: `/root/controller`
- switch节点: `/root/switch`
- vm1节点: `/root/host`
- vm2节点: `/root/host`

## 使用方法

### 启动系统
```bash
cd /root/NetLab/INC_Emulation
./start_inc.sh
```

### 停止系统
```bash
cd /root/NetLab/INC_Emulation
./stop_inc.sh
```

### 查看日志
```bash
# Controller日志
ssh controller 'tail -f /root/controller.log'

# Switch日志
ssh switch 'tail -f /root/switch.log'

# VM1日志
ssh vm1 'tail -f /root/host_rank0.log'

# VM2日志
ssh vm2 'tail -f /root/host_rank1.log'
```
