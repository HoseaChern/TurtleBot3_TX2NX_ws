# 测试一：Jazzy 节点通信

## 改动目的

验证 arm64 Jazzy 环境在 NX 内核 4.9.337 上的运行能力，以及 ROS 2 发布-订阅通信功能。该测试验证 Ubuntu 24.04 用户态与宿主的兼容性，不涉及 GPU。

## 改动位置

- `src/minimal_test/`：ROS 2 包，含发布者与订阅者节点。
- `docker/jazzy_minimal/Dockerfile` 与 `entrypoint.sh`。

## 预期效果

- 内核兼容性冒烟：容器内 `uname -r` 输出 `4.9.337`，`/etc/os-release` 为 Ubuntu 24.04。
- 通信：发布频率 $f = 10$ Hz，持续 $T = 30$ s，收到消息数 $N \ge 0.95 f T$，即 $N \ge 285$。

## 实际结果

待填。

## 结论

待填。
