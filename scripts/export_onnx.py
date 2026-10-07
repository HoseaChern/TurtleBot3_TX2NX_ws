"""导出 YOLO 模型为 ONNX, 供 NX 侧转换为 TensorRT engine.

TensorRT 8.2 的 ONNX 解析器支持的 opset 较老, 故显式指定 opset=12.
在 PC 具备 PyTorch 与 Ultralytics 的 Python 环境中运行.
"""

import shutil

from pathlib import Path

from ultralytics import YOLO

WEIGHTS = "yolov8n.pt"  # 预训练权重, 首次运行会自动下载
OPSET = 12  # TensorRT 8.2 的 ONNX 解析器支持的 opset
IMGSZ = 640  # 输入分辨率
OUTPUT_DIR = Path("models")  # ONNX 输出目录, 供同步到 NX


def main() -> None:
    """导出 ONNX 并移动到 models/ 下."""
    OUTPUT_DIR.mkdir(exist_ok=True)
    model = YOLO(WEIGHTS)
    exported = Path(model.export(format="onnx", opset=OPSET, imgsz=IMGSZ))
    shutil.move(str(exported), OUTPUT_DIR / exported.name)
    print(f"exported: {OUTPUT_DIR / exported.name}")


if __name__ == "__main__":
    main()
