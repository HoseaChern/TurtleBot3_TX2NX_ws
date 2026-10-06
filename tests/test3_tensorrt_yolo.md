# 测试三：TensorRT 与 YOLO 推理

## 改动目的

验证单容器方案的核心假设：Jazzy 容器内可链接宿主挂载的 TensorRT 8.2，完成 ONNX 到 engine 的转换并执行推理。覆盖四个风险点：CUDA 直通、TensorRT 可用、ONNX opset 兼容、Jazzy 与老工具链共存。

## 改动位置

- `models/model.onnx`：由 PC 导出并同步的模型。
- 容器内 `trtexec` 转换与推理流程。

## 预期效果

`trtexec` 退出码为 0，生成的 engine 文件非空；推理输出至少一个检测框，类别与置信度可解析。

## 实际结果

待填。

## 结论

待填。
