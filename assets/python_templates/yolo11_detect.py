# 模型要求: YOLO11 + yolo11.rknn
# 标签文件: coco_80_labels_list.txt
# 请先在"模型管理"中上传 yolo11.rknn 并选择加载

import visiong

def process(image, detections):
    """
    YOLO11 物体检测后处理。
    """
    bgr = image.to_format("bgr")
    for det in detections:
        x, y, w, h = det.box
        bgr.draw_rectangle(x, y, w, h, color=(0, 255, 0), thickness=3)
        label = f"{det.label} {det.score:.0%}"
        bgr.draw_string(x, y - 8, label, color=(0, 255, 0), scale=1.0, thickness=2)
    return bgr
