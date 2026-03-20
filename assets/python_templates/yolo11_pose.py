# 模型要求: YOLO11_POSE + yolo11_pose.rknn
# 标签文件: coco_80_labels_list.txt
# 请先在"模型管理"中上传 yolo11_pose.rknn 并选择加载

import visiong

# COCO 17 关键点骨骼连接（16 条骨骼）
SKELETON = [
    (0, 1), (0, 2), (1, 3), (2, 4),       # 头部
    (5, 6),                                  # 肩膀
    (5, 7), (7, 9), (6, 8), (8, 10),        # 手臂
    (5, 11), (6, 12),                        # 躯干
    (11, 12),                                # 臀部
    (11, 13), (13, 15), (12, 14), (14, 16)  # 腿
]

KEYPOINT_THRESH = 0.3

def process(image, detections):
    """
    YOLO11 人体姿态估计后处理。
    绘制检测框 + 骨骼关键点 + 骨骼连线。
    """
    bgr = image.to_format("bgr")
    for det in detections:
        x, y, w, h = det.box
        # 人体框（绿色）
        bgr.draw_rectangle(x, y, w, h, color=(0, 255, 0), thickness=2)
        label = f"person {det.score:.0%}"
        bgr.draw_string(x, y - 8, label, color=(0, 255, 0), scale=1.0, thickness=2)

        if not det.keypoints:
            continue

        kpts = det.keypoints  # list of (x, y, score)

        # 绘制关键点（红色圆点）
        for kx, ky, ks in kpts:
            if ks > KEYPOINT_THRESH:
                bgr.draw_circle(int(kx), int(ky), 4, color=(0, 0, 255), thickness=-1)

        # 绘制骨骼连线（青色）
        for i, j in SKELETON:
            if i < len(kpts) and j < len(kpts):
                if kpts[i][2] > KEYPOINT_THRESH and kpts[j][2] > KEYPOINT_THRESH:
                    bgr.draw_line(
                        int(kpts[i][0]), int(kpts[i][1]),
                        int(kpts[j][0]), int(kpts[j][1]),
                        color=(0, 255, 255), thickness=2
                    )
    return bgr
