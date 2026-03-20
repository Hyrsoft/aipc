# 模型要求: YOLO11_SEG + yolo11_seg.rknn
# 标签文件: coco_80_labels_list.txt
# 请先在"模型管理"中上传 yolo11_seg.rknn 并选择加载

import visiong

def process(image, detections):
    """
    YOLO11 实例分割后处理。
    绘制检测框 + mask 轮廓线。
    """
    bgr = image.to_format("bgr")
    for det in detections:
        x, y, w, h = det.box
        # 检测框（绿色）
        bgr.draw_rectangle(x, y, w, h, color=(0, 255, 0), thickness=2)
        label = f"{det.label} {det.score:.0%}"
        bgr.draw_string(x, y - 8, label, color=(0, 255, 0), scale=1.0, thickness=2)

        # mask 轮廓（品红色连线）
        if det.mask_points and len(det.mask_points) > 1:
            pts = det.mask_points
            for i in range(len(pts)):
                x1, y1 = int(pts[i][0]), int(pts[i][1])
                x2, y2 = int(pts[(i + 1) % len(pts)][0]), int(pts[(i + 1) % len(pts)][1])
                bgr.draw_line(x1, y1, x2, y2, color=(255, 0, 255), thickness=2)
    return bgr
