# 模型要求: 需要自行实现 PPOCR 推理（双模型管线）
# 说明: PPOCR 需要 det + rec 两个模型协同，不适合直接用单 NPU 接口
# 此模板仅展示基础检测结果绘制

import visiong

def process(image, detections):
    """
    文字识别后处理示例。
    绘制文本检测框和识别文字。
    
    注意: PPOCR 需要 det + rec 双模型管线，
    当前 NPU 接口仅支持单模型，完整 OCR 需要特殊实现。
    """
    bgr = image.to_format("bgr")
    for det in detections:
        x, y, w, h = det.box
        # 文本框（绿色）
        bgr.draw_rectangle(x, y, w, h, color=(0, 255, 0), thickness=2)
        # 识别文字（黄色）
        if det.label:
            bgr.draw_string(x, y - 8, det.label, color=(255, 255, 0), scale=1.0, thickness=2)
    return bgr
