# Jetson TX2 NX 开发环境部署任务书

## 1. 系统约束与总体方案

### 1.1 环境现状

本任务涉及两台设备，版本信息如下。

PC（开发与训练机）：

| 项目         | 值                                                    |
| ------------ | ----------------------------------------------------- |
| 系统         | Ubuntu 24.04.5 LTS（noble），amd64                    |
| ROS 2        | Jazzy（`packages.ros.org/ros2/ubuntu`，noble）        |
| CUDA         | 12.9.2，驱动 615.71.09                                |
| Python       | 3.12                                                  |
| 深度学习框架 | PyTorch（cu128），Ultralytics YOLO                    |
| 网络代理     | Clash Verge，`127.0.0.1:7897`，系统代理模式，手动启动 |

NX（部署机）：

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

- PC：训练 YOLO 模型，导出 ONNX，构建 arm64 ROS 2 镜像。
- NX：将 ONNX 转换为 TensorRT engine，以 C++ ROS 2 节点加载 engine 并推理。

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

### 1.5 镜像传输

PC 与 NX 架构不同，镜像不通过 registry 中转，改用点对点千兆网线直连传输：

- PC 用 `docker buildx build --platform linux/arm64 --load` 构建并落到本地镜像列表。
- PC 与 NX 以网线直连、配置静态 IP，用 `nc` 明文传输，避免 SSH 加密使 NX 的 CPU 成为瓶颈。
- NX 用 `docker load` 导入。

不需要 Docker Hub 或私有仓库账号。待镜像迭代频繁后再评估引入 registry。

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
UUID=<uuid> /mnt/nvme ext4 defaults 0 2
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

### 2.2 Docker 与 NVIDIA Runtime

#### 2.2.1 探测现状

JetPack 4.6 通过 SDK Manager 安装时通常已包含 NVIDIA Container Runtime（beta 形态）。先探测，再决定后续动作：

```bash
dpkg -l | grep -iE 'nvidia-docker|nvidia-container|libnvidia-container'
docker info 2>/dev/null | grep -i runtime
```

- 若输出含 `nvidia-docker2`、`nvidia-container-runtime`、`libnvidia-container`，说明 runtime 已存在，跳过 2.2.2，直接进入 2.2.3。
- 若 runtime 缺失，执行 2.2.2。

不使用 `nvidia-ctk` 命令。该命令来自 NVIDIA Container Toolkit 1.7.0 以上，JetPack 4.6 源中不保证存在，改为直接维护 `daemon.json`。

#### 2.2.2 安装 Docker 与 Runtime

安装 Docker：

```bash
sudo apt-get update
sudo apt-get install -y docker.io
sudo systemctl enable --now docker
```

Ubuntu 18.04 源的 `docker.io` 为 19.03 及以上版本，满足本任务要求。

若 2.2.1 显示 runtime 缺失，安装：

```bash
sudo apt-get install -y nvidia-docker2
```

安装后再次用 2.2.1 的探测命令确认。若源中无 `nvidia-docker2`，参考 NVIDIA 的 "NVIDIA Container Runtime on Jetson" 流程，通过 SDK Manager 补装该组件。

#### 2.2.3 配置运行时

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
docker info | grep -iE 'Default Runtime|Runtimes'
```

预期输出含 `Default Runtime: nvidia`。

将当前用户加入 `docker` 组，之后重新登录使组生效：

```bash
sudo usermod -aG docker $USER
```

### 2.3 Docker 数据目录迁移至 NVMe

在 Docker 安装完成、且尚未拉取大量镜像前迁移，可减少拷贝量。

停止 Docker 服务：

```bash
sudo systemctl stop docker docker.socket containerd
```

用 `rsync` 复制，保留属主、权限与扩展属性。`cp -r` 不保留属性，会导致 `overlay2` 目录属主错乱而无法启动：

```bash
sudo mkdir -p /mnt/nvme/docker
sudo rsync -aHAX /var/lib/docker/ /mnt/nvme/docker/
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
  "data-root": "/mnt/nvme/docker"
}
```

重启并确认：

```bash
sudo systemctl start docker
docker info | grep 'Docker Root Dir'
```

预期输出 `Docker Root Dir: /mnt/nvme/docker`。确认新目录可用后，删除旧目录以释放 eMMC 空间：

```bash
sudo rm -rf /var/lib/docker
```

### 2.4 网络代理

NX 不安装 Clash Verge。arm64 构建已被上游移除，且 Clash Verge 要求 Ubuntu 20.04 以上，与 NX 的 18.04 不符。

在 PC 的 Clash Verge 中开启 Allow LAN，使代理监听局域网地址。在 NX 上获取 PC 的局域网 IP，然后临时验证：

```bash
export http_proxy=http://<PC_IP>:7897
export https_proxy=http://<PC_IP>:7897
curl -sI https://nvcr.io/v2/ | head -1
```

若要让 `dockerd` 长期走代理，创建 systemd drop-in：

```bash
sudo mkdir -p /etc/systemd/system/docker.service.d
sudo tee /etc/systemd/system/docker.service.d/proxy.conf > /dev/null <<'EOF'
[Service]
Environment="HTTP_PROXY=http://<PC_IP>:7897"
Environment="HTTPS_PROXY=http://<PC_IP>:7897"
Environment="NO_PROXY=localhost,127.0.0.1,::1"
EOF
sudo systemctl daemon-reload
sudo systemctl restart docker
```

若 NX 可直连 `nvcr.io` 与 Docker Hub，可跳过本节。代理为手动启动，构建时须确认 PC 上 Clash 已开启。

---

## 3. PC 环境配置

### 3.1 Docker 安装

PC 运行 Ubuntu 24.04，属于 Docker 官方支持的 LTS 版本。使用 Docker 官方 APT 仓库安装，版本可控，不使用 `apt install docker.io` 或 `get.docker.com` 脚本。apt 已配置自动走代理，安装过程无需额外设置。

卸载可能存在的旧版本：

```bash
sudo apt remove $(dpkg --get-selections docker.io docker-compose docker-compose-v2 docker-doc docker-buildx podman-docker containerd runc | cut -f1)
```

添加官方 GPG 密钥与仓库：

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc

echo \
  "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu \
  $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | \
  sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
```

安装 Docker Engine：

```bash
sudo apt-get update
sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
```

验证：

```bash
sudo docker run hello-world
```

将当前用户加入 `docker` 组，之后重新登录：

```bash
sudo usermod -aG docker $USER
```

### 3.2 Docker daemon 代理

apt 走代理与 `dockerd` 走代理相互独立。系统代理模式下 `dockerd` 不读取系统代理设置，需显式配置。创建 systemd drop-in：

```bash
sudo mkdir -p /etc/systemd/system/docker.service.d
sudo tee /etc/systemd/system/docker.service.d/proxy.conf > /dev/null <<'EOF'
[Service]
Environment="HTTP_PROXY=http://127.0.0.1:7897"
Environment="HTTPS_PROXY=http://127.0.0.1:7897"
Environment="NO_PROXY=localhost,127.0.0.1,::1"
EOF
sudo systemctl daemon-reload
sudo systemctl restart docker
docker info | grep -i proxy
```

若 Clash 仅运行于系统代理模式，此配置必要；若启用 TUN 模式，则流量已被接管，此配置无害但非必需。

### 3.3 跨架构构建

PC 为 amd64，NX 为 arm64。在 PC 上为 NX 构建镜像须使用 Buildx 配合 QEMU 用户态模拟。QEMU 无法验证 CUDA 功能，涉及 CUDA 的镜像在 NX 上原生构建。

安装 QEMU 用户态仿真：

```bash
sudo apt-get install -y qemu-user-static binfmt-support
docker run --privileged --rm tonistiigi/binfmt --install all
ls /proc/sys/fs/binfmt_misc/qemu-*
```

创建 Buildx Builder：

```bash
docker buildx create --name cross --use
docker buildx inspect --bootstrap
docker buildx ls
```

预期 `Platforms` 包含 `linux/arm64`。

### 3.4 工作区结构

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
├── scripts/
│   ├── setup_nvme.sh
│   ├── setup_docker_nx.sh
│   ├── setup_docker_pc.sh
│   └── build_and_transfer.sh
└── tests/
    ├── test1_jazzy_comm.md
    ├── test2_cuda.md
    └── test3_tensorrt_yolo.md
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

### 3.5 镜像与数据传输

PC 与 NX 均为千兆网口，用点对点网线直连，配置静态 IP，用 `nc` 明文传输。选择 `nc` 而非 `scp` 的依据：传输时间 $t = S / B$，$B = \min(B_{\text{link}}, B_{\text{disk}}, B_{\text{cpu}})$。链路与 NVMe 存储均非瓶颈，而 NX 的 CPU 在 SSH 加解密上是瓶颈；`nc` 不做加密，可达链路上限 $118$ MB/s。

#### 3.5.1 直连链路配置

PC 端使用 USB 3.0 千兆网卡 `enx00e04c680c2f`，Wi-Fi 保持用于上网与代理，两条链路互不干扰：

```bash
sudo ip addr add 192.168.60.1/24 dev enx00e04c680c2f
sudo ip link set enx00e04c680c2f up
```

NX 端：

```bash
sudo ip addr add 192.168.60.2/24 dev eth0
sudo ip link set eth0 up
```

验证：

```bash
ping -c 3 192.168.60.2
```

#### 3.5.2 带宽验证

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

预期约 $900$ Mbps 以上。

#### 3.5.3 构建与传输

构建 arm64 镜像并落到本地镜像列表：

```bash
docker buildx build --platform linux/arm64 \
  -t jazzy_minimal:arm64 --load \
  -f docker/jazzy_minimal/Dockerfile .
```

NX 端接收：

```bash
nc -l -p 9000 | docker load
```

PC 端发送：

```bash
docker save jazzy_minimal:arm64 | nc 192.168.60.2 9000
```

`nc` 语法因实现而异：Ubuntu 18.04 的 netcat-openbsd 用 `nc -l 9000`。两端对齐即可。

传目录同理：

```bash
# PC
tar cf - <dir> | nc 192.168.60.2 9000
# NX
nc -l -p 9000 | tar xf - -C /mnt/nvme/<dest>
```

#### 3.5.4 压缩策略

不对 `docker save` 的输出再 `gzip`。镜像层本身已压缩，二次压缩收益低，且 NX 的 CPU 解压会成为新瓶颈。未压缩的数据集若需压缩，用 `zstd` 取代 `gzip`，压缩率相近而解压快数倍。

#### 3.5.5 注意

- 直连网段不走代理。Clash 为系统代理模式，`nc` 不读取代理设置，不受影响。若切换到 TUN 模式，需把 `192.168.60.0/24` 加入 Clash 直连规则。
- 接收目标写 `/mnt/nvme/`，不占 eMMC。Docker 数据目录已在 2.3 节迁移至 NVMe。

---

## 4. 最小实验计划

### 4.1 测试一：Jazzy 节点通信

#### 4.1.1 目标

验证 arm64 Jazzy 环境在 NX 内核 4.9.337 上的运行能力，以及 ROS 2 发布-订阅通信功能。该测试验证 Ubuntu 24.04 用户态与宿主的兼容性，不涉及 GPU。

#### 4.1.2 第一层：内核兼容性冒烟

先在 NX 上直接拉取官方 arm64 镜像，确认 Ubuntu 24.04 用户态可运行：

```bash
docker run --rm ros:jazzy-ros-base uname -r
docker run --rm ros:jazzy-ros-base cat /etc/os-release
```

预期输出内核 `4.9.337` 与 Ubuntu 24.04。若该步骤失败，1.4 的单容器方案不成立，需转入备选方案评估。

#### 4.1.3 第二层：自定义包通信

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

按 3.5 构建、传输、导入后，在 NX 上运行：

```bash
docker run --network host -it --name jazzy_test jazzy_minimal:arm64
```

容器内启动 launch 文件：

```bash
ros2 launch minimal_test minimal_test.launch.py
```

另开终端进入同一容器，验证话题：

```bash
docker exec -it jazzy_test bash
ros2 topic list
ros2 topic echo /topic
```

#### 4.1.4 预期结果

`ros2 topic list` 显示 `/topic`，`ros2 topic echo /topic` 持续输出递增计数。

### 4.2 测试二：CUDA 直通冒烟

#### 4.2.1 目标

验证 NVIDIA runtime 可将 Pascal GPU 直通给容器。该测试为测试三的前置条件，本身不涉及 TensorRT。

#### 4.2.2 容器镜像

`docker/cuda_test/Dockerfile`：

```dockerfile
FROM nvcr.io/nvidia/l4t-base:r32.7.1

RUN apt-get update && apt-get install -y --no-install-recommends \
    make g++ \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
COPY docker/cuda_test/deviceQuery.cu .
```

镜像标签为 `r32.7.1`。NGC 上 `l4t-base` 的容器版本与宿主 L4T 版本非一一对应，宿主 L4T 32.7.6 对应的容器标签停留在 `r32.7.1`。执行前用 `docker manifest inspect nvcr.io/nvidia/l4t-base:r32.7.1` 核实标签存在。

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

#### 4.2.3 构建与运行

CUDA 测试在 NX 上原生构建，不经过 PC 的 QEMU。

```bash
cd ~/Documents/TurtleBot3_TX2NX_ws
docker build -t cuda_test docker/cuda_test/
docker run --rm cuda_test sh -c 'nvcc -o deviceQuery deviceQuery.cu && ./deviceQuery'
```

#### 4.2.4 预期结果

输出 `Detected 1 CUDA Capable device(s)`，设备名称为 `NVIDIA Tegra X2` 或类似标识，`CUDA Capability` 为 `6.2`。

### 4.3 测试三：TensorRT 8.2 与 YOLO 推理（关键路径）

#### 4.3.1 目标

验证单容器方案的核心假设：Jazzy 容器内可链接宿主挂载的 TensorRT 8.2，完成 ONNX 到 engine 的转换并执行推理。该测试同时覆盖四个风险点：CUDA 直通、TensorRT 可用、ONNX opset 兼容、Jazzy 与老工具链共存。

#### 4.3.2 前置：PC 导出 ONNX

在 PC 的 Python 环境中导出 YOLO 模型。TensorRT 8.2 的 ONNX 解析器支持的 opset 较老，导出时显式指定：

```python
from ultralytics import YOLO
model = YOLO("yolov8n.pt")
model.export(format="onnx", opset=12, imgsz=640)
```

将导出的 `model.onnx` 传入 NX。

#### 4.3.3 容器内转换与推理

在 Jazzy 容器内确认 TensorRT 可见：

```bash
docker run --network host -it -v /home/<user>/model.onnx:/work/model.onnx jazzy_minimal:arm64
```

在容器内执行：

```bash
trtexec --onnx=/work/model.onnx --saveEngine=/work/model.engine --fp16
ls -l /work/model.engine
```

若容器内缺少 `trtexec`，使用宿主路径或 TensorRT 提供的工具：

```bash
/usr/src/tensorrt/bin/trtexec --onnx=/work/model.onnx --saveEngine=/work/model.engine --fp16
```

#### 4.3.4 风险与处置

- 头文件可用性：NVIDIA runtime 挂载宿主 CUDA 与 TensorRT 的库，头文件是否一并挂载需实测。若容器内缺少 <NvInfer.h> 等头文件，在容器内安装匹配版本的 `libnvinfer-dev`（8.2），或挂载宿主的 `/usr/include`。
- 工具链差异：容器内 gcc 13 编译链接 TensorRT 8.2 头文件。若出现编译错误，记录错误信息并评估是否需要在容器内使用 gcc 11 或宿主的 gcc 7。
- opset 兼容：若 `trtexec` 报算子不支持，降低导出 opset。

#### 4.3.5 预期结果

`trtexec` 成功生成 `model.engine`，构建日志显示无 unsupported operator。加载 engine 并推理一张测试图，输出检测框。

### 4.4 执行顺序

依赖关系决定执行顺序：

```text
NX 磁盘配置
  -> NX Docker 与 runtime
  -> NX 数据目录迁移
  -> NX 网络代理
  -> PC Docker 与代理
  -> PC 跨架构构建
  -> 测试一（Jazzy 通信）
  -> 测试二（CUDA 直通）
  -> 测试三（TensorRT 与 YOLO）
```

测试一验证用户态与内核兼容，测试二验证 GPU 直通，测试三在两者之上验证 TensorRT 与老工具链共存。任一前置失败则后续测试无意义。

### 4.5 验收标准

量化指标如下，用于替代定性判断。

测试一：发布频率 $f = 10$ Hz，持续 $T = 30$ s，期望收到消息数 $N \ge 0.95 f T$，即 $N \ge 285$。

测试二：设备枚举结果 $\mathbf{d} = (d_1, \ldots, d_n)$ 满足 $|\mathbf{d}| = 1$；计算能力满足 $d_1.\text{major} = 6$ 且 $d_1.\text{minor} = 2$。

测试三：`trtexec` 退出码为 0，生成的 engine 文件非空；推理输出至少一个检测框，类别与置信度可解析。

失败处置：任一测试失败时，按版本控制历史回退相关改动，并在对应 `tests/` 文件中记录失败现象与已排除的假设。

### 4.6 测试记录规范

每次测试的改动与结果记录在 `tests/` 下的 Markdown 文件中，格式为：

```markdown
# 测试名称

## 改动目的

## 改动位置

## 预期效果

## 实际结果

## 结论
```

测试成功后，将临时配置整合入 `docker/` 与 `scripts/` 的正式文件。测试失败时，根据 Git 历史回退。

---

## 5. 风险与待确认事项

### 5.1 已确定事项

- NX 推理引擎锁定 TensorRT 8.2 / CUDA 10.2，不可升级。
- 容器方案为单容器：Jazzy 加宿主挂载的 CUDA 与 TensorRT，推理层用 C++。
- 镜像传输使用 `docker save` / `docker load`，不使用 registry。
- NX 不安装 Clash Verge，复用 PC 的 Allow LAN 代理。

### 5.2 待确认事项

事项 1：工作区根命名。本文档统一使用当前仓库 `~/Documents/TurtleBot3_TX2NX_ws/`，需确认是否与实际使用路径一致。

事项 2：SSD 当前是否已物理安装且被系统识别。执行 2.1 前用 `lsblk` 确认 `nvme0n1` 存在且无有用数据。

事项 3：NGC 镜像标签 `nvcr.io/nvidia/l4t-base:r32.7.1` 的可拉取性。执行 4.2 前用 `docker manifest inspect` 核实。

### 5.3 已知风险

风险 1：容器内 TensorRT 头文件与工具链的可用性，由测试三验证。

风险 2：Ubuntu 24.04 用户态在内核 4.9 上的稳定性。glibc 2.39 依赖的 `clone3`、`futex_waitv` 等新系统调用在 4.9 上不存在，glibc 会回退到旧调用。多数程序可运行，但行为需实测。由测试一第一层验证。
