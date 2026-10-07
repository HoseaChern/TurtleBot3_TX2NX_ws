# TurtleBot3 TX2 NX Development Environment

This repository sets up the ROS 2 development environment for TurtleBot3 on a Jetson TX2 NX. The PC handles coding and training; the NX handles native compilation and inference. Full deployment details are in `docs/Env_Plan.md`.

## Environment

| Device | Role             | OS                                    | Key versions                                        |
| ------ | ---------------- | ------------------------------------- | --------------------------------------------------- |
| PC     | Coding/Training  | Ubuntu 24.04 amd64                    | ROS 2 Jazzy, CUDA 12.9, Python 3.12, PyTorch cu128  |
| NX     | Build/Inference  | Ubuntu 18.04 aarch64 (JetPack 4.6.6)  | CUDA 10.2, TensorRT 8.2, kernel 4.9.337             |

Division of labor:

- PC: write ROS 2 code in VSCode, train YOLO, export ONNX.
- NX: build the arm64 image natively, convert ONNX to a TensorRT engine, run the C++ inference node.

The two machines are connected point-to-point by a gigabit Ethernet cable (`192.168.60.0/24`). Source and models are synced with `rsync` over SSH. The rationale for this split is in Chapter 1 of `docs/Env_Plan.md`.

## Directory layout

```text
TurtleBot3_TX2NX_ws/
├── docs/          Plans and design documents
├── docker/        Image definitions per test (jazzy_minimal, cuda_test)
├── models/        ONNX exported on the PC and the converted engine
├── scripts/       Setup and sync scripts
└── src/           ROS 2 packages
```

## Daily startup

### 1. Direct link

The static IPs on the direct link are not persistent across reboots and must be reconfigured after boot. On the PC:

```bash
sudo ip addr add 192.168.60.1/24 dev enp8s0
sudo ip link set enp8s0 up
```

On the NX (interface name per `ip -br link`):

```bash
sudo ip addr add 192.168.60.2/24 dev eth0
sudo ip link set eth0 up
```

Verify connectivity from the PC:

```bash
ping -c 3 192.168.60.2
```

### 2. Proxy

Start Clash Verge on the PC and enable Allow LAN. The NX `dockerd` proxy is persisted by a systemd drop-in (`/etc/systemd/system/docker.service.d/proxy.conf`, pointing to `192.168.60.1:7897`), so it needs no reconfiguration as long as Clash runs on the PC.

CLI operations such as `docker login` and `docker buildx` read the shell proxy variables, which must be set in the current session (uppercase takes precedence):

```bash
export HTTP_PROXY=http://192.168.60.1:7897
export HTTPS_PROXY=http://192.168.60.1:7897
export http_proxy=http://192.168.60.1:7897
export https_proxy=http://192.168.60.1:7897
```

Pulling `nvcr.io` images requires an NGC login (username `$oauthtoken`, password is an NGC API Key). The credential is stored in `~/.docker/config.json` and persists:

```bash
echo '<NGC_API_KEY>' | docker login nvcr.io -u '$oauthtoken' --password-stdin
```

See `docs/Env_Plan.md` 4.3 for proxy and login configuration.

### 3. Sync the workspace

Run in the PC workspace root to sync the whole workspace to the NX (excluding generated build artifacts):

```bash
rsync -avz --delete -e ssh \
  --exclude 'build' --exclude 'install' --exclude 'log' --exclude 'models/*.engine' \
  ./ changli@192.168.60.2:~/TurtleBot3_TX2NX_ws/
```

### 4. Build and run

On the NX, build the image and run:

```bash
cd ~/TurtleBot3_TX2NX_ws
docker build -t jazzy_minimal:arm64 -f docker/jazzy_minimal/Dockerfile .
docker run --network host -it --name jazzy_test jazzy_minimal:arm64
```

During development there is no need to rebuild the image; mount the source and build inside the container, leaving artifacts on the NX disk:

```bash
docker run --rm -it \
  -v ~/TurtleBot3_TX2NX_ws/src:/ros2_ws/src \
  -v ~/TurtleBot3_TX2NX_ws/build:/ros2_ws/build \
  -v ~/TurtleBot3_TX2NX_ws/install:/ros2_ws/install \
  jazzy_minimal:arm64 \
  bash -c 'colcon build'
```

## Documents

- `docs/Env_Plan.md`: environment setup, build and experiment process.
- `AGENTS.md`: collaboration and code-discipline conventions for this repository.

## Roadmap

Navigation, perception and control modules will be added as separate ROS 2 packages under `src/` after the minimal validation passes. This document will be extended with the corresponding startup instructions.

## License

This project is licensed under the Apache License 2.0, see `LICENSE`.
