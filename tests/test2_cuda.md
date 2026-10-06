# 测试二：CUDA 直通

## 改动目的

验证 NVIDIA runtime 可将 Pascal GPU 直通给容器。该测试为测试三的前置条件，本身不涉及 TensorRT。

## 改动位置

- `docker/cuda_test/Dockerfile` 与 `deviceQuery.cu`。

## 预期效果

设备枚举结果 $\mathbf{d} = (d_1, \ldots, d_n)$ 满足 $|\mathbf{d}| = 1$；计算能力满足 $d_1.\text{major} = 6$ 且 $d_1.\text{minor} = 2$。

## 实际结果

待填。

## 结论

待填。
