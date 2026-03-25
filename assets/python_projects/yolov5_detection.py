# YOLOv5 目标检测

import aipc
import visiong

MODEL_PATH = "../model/yolov5.rknn"
LABEL_PATH = "../model/coco_80_labels_list.txt"

CAM_FORMAT = "rgb"

BOX_THRESHOLD = 0.25
NMS_THRESHOLD = 0.45

_cam = None
_detector = None


def init():
    global _cam, _detector

    # 可选：提升 NPU 时钟以降低推理延迟
    try:
        visiong.NpuClock().set_rate_mhz(
            420,
            update_cru_clk500m_src=True,
            unbind_rebind_npu=True,
        )
    except Exception as e:
        print("[YOLOV5][WARN] NPU clock setup skipped:", e)

    _cam = visiong.Camera(640, 360, format="rgb")
    _cam.skip(8)

    _detector = visiong.NPU(
        "yolov5",
        MODEL_PATH,
        LABEL_PATH,
        box=BOX_THRESHOLD,
        nms=NMS_THRESHOLD,
    )
    print("[YOLOV5][INFO] detector loaded:", MODEL_PATH)


def run():
    while aipc.is_running():
        frame = _cam.snapshot()
        if not frame.is_valid():
            continue

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

        aipc.submit_frame(out)


def cleanup():
    global _cam, _detector
    if _cam:
        _cam.release()
        _cam = None
    _detector = None
    print("[YOLOV5][INFO] resources released")
