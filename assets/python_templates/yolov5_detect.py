# 模型要求: YOLOV5 + yolov5.rknn (内置)
# 标签文件: coco_80_labels_list.txt
# 默认模板 — YOLOv5 物体检测

import visiong

def process(image, detections):
    """
    YOLOv5 物体检测后处理。

    Args:
        image: visiong.ImageBuffer (NV12 原始帧)
        detections: list[visiong.Detection] (NPU 推理结果)

    Returns:
        visiong.ImageBuffer (BGR 帧，绘制了检测框和标签)
    """
    bgr = image.to_format("bgr")
    for det in detections:
        x, y, w, h = det.box
        bgr.draw_rectangle(x, y, w, h, color=(0, 255, 0), thickness=3)
        label = f"{det.label} {det.score:.0%}"
        bgr.draw_string(x, y - 8, label, color=(0, 255, 0), scale=1.0, thickness=2)
    return bgr
