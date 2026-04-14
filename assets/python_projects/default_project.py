# Default VisionG project: passthrough (run() contract)

import aipc
import visiong

_cam = None


def init():
    global _cam
    _cam = visiong.Camera(640, 360, format="rgb")


def run():
    while aipc.is_running():
        frame = _cam.snapshot()
        if frame.is_valid():
            aipc.submit_frame(frame.to_format("bgr888"))


def cleanup():
    global _cam
    if _cam:
        _cam.release()
        _cam = None
