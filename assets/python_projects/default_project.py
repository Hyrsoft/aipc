# Default VisionG project: passthrough
#
# 架构说明（Phase B）：
#   C++ 负责摄像头采集（Camera::snapshot()）并驱动帧循环，
#   每帧调用 process(frame) 传入原始帧；Python 只负责推理与绘制。
#   脚本不需要自建 Camera 或帧循环。
#
# 契约：
#   init()         可选，模块加载时调用一次（初始化模型等资源）
#   process(frame) 必须，每帧调用；返回 ImageBuffer 或 None（跳过该帧）
#   cleanup()      可选，模块卸载时调用一次（释放资源）


def init():
    pass


def process(frame):
    """
    透传示例：直接返回 C++ 提供的输入帧，不做任何处理。
    替换此函数内容即可实现自定义推理与绘制逻辑。
    """
    return frame


def cleanup():
    pass
