# YOLOv5 目标检测工程
#
# Phase B 契约：C++ 驱动帧循环，Python 只负责推理与绘制。
#   init()         可选，加载模型等一次性资源
#   process(frame) 必须，每帧调用；返回 ImageBuffer 或 None（跳过该帧）
#   cleanup()      可选，释放资源
#
# 不再需要在 init() 中创建 Camera，也不需要在 process() 中调用 snapshot()。
# C++ 已经完成采集，frame 是当前帧的 ImageBuffer（rgb 格式）。

import visiong

MODEL_PATH = "../model/yolov5.rknn"
LABEL_PATH = "../model/coco_80_labels_list.txt"

# 必须与 C++ ProducerConfig.ai_width / ai_height 以及摄像头格式一致
CAM_FORMAT = "rgb"

BOX_THRESHOLD = 0.25
NMS_THRESHOLD = 0.45

_detector = None


def init():
    global _detector

    # 可选：提升 NPU 时钟以降低推理延迟
    try:
        visiong.NpuClock().set_rate_mhz(
            420,
            update_cru_clk500m_src=True,
            unbind_rebind_npu=True,
        )
    except Exception as e:
        print("[YOLOV5][WARN] NPU clock setup skipped:", e)

    _detector = visiong.NPU(
        "yolov5",
        MODEL_PATH,
        LABEL_PATH,
        box=BOX_THRESHOLD,
        nms=NMS_THRESHOLD,
    )
    print("[YOLOV5][INFO] detector loaded:", MODEL_PATH)


def process(frame):
    """
    C++ 每帧调用本函数。
    :param frame: visiong.ImageBuffer，rgb 格式，尺寸由 C++ ProducerConfig 决定
    :return: 绘制了检测框的 ImageBuffer（bgr888），或 None 表示跳过本帧
    """
    if _detector is None or not frame.is_valid():
        return None

    # 转为 BGR 用于绘制（infer 仍使用原始 rgb 帧以匹配模型输入格式）
    out = frame.to_format("bgr888")

    for result in _detector.infer(frame, model_format=CAM_FORMAT):
        x, y, w, h = result.box
        out.draw_rectangle(x, y, w, h, color=(0, 255, 0), thickness=2)
        out.draw_string(
            x,
            max(0, y - 20),
            f"{result.label} {result.score:.2f}",
            color=(0, 255, 0),
            scale=0.9,
            thickness=2,
        )

    return out


def cleanup():
    global _detector
    _detector = None
    print("[YOLOV5][INFO] detector released")
