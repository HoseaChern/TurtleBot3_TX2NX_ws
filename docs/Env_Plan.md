# Jetson TX2 NX 开发环境部署任务书

## 1. 系统约束与总体方案

### 1.1 环境现状

本任务涉及两台设备，版本信息如下。

PC（编码与训练机）：

| 项目         | 值                                                        |
| ------------ | --------------------------------------------------------- |
| 系统         | Ubuntu 24.04.5 LTS（noble），amd64                        |
| ROS 2        | Jazzy（`packages.ros.org/ros2/ubuntu`，noble）            |
| CUDA         | 12.9.2，驱动 615.71.09                                    |
| Python       | 3.12                                                      |
| 深度学习框架 | PyTorch（cu128），Ultralytics YOLO                        |
| 网络代理     | Clash Verge Rev，`127.0.0.1:7897`，系统代理模式，手动启动 |

NX（编译与部署机）：

| 项目     | 值                                          |
| -------- | ------------------------------------------- |
| 设备     | Jetson TX2 NX                               |
| JetPack  | 4.6.6                                       |
| L4T      | 32.7.6，Ubuntu 18.04，内核 4.9.337，aarch64 |
| GPU      | Pascal，计算能力 SM 6.2                     |
| CUDA     | 10.2                                        |
| TensorRT | 8.2                                         |
| cuDNN    | 8.3                                         |
| 内存     | 4GB                                         |
| 存储     | 16GB eMMC，外接 119.2GB NVMe（`nvme0n1`）   |
| 网络     | 千兆以太网（实测 1000 Mb/s）                |

### 1.2 版本锁定推导

NX 的 TensorRT 版本不可升级，结论由两条已核实的事实推出：

1. TensorRT 10.x 要求 GPU 计算能力不低于 SM 7.5。NX 的 Pascal 为 SM 6.2，低于门限，无法运行 TensorRT 10.x。
2. TensorRT 11.x 不支持 Jetson。NVIDIA 迁移文档明确要求 Jetson 停留在其 JetPack 版本对应的 TensorRT 10.x 之前版本。

因此 NX 上可用的 TensorRT 版本唯一确定为 8.2，CUDA 固定为 10.2。该约束在本任务周期内不随 TensorRT 上游迭代而改变。

推论：推理引擎选择不受"是否跨平台"驱动。ONNX Runtime 在 NX 上匹配 CUDA 10.2 的版本停留在 1.5 至 1.6（2020 年），其 TensorRT Execution Provider 亦要求 CUDA 11 以上，与平台不匹配。故 NX 侧推理统一走 TensorRT 8.2 原生接口。

模型资产不因引擎而锁定：训练侧导出的 ONNX 是开放格式，可迁移；锁定的仅是 NX 上的执行引擎，且由硬件决定。

### 1.3 分工模型

由 1.1 与 1.2 得出职责划分：

- PC：用 VSCode 编写 ROS 2 代码，训练 YOLO 模型并导出 ONNX。不参与 NX 的镜像构建。
- NX：原生构建 arm64 镜像，将 ONNX 转换为 TensorRT engine，以 C++ ROS 2 节点加载 engine 并推理。

选择"编码在 PC、编译在 NX"的依据：NX 的 CPU 不适合高频编译，而 PC 的 CPU 编译与调试快；镜像在 NX 原生构建，因 amd64 到 arm64 的交叉构建需 QEMU 模拟，其结果的可信度低于原生构建，且本任务的镜像体量小，原生构建无性能压力。

部署链路为：

```text
PC:  PyTorch / Ultralytics  --导出-->  model.onnx
NX:  model.onnx  --trtexec(TensorRT 8.2)-->  model.engine
NX:  ROS 2 C++ 节点  --加载-->  model.engine  --发布-->  检测结果话题
```

### 1.4 容器方案

采用单容器方案：一个基于 `ros:jazzy-ros-base` 的 arm64 镜像，通过 NVIDIA runtime 挂载宿主的 CUDA 10.2 与 TensorRT 8.2。

依据：

- CUDA 与 TensorRT 的 C++ 库只依赖 glibc 与 libstdc++。宿主的库由 glibc 2.27 编译，容器提供 glibc 2.39，新版本向前兼容，C++ 库可被容器内程序链接。
- TensorRT 与 PyTorch 的 Python binding 绑定 Python 3.6（cp36），容器内 Python 为 3.12，无法直接导入。因此推理层使用 C++，不使用 Python binding。

该方案的风险集中在工具链差异：容器内 gcc 13 链接宿主的 TensorRT 8.2 头文件与库。此风险由测试三（关键路径）验证。

### 1.5 工作流与数据传输

编码在 PC，编译在 NX。传输对象为源码与模型，二者均为小文件，用 `rsync` over SSH 同步，走第 4 章配置的点对点千兆网线。

```text
PC（编码 / 训练）                         NX（原生编译 / 推理）
  编辑 src/（VSCode + 本地 ROS 2 Jazzy）    docker build（arm64 原生）
  训练 YOLO，导出 model.onnx               容器内 colcon build
                                           trtexec 转换 engine，运行推理
        |           rsync over SSH                 ^
        +------ 直连千兆网线 192.168.60.0/24 ------+
```

不使用镜像仓库：镜像在 NX 原生构建，无需传输镜像。不使用 `nc`：`nc` 的适用场景是 GB 级大文件打满网口，而数据集留在 PC 训练、不进入 NX，NX 只需源码与模型这类小文件，`rsync` 的增量与校验特性更合适。

`rsync` 走 SSH 而非 daemon：同一条 SSH 通道既用于文件同步，也用于远程触发 NX 的编译命令，减少一处服务配置。

---

## 2. NX 环境配置

### 2.1 磁盘配置

#### 2.1.1 SSD 分区与格式化

先确认设备名称与磁盘内是否已有数据。`mklabel` 会清空整盘分区表，属不可逆操作，必须在确认无有用数据后执行。

```bash
lsblk
sudo parted /dev/nvme0n1 print
```

预期 `nvme0n1` 的 `MOUNTPOINT` 为空。若 `print` 显示已有分区或文件系统，先备份。

创建 GPT 分区表并分配单一分区：

```bash
sudo parted /dev/nvme0n1 mklabel gpt
sudo parted /dev/nvme0n1 mkpart primary ext4 0% 100%
sudo partprobe /dev/nvme0n1
sudo mkfs.ext4 /dev/nvme0n1p1
```

`partprobe` 用于在内核中重读分区表，避免 `mkfs` 找不到 `nvme0n1p1`。

#### 2.1.2 自动挂载

创建挂载点并获取分区 UUID：

```bash
sudo mkdir -p /mnt/nvme0n1p1
sudo blkid /dev/nvme0n1p1
```

将 UUID 写入 `/etc/fstab`：

```bash
sudo vim /etc/fstab
```

追加以下行，`<uuid>` 替换为 `blkid` 输出：

```text
UUID=<uuid> /mnt/nvme0n1p1 ext4 defaults 0 2
```

挂载并验证：

```bash
sudo mount -a
df -h /mnt/nvme0n1p1
```

#### 2.1.3 Swap 配置

4GB 内存在编译大型 ROS 2 包时可能不足。在 NVMe 上分配 16GB Swap 文件：

```bash
sudo fallocate -l 16G /mnt/nvme0n1p1/16GB.swap
sudo chmod 600 /mnt/nvme0n1p1/16GB.swap
sudo mkswap /mnt/nvme0n1p1/16GB.swap
sudo swapon /mnt/nvme0n1p1/16GB.swap
```

在 `/etc/fstab` 中追加：

```text
/mnt/nvme0n1p1/16GB.swap none swap sw 0 0
```

### 2.2 TensorRT

JetPack 4.6 的 TensorRT 不是必然随系统自带，取决于刷机或 SDK Manager 安装时的组件选择。先确认：

```bash
dpkg -l | grep -i tensorrt
ls /usr/lib/aarch64-linux-gnu/ | grep -i nvinfer
```

若两条均无输出，说明 TensorRT 未安装。安装 JetPack 4.6 对应的 TensorRT 8.2。安装需访问 NVIDIA 源，若 NX 未直连网络，先完成第 4 章的代理：

```bash
sudo apt-get update
sudo apt-get install -y nvidia-tensorrt
```

装完后确认 `libnvinfer.so.8` 等文件出现在 `/usr/lib/aarch64-linux-gnu/`。若 apt 源无 `nvidia-tensorrt`，用 SDK Manager 在已有系统上仅勾选 TensorRT 组件补装。

TensorRT 是宿主组件，由 NVIDIA runtime 挂载进容器，供 5.3 的推理使用。官方的 runtime 挂载清单不含 TensorRT，需手动把库加入 `/etc/nvidia-container-runtime/host-files-for-container.d/l4t.csv`：

```text
lib, /usr/lib/aarch64-linux-gnu/libnvinfer.so.8.2.1
lib, /usr/lib/aarch64-linux-gnu/libnvinfer_plugin.so.8.2.1
lib, /usr/lib/aarch64-linux-gnu/libnvonnxparser.so.8.2.1
lib, /usr/lib/aarch64-linux-gnu/libnvparsers.so.8.2.1
lib, /usr/lib/aarch64-linux-gnu/libEGL.so.1
lib, /usr/lib/aarch64-linux-gnu/libGLdispatch.so.0
```

清单条目格式为 `类型, 路径`（逗号分隔）。后两行是 `libnvinfer` 的依赖：它在 Jetson 上链接了图形栈（NvMedia、DLA、EGL），其中 NvMedia 与 DLA 由官方清单的 `tegra/` 条目提供，GLVND 的 `libEGL` 需补充。添加后重启 `dockerd` 使其重读清单。

### 2.3 Docker 与 NVIDIA Runtime

#### 2.3.1 探测现状

JetPack 4.6 通过 SDK Manager 安装时通常已包含 NVIDIA Container Runtime（beta 形态）。先探测，再决定后续动作：

```bash
dpkg -l | grep -iE 'nvidia-docker|nvidia-container|libnvidia-container'
docker info 2>/dev/null | grep -i runtime
```

- 若输出含 `nvidia-docker2`、`nvidia-container-runtime`、`libnvidia-container`，说明 runtime 已存在，跳过 2.3.2，直接进入 2.3.3。
- 若 runtime 缺失，执行 2.3.2。

不使用 `nvidia-ctk` 命令。该命令来自 NVIDIA Container Toolkit 1.7.0 以上，JetPack 4.6 源中不保证存在，改为直接维护 `daemon.json`。

#### 2.3.2 安装 Docker 与 Runtime

安装 Docker：

```bash
sudo apt-get update
sudo apt-get install -y docker.io
sudo systemctl enable --now docker
```

Ubuntu 18.04 源的 `docker.io` 为 19.03 及以上版本，满足本任务要求。

若 2.3.1 显示 runtime 缺失，安装：

```bash
sudo apt-get install -y nvidia-docker2
```

安装后再次用 2.3.1 的探测命令确认。若源中无 `nvidia-docker2`，参考 NVIDIA 的 "NVIDIA Container Runtime on Jetson" 流程，通过 SDK Manager 补装该组件。

#### 2.3.3 配置运行时

配置默认运行时，避免每次运行容器手动指定 `--runtime nvidia`。写入 `/etc/docker/daemon.json`（若文件已存在，在原有键基础上追加，不要覆盖其它键）：

```json
{
  "runtimes": {
    "nvidia": {
      "path": "nvidia-container-runtime",
      "runtimeArgs": []
    }
  },
  "default-runtime": "nvidia"
}
```

重启并确认：

```bash
sudo systemctl restart docker
sudo docker info | grep -iE 'Default Runtime|Runtimes'
```

预期输出含 `Default Runtime: nvidia`。

将当前用户加入 `docker` 组，之后重新登录使组生效：

```bash
sudo usermod -aG docker $USER
```

除上述 daemon 配置外，容器镜像还需声明 GPU 可见性，否则 NVIDIA runtime 不会向容器挂载设备与库。NGC 的 l4t 系列镜像内置了这些变量，普通镜像（如 `ros:jazzy-ros-base`）没有：

```dockerfile
ENV NVIDIA_VISIBLE_DEVICES=all
ENV NVIDIA_DRIVER_CAPABILITIES=all
```

本工作区的镜像统一在 Dockerfile 中设置。

### 2.4 Docker 数据目录迁移至 NVMe

在 Docker 安装完成、且尚未拉取大量镜像前迁移，可减少拷贝量。

停止 Docker 服务：

```bash
sudo systemctl stop docker docker.socket containerd
```

用 `rsync` 复制，保留属主、权限与扩展属性。`cp -r` 不保留属性，会导致 `overlay2` 目录属主错乱而无法启动：

```bash
sudo mkdir -p /mnt/nvme0n1p1/docker
sudo rsync -aHAX /var/lib/docker/ /mnt/nvme0n1p1/docker/
```

在 `/etc/docker/daemon.json` 中追加 `data-root`。完整内容如下：

```json
{
  "runtimes": {
    "nvidia": {
      "path": "nvidia-container-runtime",
      "runtimeArgs": []
    }
  },
  "default-runtime": "nvidia",
  "data-root": "/mnt/nvme0n1p1/docker"
}
```

重启并确认：

```bash
sudo systemctl start docker
sudo docker info | grep 'Docker Root Dir'
```

预期输出 `Docker Root Dir: /mnt/nvme0n1p1/docker`。确认新目录可用后，删除旧目录以释放 eMMC 空间：

```bash
sudo rm -rf /var/lib/docker
```

---

## 3. PC 环境配置

### 3.1 开发与训练环境

PC 承担编码与训练，不安装 Docker。三项工具均已就绪：

- 编码：VSCode 加本地 ROS 2 Jazzy（`/opt/ros/jazzy`）。在 VSCode 的 C/C++ 配置中把 `/opt/ros/jazzy/include/**` 加入 `includePath`，即可获得头文件补全与跳转。
- 训练：`uv` 管理的 Python 3.12 虚拟环境（PyTorch cu128、Ultralytics）。
- 同步：`rsync`，见第 4 章。

不使用 VSCode Remote-SSH 连接 NX，理由：Remote-SSH 会在 NX 上安装并运行 VSCode Server，占用 NX 的内存与 CPU，且会把代码真源迁到 NX；而 NX 需保持最简。代码真源留在 PC。

### 3.2 工作区结构

工作区根为当前 git 仓库 `~/Documents/TurtleBot3_TX2NX_ws/`。

```text
TurtleBot3_TX2NX_ws/
├── docs/
│   └── Env_Plan.md
├── docker/
│   ├── jazzy_minimal/
│   │   ├── Dockerfile
│   │   └── entrypoint.sh
│   └── cuda_test/
│       ├── Dockerfile
│       └── deviceQuery.cu
├── src/
│   └── minimal_test/
│       ├── package.xml
│       ├── CMakeLists.txt
│       ├── src/
│       └── launch/
├── models/
│   └── model.onnx
└── scripts/
    ├── export_onnx.py
    ├── trt_link_check.cpp
    ├── setup_nvme.sh
    ├── setup_docker_nx.sh
    └── sync_to_nx.sh
```

`.gitignore` 内容：

```text
build/
install/
log/
*.swap
*.tar.gz
```

`src/` 当前仅包含最小验证所需的节点实现，后续导航、感知、控制模块在最小验证通过后以独立包加入。

---

## 4. 网络与数据同步

网络连接与数据同步由 PC 与 NX 两端协同完成，故独立成章，不属于任一单机配置。

### 4.1 直连链路配置

PC 与 NX 用点对点千兆网线直连，配置静态 IP，旁路路由器与 Wi-Fi。链路两端均为千兆，有效带宽上限 118 MB/s。

PC 端使用板载千兆网卡 `enp8s0`，Wi-Fi（`wlp14s0`）保持用于上网与代理，两条链路互不干扰：

```bash
sudo ip addr add 192.168.60.1/24 dev enp8s0
sudo ip link set enp8s0 up
```

NX 端（网口名以 `ip -br link` 为准，TX2 NX 上为 `eth0`）：

```bash
sudo ip addr add 192.168.60.2/24 dev eth0
sudo ip link set eth0 up
```

验证（在 PC 上执行）：

```bash
ping -c 3 192.168.60.2
```

### 4.2 带宽验证

在 NX 上启动服务端：

```bash
sudo apt-get install -y iperf3
iperf3 -s
```

在 PC 上：

```bash
sudo apt-get install -y iperf3
iperf3 -c 192.168.60.2
```

预期约 900 Mbps 以上。

### 4.3 网络代理

NX 不安装 Clash Verge Rev。arm64 构建已被上游移除，且 Clash Verge 要求 Ubuntu 20.04 以上，与 NX 的 18.04 不符。

NX 复用 PC 的 Clash 代理。代理地址取 PC 直连网卡的 IP `192.168.60.1`：NX 与 PC 经直连网线互通，代理流量走该链路到达 PC 的 Clash，再由 PC 的 Wi-Fi 出口访问外网。不要填 PC 的 Wi-Fi IP，NX 经直连链路访问不到该网段。本节依赖 4.1 的直连链路已配置，故顺序上 4.1 先于本节。

代理分两类，读取位置不同：

| 操作                                               | 读取代理的位置                                              |
| -------------------------------------------------- | ----------------------------------------------------------- |
| `docker login`、`docker manifest`、`docker buildx` | CLI 进程的环境变量，或 `~/.docker/config.json` 的 `proxies` |
| `docker pull`、`docker run` 拉取镜像               | `dockerd` 的 systemd drop-in                                |

Go 解析代理时大写优先，若只设小写 `http_proxy` 而大写 `HTTP_PROXY` 仍为旧值，CLI 会用旧值。故四个变量都设为直连代理。

在 PC 的 Clash Verge 中开启 Allow LAN，使代理监听 `0.0.0.0:7897`。在 NX 上临时验证（CLI 侧）：

```bash
export HTTP_PROXY=http://192.168.60.1:7897
export HTTPS_PROXY=http://192.168.60.1:7897
export http_proxy=http://192.168.60.1:7897
export https_proxy=http://192.168.60.1:7897
export NO_PROXY=localhost,127.0.0.1,::1
curl -sI https://registry-1.docker.io/v2/ | head -1
```

预期返回 `HTTP/... 401` 或 `200`。

让 `dockerd` 长期走代理，创建 systemd drop-in：

```bash
sudo mkdir -p /etc/systemd/system/docker.service.d
sudo tee /etc/systemd/system/docker.service.d/proxy.conf > /dev/null <<'EOF'
[Service]
Environment="HTTP_PROXY=http://192.168.60.1:7897"
Environment="HTTPS_PROXY=http://192.168.60.1:7897"
Environment="NO_PROXY=localhost,127.0.0.1,::1"
EOF
sudo systemctl daemon-reload
sudo systemctl restart docker
sudo docker info | grep -i proxy
```

`daemon-reload` 与 `restart` 必须执行，否则改动不生效。

若 NX 可直连 `nvcr.io` 与 Docker Hub，可跳过本节。代理为手动启动，构建时须确认 PC 上 Clash 已开启。

### 4.4 SSH 与 rsync 同步

NX 端启用 SSH 服务：

```bash
sudo apt-get install -y openssh-server
sudo systemctl enable --now ssh
```

先在 NX 上创建与 PC 同名的工作区目录，再从 PC 推送。`rsync` 只能在目标父目录已存在时创建其下的子目录，故这一步不可省略：

```bash
# NX 上
mkdir -p ~/TurtleBot3_TX2NX_ws
```

同步整个工作区，作为 NX 侧的镜像副本与备份。命令须在 PC 的工作区根 `~/Documents/TurtleBot3_TX2NX_ws/` 下执行，`<user>` 替换为 NX 的登录用户名（本机两台设备同名，均为 `changli`），目标地址为直连 IP `192.168.60.2`：

```bash
rsync -avz --delete -e ssh \
  --exclude 'build' --exclude 'install' --exclude 'log' --exclude 'models/*.engine' \
  ./ <user>@192.168.60.2:~/TurtleBot3_TX2NX_ws/
```

`--delete` 使 NX 副本与 PC 源一一对应。排除 `build`、`install`、`log` 三类生成物：它们在 NX 本地由 colcon 生成，不同步。另排除 `models/*.engine`：engine 由 NX 上的 `trtexec` 生成，PC 侧没有，若参与 `--delete` 会被删除，排除后可保留。`.git` 一并同步，使 NX 副本可作为完整备份恢复。工作区为纯文本，同步量与耗时均可忽略。

### 4.5 注意

- 直连网段不走代理。Clash 为系统代理模式，`rsync` 与 `ssh` 不读取代理设置，不受影响。若切换到 TUN 模式，需把 `192.168.60.0/24` 加入 Clash 直连规则。
- 直连静态 IP 在重启后不保留。若要持久化，把地址配置写入 NX 的 `/etc/network/interfaces` 或 PC 的 NetworkManager 连接配置。

---

## 5. 最小实验计划

### 5.1 测试一：Jazzy 节点通信

#### 5.1.1 目标

验证 arm64 Jazzy 环境在 NX 内核 4.9.337 上的运行能力，以及 ROS 2 发布-订阅通信功能。该测试验证 Ubuntu 24.04 用户态与宿主的兼容性，不涉及 GPU。

#### 5.1.2 第一层：内核兼容性冒烟

先在 NX 上直接拉取官方 arm64 镜像，确认 Ubuntu 24.04 用户态可运行：

```bash
docker run --rm ros:jazzy-ros-base uname -r
docker run --rm ros:jazzy-ros-base cat /etc/os-release
```

预期输出内核 `4.9.337` 与 Ubuntu 24.04。若该步骤失败，1.4 的单容器方案不成立，需转入备选方案评估。

#### 5.1.3 第二层：自定义包通信

节点实现：`src/minimal_test/` 下创建 ROS 2 包。`publisher_node.cpp` 使用 `rclcpp` 与 `std_msgs::msg::String` 以固定频率发布递增计数，`subscriber_node.cpp` 订阅同一话题并输出接收内容。

`package.xml` 声明依赖：

```xml
<depend>rclcpp</depend>
<depend>std_msgs</depend>
```

`CMakeLists.txt` 注册两个可执行文件，`install` 安装 `launch/` 目录。

镜像定义 `docker/jazzy_minimal/Dockerfile`：

```dockerfile
FROM ros:jazzy-ros-base

RUN apt-get update && apt-get install -y --no-install-recommends \
    ros-jazzy-rmw-cyclonedds-cpp \
    && rm -rf /var/lib/apt/lists/*

ENV RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
ENV NVIDIA_VISIBLE_DEVICES=all
ENV NVIDIA_DRIVER_CAPABILITIES=all

WORKDIR /ros2_ws
COPY src/ /ros2_ws/src/
RUN . /opt/ros/jazzy/setup.sh && colcon build

COPY docker/jazzy_minimal/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]
CMD ["bash"]
```

`COPY` 路径相对于构建上下文（仓库根）。

`docker/jazzy_minimal/entrypoint.sh`：

```bash
#!/bin/bash
source /opt/ros/jazzy/setup.bash
source /ros2_ws/install/setup.bash
exec "$@"
```

`CMD ["bash"]` 保证不带命令直接运行容器时进入交互 shell，避免 `exec "$@"` 因参数为空而退出。

执行步骤：

1. 在 PC 上先本地编译，验证代码正确（PC 已装 ROS 2 Jazzy，无需容器）：

```bash
cd ~/Documents/TurtleBot3_TX2NX_ws
source /opt/ros/jazzy/setup.bash
colcon build --packages-select minimal_test
source install/setup.bash
```

2. 在 PC 工作区根同步整个工作区到 NX（见 4.4）：

```bash
rsync -avz --delete -e ssh \
  --exclude 'build' --exclude 'install' --exclude 'log' --exclude 'models/*.engine' \
  ./ changli@192.168.60.2:~/TurtleBot3_TX2NX_ws/
```

3. 在 NX 上原生构建镜像（不带 `--platform`，结果即 NX 的 arm64）：

```bash
cd ~/TurtleBot3_TX2NX_ws
docker build -t jazzy_minimal:arm64 -f docker/jazzy_minimal/Dockerfile .
```

4. 运行容器：

```bash
docker run --network host -it --name jazzy_test jazzy_minimal:arm64
```

5. 容器内启动 launch 文件：

```bash
ros2 launch minimal_test minimal_test.launch.py
```

6. 另开终端进入同一容器，验证话题：

```bash
docker exec -it jazzy_test bash
source /opt/ros/jazzy/setup.bash
ros2 topic list
ros2 topic echo /topic
```

#### 5.1.4 开发迭代

代码改动后不重建镜像，用挂载源码在容器内编译，`build` 等中间产物留在 NX 本地磁盘：

```bash
# PC 修改代码后同步工作区
rsync -avz --delete -e ssh \
  --exclude 'build' --exclude 'install' --exclude 'log' --exclude 'models/*.engine' \
  ./ changli@192.168.60.2:~/TurtleBot3_TX2NX_ws/

# NX 上挂载源码与构建目录编译
docker run --rm -it \
  -v ~/TurtleBot3_TX2NX_ws/src:/ros2_ws/src \
  -v ~/TurtleBot3_TX2NX_ws/build:/ros2_ws/build \
  -v ~/TurtleBot3_TX2NX_ws/install:/ros2_ws/install \
  jazzy_minimal:arm64 \
  bash -c 'colcon build'
```

#### 5.1.5 预期结果

`ros2 topic list` 显示 `/topic`，`ros2 topic echo /topic` 持续输出递增计数。

### 5.2 测试二：CUDA 直通冒烟

#### 5.2.1 目标

验证 NVIDIA runtime 可将 Pascal GPU 直通给容器。该测试为测试三的前置条件，本身不涉及 TensorRT。

#### 5.2.2 容器镜像

`docker/cuda_test/Dockerfile`：

```dockerfile
FROM nvcr.io/nvidia/l4t-base:r32.7.1

RUN apt-get update && apt-get install -y --no-install-recommends \
    make g++ \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
COPY docker/cuda_test/deviceQuery.cu .
```

镜像标签为 `r32.7.1`。NGC 上 `l4t-base` 的容器版本与宿主 L4T 版本非一一对应，宿主 L4T 32.7.6 对应的容器标签停留在 `r32.7.1`。`nvcr.io` 的镜像需要 NGC 账号认证，拉取前须先 `docker login nvcr.io`（见 5.2.3）。

`deviceQuery.cu` 使用 CUDA Runtime API 查询设备属性：

```cpp
#include <cstdio>
#include <cuda_runtime.h>

int main() {
    int deviceCount = 0;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    if (err != cudaSuccess) {
        printf("cudaGetDeviceCount failed: %s\n", cudaGetErrorString(err));
        return 1;
    }
    printf("Detected %d CUDA Capable device(s)\n", deviceCount);
    for (int i = 0; i < deviceCount; i++) {
        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, i);
        printf("Device %d: %s\n", i, prop.name);
        printf("  CUDA Capability: %d.%d\n", prop.major, prop.minor);
    }
    return 0;
}
```

#### 5.2.3 执行步骤

CUDA 测试在 NX 上原生构建，不经过交叉编译。构建上下文为仓库根，`Dockerfile` 中的 `COPY docker/cuda_test/deviceQuery.cu .` 相对该上下文。

1. 在 NX 上登录 NGC <https://org.ngc.nvidia.com> 。`nvcr.io` 的镜像需认证，用户名固定为 `$oauthtoken`，密码为 NGC API Key（登录 NGC 后，在账号设置中生成）。CLI 侧走 4.3 的代理变量。

```bash
echo '<NGC_API_KEY>' | docker login nvcr.io -u '$oauthtoken' --password-stdin
# --password-stdin 确保密钥不会记录到命令行历史
```

2. 拉取基础镜像，同时验证标签存在与认证有效：

```bash
docker pull nvcr.io/nvidia/l4t-base:r32.7.1
```

3. 在 NX 上原生构建：

```bash
cd ~/TurtleBot3_TX2NX_ws
docker build -f docker/cuda_test/Dockerfile -t cuda_test .
```

4. 运行并编译执行：

```bash
docker run --rm cuda_test sh -c 'nvcc -o deviceQuery deviceQuery.cu && ./deviceQuery'
```

#### 5.2.4 预期结果

输出 `Detected 1 CUDA Capable device(s)`，设备名称为 `NVIDIA Tegra X2` 或类似标识，`CUDA Capability` 为 `6.2`。

### 5.3 测试三：TensorRT 8.2 与 YOLO 推理（关键路径）

#### 5.3.1 目标

验证单容器方案的核心假设：Jazzy 容器内可链接宿主挂载的 TensorRT 8.2，完成 ONNX 到 engine 的转换并执行推理。该测试同时覆盖四个风险点：CUDA 直通、TensorRT 可用、ONNX opset 兼容、Jazzy 与老工具链共存。

#### 5.3.2 前置：PC 导出 ONNX

在 PC 的 Python 环境（需装 PyTorch 与 Ultralytics）中，运行本工作区的导出脚本。TensorRT 8.2 的 ONNX 解析器支持的 opset 较老，脚本显式指定 `opset=12`：

```bash
python scripts/export_onnx.py
```

脚本将 `yolov8n.onnx` 输出到本工作区的 `models/`，按 4.4 同步到 NX。

#### 5.3.3 执行步骤

1. 在 NX 上把模型、自检程序与头文件挂载进 Jazzy 容器。runtime 默认只挂库与设备，不挂头文件，故需额外挂载宿主 include 目录：

```bash
cd ~/TurtleBot3_TX2NX_ws
docker run --network host -it \
  -v ~/TurtleBot3_TX2NX_ws/models:/work \
  -v ~/TurtleBot3_TX2NX_ws/scripts:/src \
  -v /usr/include/aarch64-linux-gnu:/opt/host-include:ro \
  -v /usr/local/cuda-10.2/include:/opt/cuda-include:ro \
  jazzy_minimal:arm64
```

2. 在容器内确认 TensorRT 库可见：

```bash
ldconfig -p | grep -i nvinfer
```

3. 编译链接 TensorRT 的最小程序，验证工具链兼容（核心风险）。用 `-idirafter` 把宿主头文件排在容器系统头之后，避免 18.04 与 24.04 头文件错配；用 `-L` 指向 `tegra/`（`libnvinfer` 依赖 Jetson 图形栈 NvMedia、DLA、EGL）；链接名用完整 soname：

```bash
g++ -o /tmp/trt_link_check /src/trt_link_check.cpp \
  -idirafter /opt/host-include -idirafter /opt/cuda-include \
  -L/usr/lib/aarch64-linux-gnu/tegra \
  -Wl,-rpath-link,/usr/lib/aarch64-linux-gnu/tegra \
  -Wl,--allow-shlib-undefined \
  -l:libnvinfer.so.8
```

运行：

```bash
LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu/tegra /tmp/trt_link_check
```

预期输出 `TensorRT version: 8201`。

4. 将 ONNX 转为 engine。jazzy 镜像不含 `trtexec`，在宿主（容器外）运行 TensorRT 自带的 `trtexec`：

```bash
cd ~/TurtleBot3_TX2NX_ws
/usr/src/tensorrt/bin/trtexec --onnx=models/yolov8n.onnx --saveEngine=models/yolov8n.engine --fp16
```

TX2 NX 上该转换耗时约 10 分钟，属正常，需耐心等待。产物 `models/yolov8n.engine` 位于工作区，容器内通过 `/work` 可见。

5. 用 engine 做推理（随机输入，验证 engine 可运行），同样在宿主运行：

```bash
cd ~/TurtleBot3_TX2NX_ws
/usr/src/tensorrt/bin/trtexec --loadEngine=models/yolov8n.engine --iterations=10 --avgRuns=10
```

#### 5.3.4 风险与处置

- 头文件：runtime 不挂头文件，已通过运行时挂载宿主 include 目录解决（见 5.3.3 第 1 步）。
- 工具链差异：容器内 gcc 13 编译链接 TensorRT 8.2 头文件与库，由 5.3.3 第 3 步验证，已通过。
- opset 兼容：若 `trtexec` 报算子不支持，降低导出 opset。

#### 5.3.5 预期结果

第 2 步列出 `libnvinfer` 相关库；第 3 步输出 TensorRT 版本号；第 4 步生成非空 engine 且构建日志无 unsupported operator；第 5 步退出码为 0。

### 5.4 执行顺序

依赖关系决定执行顺序：

```text
PC 与 NX 直连链路配置
  -> NX 磁盘配置
  -> NX Docker 与 runtime
  -> NX 数据目录迁移
  -> NX 网络代理（指向 PC 直连 IP 192.168.60.1）
  -> PC rsync 同步工作区
  -> 测试一（Jazzy 通信，NX 原生构建）
  -> 测试二（CUDA 直通）
  -> 测试三（TensorRT 与 YOLO）
```

测试一验证用户态与内核兼容，测试二验证 GPU 直通，测试三在两者之上验证 TensorRT 与老工具链共存。任一前置失败则后续测试无意义。
