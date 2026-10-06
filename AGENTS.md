# TurtleBot3 TX2 NX 工程协作规范

本文件只固化本项目特有的流程与纪律, 通用协作规则见全局 AGENTS. 具体状态以 `docs/Env_Plan.md`、`tests/` 记录与实际代码为准, 行动前应先阅读这两者.

## 1. 权威来源

- 环境部署与实验计划: `docs/Env_Plan.md`.
- 测试目的、位置与结果: `tests/`.
- 进度、变更与历史: 以版本控制记录为准, 不在本文件复述.

## 2. 分工铁律

- 编码与训练在 PC, 编译与推理在 NX.
- PC 不安装 Docker; NX 的 arm64 镜像一律在 NX 原生构建, 不使用 QEMU 交叉构建.
- 镜像不经过 registry. 源码与模型经 `rsync` over SSH 同步, 走点对点千兆网线.

## 3. 构建与同步工作流

- 编辑在 PC, 在 PC 工作区根用 `rsync` 同步到 NX 的镜像目录.
- 开发迭代用挂载源码在容器内 `colcon build`, 不留中间产物于镜像; 仅在生成正式镜像时 `docker build`.
- 编译与运行步骤见 `docs/Env_Plan.md` 第 4 节.
