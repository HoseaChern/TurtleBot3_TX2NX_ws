# TurtleBot3 TX2 NX 开发环境

本仓库部署 TurtleBot3 在 Jetson TX2 NX 上的 ROS 2 开发环境。PC 负责编码与训练，NX 负责原生编译与推理。全部部署细节见 `docs/Env_Plan.md`。

## 基本环境

| 设备 | 角色      | 系统                                  | 关键版本                                           |
| ---- | --------- | ------------------------------------- | -------------------------------------------------- |
| PC   | 编码/训练 | Ubuntu 24.04 amd64                    | ROS 2 Jazzy、CUDA 12.9、Python 3.12、PyTorch cu128 |
| NX   | 编译/推理 | Ubuntu 18.04 aarch64（JetPack 4.6.6） | CUDA 10.2、TensorRT 8.2、内核 4.9.337              |

分工：

- PC：用 VSCode 编写 ROS 2 代码，训练 YOLO，导出 ONNX。
- NX：原生构建 arm64 镜像，将 ONNX 转为 TensorRT engine，运行 C++ 推理节点。

两机用点对点千兆网线直连（`192.168.60.0/24`），源码与模型经 `rsync` over SSH 同步。分工与选型的依据见 `docs/Env_Plan.md` 第 1 章。

## 目录结构

```text
TurtleBot3_TX2NX_ws/
├── docs/          计划与设计文档
├── docker/        各测试的镜像定义（jazzy_minimal、cuda_test）
├── models/        训练侧导出的 ONNX 与转换后的 engine
├── scripts/       环境配置与同步脚本
└── src/           ROS 2 包
```

## 每日启动

### 1. 直连网络

直连静态 IP 在重启后不保留，开机后需重配。PC 端：

```bash
sudo ip addr add 192.168.60.1/24 dev enp8s0
sudo ip link set enp8s0 up
```

NX 端（网口名以 `ip -br link` 为准）：

```bash
sudo ip addr add 192.168.60.2/24 dev eth0
sudo ip link set eth0 up
```

在 PC 上验证连通：

```bash
ping -c 3 192.168.60.2
```

### 2. 网络代理

在 PC 上启动 Clash Verge 并开启 Allow LAN。NX 的 `dockerd` 代理已由 systemd drop-in 持久化（`/etc/systemd/system/docker.service.d/proxy.conf`，指向 `192.168.60.1:7897`），无需每次重配，只要 PC 的 Clash 在运行。

`docker login`、`docker buildx` 一类的 CLI 操作读的是 shell 代理变量，需在当前会话设置（大写优先）：

```bash
export HTTP_PROXY=http://192.168.60.1:7897
export HTTPS_PROXY=http://192.168.60.1:7897
export http_proxy=http://192.168.60.1:7897
export https_proxy=http://192.168.60.1:7897
```

拉取 `nvcr.io` 的镜像需先登录 NGC（用户名 `$oauthtoken`，密码为 NGC API Key），凭据存入 `~/.docker/config.json` 后长期有效：

```bash
echo '<NGC_API_KEY>' | docker login nvcr.io -u '$oauthtoken' --password-stdin
```

代理与登录配置见 `docs/Env_Plan.md` 4.3。

### 3. 同步工作区

在 PC 的工作区根执行，将整个工作区同步到 NX（排除生成的构建产物）：

```bash
rsync -avz --delete -e ssh \
  --exclude 'build' --exclude 'install' --exclude 'log' --exclude 'models/*.engine' \
  ./ <user>@192.168.60.2:~/TurtleBot3_TX2NX_ws/
```

### 4. 编译与运行

在 NX 上构建镜像并运行：

```bash
cd ~/TurtleBot3_TX2NX_ws
docker build -t jazzy_minimal:arm64 -f docker/jazzy_minimal/Dockerfile .
docker run --network host -it --name jazzy_test jazzy_minimal:arm64
```

开发迭代时不必重建镜像，挂载源码在容器内编译，中间产物留在 NX 本地：

```bash
docker run --rm -it \
  -v ~/TurtleBot3_TX2NX_ws/src:/ros2_ws/src \
  -v ~/TurtleBot3_TX2NX_ws/build:/ros2_ws/build \
  -v ~/TurtleBot3_TX2NX_ws/install:/ros2_ws/install \
  jazzy_minimal:arm64 \
  bash -c 'colcon build'
```

## 文档

- `docs/Env_Plan.md`：环境配置、构建与实验过程。
- `AGENTS.md`：本仓库的协作与代码纪律约定。

## 后续

导航、感知、控制等模块在最小验证通过后以独立 ROS 2 包加入 `src/`，本文档随功能扩展补充对应的启动说明。

## 许可证

本项目采用 Apache License 2.0，全文见 `LICENSE`。
