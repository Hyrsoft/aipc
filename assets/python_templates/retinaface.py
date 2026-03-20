# 模型要求: RETINAFACE + retinaface.rknn
# 标签文件: 无
# 请先在"模型管理"中上传 retinaface.rknn 并选择加载

import visiong

def process(image, detections):
    """
    RetinaFace 人脸检测后处理。
    绘制人脸框、置信度和 5 个关键点（眼、鼻、嘴角）。
    """
    bgr = image.to_format("bgr")
    for det in detections:
        x, y, w, h = det.box
        # 人脸框（青色）
        bgr.draw_rectangle(x, y, w, h, color=(0, 255, 255), thickness=2)
        label = f"{det.score:.0%}"
        bgr.draw_string(x, y - 8, label, color=(0, 255, 255), scale=1.0, thickness=2)

        # 5 个面部关键点（蓝色圆点）
        if det.landmarks:
            for lx, ly in det.landmarks:
                bgr.draw_circle(int(lx), int(ly), 4, color=(255, 0, 0), thickness=-1)
    return bgr
