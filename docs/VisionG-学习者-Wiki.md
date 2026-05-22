## 1) 快速开始

#### 1\. 放置库文件

将 visiong.py 与 \_visiong.so 放入当前 Python 环境可导入路径（如 site-packages）。

#### 2\. 检查导入

运行 python3 -c "import visiong; print(visiong.\_\_version\_\_)"，确认无报错。

#### 3\. 先跑最小样例

先验证 Camera + DisplayUDP，确认图像流正常，再叠加 IVE/NPU/GUI 模块。

#### 4\. 识别模块开关

某些类可能不存在（如 IVE/NPU/GUI），通常是构建时对应模块未启用。

#### 5\. AIPC 集成约束（必读）

在 AIPC 的 VisionG 模式中，Python 工程与独立示例脚本职责不同：

- Python 负责：采集、推理、绘制，并通过 `aipc.submit_frame(frame)` 提交处理后的 `visiong.ImageBuffer`。
- AIPC 负责：接收提交帧后执行 H.264 编码与流媒体分发（RTSP/WebRTC/WebSocket Preview/录制）。

因此，不要直接照搬包含 `DisplayUDP/DisplayHTTP/DisplayRTSP` 的独立脚本到 AIPC 工程。AIPC 工程建议采用以下接口：

- `init()`：初始化 Camera/NPU 等资源。
- `run()`：必须存在，驱动帧循环，通常使用 `while aipc.is_running():` 判断退出。
- `cleanup()`：可选，释放资源。

最小 AIPC 工程骨架：

```python
import visiong
import aipc

_cam = None

def init():
    global _cam
    _cam = visiong.Camera(640, 360, format='rgb')
    _cam.skip(8)

def run():
    while aipc.is_running():
        frame = _cam.snapshot()
        if not frame.is_valid():
            continue
        aipc.submit_frame(frame)

def cleanup():
    global _cam
    if _cam:
        _cam.release()
        _cam = None
```

预设工程可直接使用：`python_projects/yolov5_detection.py`。

## 2) 学习路径

1.  阶段 A：Camera + ImageBuffer（采集、格式转换、基础绘制）
2.  阶段 B：显示与传输（DisplayFB/UDP/RTSP/HTTP）
3.  阶段 C：IVE 算子（阈值、边缘、形态学、模板相关）
4.  阶段 D：NPU 任务（检测、OCR、跟踪、底层张量 IO）
5.  阶段 E：系统能力（PinMux、GPIO、ADC、NPU 时钟）
6.  阶段 F：GUI 交互（触控窗口、控件、画布绘制）

### 2.1 学习者排错顺序

-   先保证采集链路：Camera.snapshot() 得到 valid 图像，再进入算法/推流步骤。
-   每次只叠加一个能力模块：先显示，再加 IVE，再加 NPU，最后接 GUI 或系统控制。
-   遇到结果异常时优先检查输入格式（rgb/bgr/gray/yuv）和宽高是否符合模型/算子要求。
-   实时循环里先做最小逻辑，保证帧率，再逐步加绘制、日志、异常处理。
-   资源类对象统一在 finally 里释放（stop/release/close），避免句柄泄漏。
-   改参数遵循“一次只改一个维度”（阈值、分辨率、码率、NMS）便于定位效果变化。

### 2.2 实时循环通用骨架

```
import visiong

cam = visiong.Camera(640, 360, 'bgr')
out = visiong.DisplayUDP()
try:
    cam.skip(8)
    while True:
        frame = cam.snapshot()
        if not frame.is_valid():
            continue
        # 1) 可选预处理
        # 2) 可选算法推理
        # 3) 可选结果绘制
        out.display(frame)
finally:
    out.release()
    cam.release()
```

### 2.3 性能与稳定性清单

-   先降低分辨率再提 FPS：一般先从 320x240 或 640x360 起步。
-   算法输入格式和模型格式保持一致（rgb/bgr/gray），避免隐式转换损耗。
-   推流与推理分离思考：先测纯推流帧率，再叠加推理。
-   NPU 推理先记录耗时与置信度分布，再调整 box/nms 阈值。
-   长期运行时定期观察内存与句柄，确保 close/release/stop 都被调用。
-   线上脚本保留最低限度日志，并为关键模块加 try/finally 保护。

## 3) 任务快速参考

按“要做什么”而不是“类名”组织。先选任务，再进入对应 API 和示例。

#### 采集与预览

快速确认摄像头链路可用

**推荐组合：**Camera + DisplayUDP

**核心函数：**`snapshot, display, is_valid`

#### 图像预处理

在推理前做阈值/边缘/颜色空间处理

**推荐组合：**IVE + ImageBuffer

**核心函数：**`threshold, canny, sobel, csc`

#### 目标检测

做目标框与类别可视化

**推荐组合：**NPU + ImageBuffer

**核心函数：**`infer, draw_rectangle, draw_string`

#### OCR 识别

识别图中文字并回绘

**推荐组合：**PPOCR + ImageBuffer

**核心函数：**`infer, rect, text`

#### 目标跟踪

初始框后持续更新轨迹

**推荐组合：**NanoTrack + Camera

**核心函数：**`init, track`

#### 网络推流

浏览器/播放器实时查看画面

**推荐组合：**DisplayHTTP/RTSP/FLV

**核心函数：**`display, stop`

#### 录像保存

编码并落盘

**推荐组合：**VencRecorder + ImageBuffer

**核心函数：**`write, close, save_venc_h264/h265`

#### 系统 IO

GPIO/ADC/Pin 复用控制

**推荐组合：**PinMux + GpioLineConfig

**核心函数：**`gpio_request_line, gpio_set_value, read_adc`

#### GUI 交互

构建菜单、按钮和画布叠加

**推荐组合：**GUI + Touch + Canvas

**核心函数：**`begin_frame, begin_window, button, end_frame`

## 4) 脚本模板集（可直接起步）

覆盖采集、预处理、推理、推流四类最常见工程骨架。复制后替换参数即可。

#### 模板 A：采集 + 显示

最小可跑链路，先用它验证运行环境。

```
import visiong

cam = visiong.Camera(640, 360, 'bgr')
out = visiong.DisplayUDP()
try:
    cam.skip(8)
    while True:
        frame = cam.snapshot()
        if frame.is_valid():
            out.display(frame)
finally:
    out.release(); cam.release()
```

#### 模板 B：预处理 + 显示

在实时循环里接入 IVE 算子。

```
import visiong

cam = visiong.Camera(640, 360, 'gray')
ive = visiong.IVE()
out = visiong.DisplayUDP()
try:
    cam.skip(8)
    while True:
        src = cam.snapshot()
        if not src.is_valid():
            continue
        edge = ive.canny(src, high_thresh=120, low_thresh=40)
        out.display(edge)
finally:
    out.release(); cam.release()
```

#### 模板 C：推理 + 绘制 + 显示

通用 NPU 检测骨架。

```
import visiong

cam = visiong.Camera(640, 360, 'rgb')
npu = visiong.NPU('yolov5', '/path/to/model.rknn', '/path/to/labels.txt')
out = visiong.DisplayUDP()
try:
    cam.skip(8)
    while True:
        frame = cam.snapshot()
        if not frame.is_valid():
            continue
        vis = frame.to_format('bgr888')
        for det in npu.infer(frame, model_format='rgb'):
            x, y, w, h = det.box
            vis.draw_rectangle(x, y, w, h, color=(0, 255, 0), thickness=2)
        out.display(vis)
finally:
    out.release(); cam.release()
```

#### 模板 D：推流服务

将采集流发布为 HTTP/RTSP。

```
import visiong

cam = visiong.Camera(640, 360, 'yuv')
stream = visiong.DisplayRTSP(port=554, path='/live/0', codec='h265', fps=25)
try:
    cam.skip(8)
    while True:
        frame = cam.snapshot()
        if frame.is_valid():
            stream.display(frame)
finally:
    stream.stop(); cam.release()
```

## 5) 大量实战示例

手写案例按难度分层组织，从“最小跑通”到“组合实战”。建议按顺序练习。

**当前示例数量：**80（手写） + 453（逐函数自动示例）

#### 模块函数覆盖

7/7

#### 类方法覆盖

95/308

#### 类覆盖

17/43

查看覆盖薄弱点

**未覆盖模块函数：**无

**覆盖较低的类（手写案例）：**

-   `Blob`：0/1
-   `Circle`：0/1
-   `Detection`：0/1
-   `Line`：0/1
-   `MotionVector`：0/1
-   `OCRResult`：0/1
-   `PinAltFunction`：0/1
-   `PinId`：0/1
-   `PinMuxRegisterInfo`：0/1
-   `QRCode`：0/1
-   `TouchPoint`：0/1
-   `ImageBuffer`：4/39

### 1\. L1 入门基础：先跑通采集与显示 初级

10 个案例

掌握 Camera、DisplayUDP、ImageBuffer 的最小闭环，先看到稳定视频流。

#### 案例 1-1：最小实时预览

确认摄像头和 UDP 显示链路可用。

```
import visiong
cam = visiong.Camera(640, 360, format='bgr')
udp = visiong.DisplayUDP()
try:
    cam.skip(10)
    while True:
        img = cam.snapshot()
        if img.is_valid():
            udp.display(img)
finally:
    udp.release()
    cam.release()
```

#### 案例 1-2：抓一帧并保存 JPG

学习 snapshot + save 的最小离线流程。

```
import visiong
cam = visiong.Camera(640, 360, format='bgr')
try:
    cam.skip(5)
    img = cam.snapshot()
    if img.is_valid():
        img.save('frame.jpg', quality=90)
        print('saved frame.jpg')
finally:
    cam.release()
```

#### 案例 1-3：灰度转换并显示

把彩色帧转灰度，理解 to\_grayscale。

```
import visiong
cam = visiong.Camera(640, 360, format='bgr')
udp = visiong.DisplayUDP()
try:
    cam.skip(8)
    while True:
        frame = cam.snapshot()
        if not frame.is_valid():
            continue
        gray = frame.to_grayscale()
        udp.display(gray)
finally:
    udp.release()
    cam.release()
```

#### 案例 1-4：缩放 + letterbox

统一输出尺寸，避免直接拉伸变形。

```
import visiong
cam = visiong.Camera(1280, 720, format='bgr')
udp = visiong.DisplayUDP()
try:
    cam.skip(8)
    while True:
        src = cam.snapshot()
        if not src.is_valid():
            continue
        fit = src.letterbox(640, 360, color=(0, 0, 0))
        udp.display(fit)
finally:
    udp.release()
    cam.release()
```

#### 案例 1-5：基础绘制（线/框/圆/文字）

熟悉 draw\_line、draw\_rectangle、draw\_circle、draw\_string。

```
import visiong
img = visiong.ImageBuffer.create(640, 360, 'bgr888', (18, 24, 30))
img.draw_line(20, 20, 220, 20, color=(0, 255, 0), thickness=2)
img.draw_rectangle(40, 60, 180, 100, color=(255, 255, 0), thickness=2)
img.draw_circle(380, 180, 70, color=(255, 120, 0), thickness=3)
img.draw_string(30, 300, 'VisionG Drawing Demo', color=(255, 255, 255), scale=1.0, thickness=2)
img.save('draw_demo.jpg', quality=90)
```

#### 案例 1-6：NumPy 回路（拷贝）

to\_numpy + from\_numpy 的安全互通。

```
import numpy as np
import visiong
cam = visiong.Camera(640, 360, format='bgr')
udp = visiong.DisplayUDP()
try:
    cam.skip(8)
    while True:
        img = cam.snapshot()
        if not img.is_valid():
            continue
        arr = img.to_numpy(copy=True)
        arr[:, :, 1] = np.clip(arr[:, :, 1] + 25, 0, 255)
        out = visiong.ImageBuffer.from_numpy(arr, format='bgr888', copy=True)
        udp.display(out)
finally:
    udp.release()
    cam.release()
```

#### 案例 1-7：NumPy 零拷贝导入

from\_numpy\_zero\_copy 的最小示例（需 uint8、C 连续、偶数宽高）。

```
import numpy as np
import visiong
arr = np.zeros((240, 320, 3), dtype=np.uint8)
arr[:, :, 2] = 180
img = visiong.ImageBuffer.from_numpy_zero_copy(arr, format='bgr888')
img.save('zero_copy.jpg', quality=90)
```

#### 案例 1-8：裁剪 + 旋转 + 翻转

几何变换链路的常见写法。

```
import visiong
src = visiong.ImageBuffer.load('/path/to/input.jpg')
roi = src.crop(120, 60, 320, 220)
rot = roi.rotate(90)
out = rot.flip(horizontal=True, vertical=False)
out.save('geom_out.jpg', quality=90)
```

#### 案例 1-9：实时调亮度/对比度/饱和度

演示 Camera 参数调节对画面的影响。

```
import time
import visiong
cam = visiong.Camera(640, 360, format='bgr')
udp = visiong.DisplayUDP()
try:
    cam.skip(8)
    cam.set_brightness(150)
    cam.set_contrast(140)
    cam.set_saturation(140)
    t0 = time.time()
    while time.time() - t0 < 15:
        img = cam.snapshot()
        if img.is_valid():
            udp.display(img)
finally:
    udp.release()
    cam.release()
```

#### 案例 1-10：自动对焦与手动对焦

set\_focus\_mode / set\_manual\_focus / get\_focus\_position。

```
import time
import visiong
cam = visiong.Camera(640, 360, format='bgr')
udp = visiong.DisplayUDP()
try:
    cam.skip(8)
    cam.set_focus_mode('continuous')
    t0 = time.time()
    while time.time() - t0 < 5:
        img = cam.snapshot()
        if img.is_valid():
            img.draw_string(10, 10, 'AF', color=(0, 255, 0), scale=1.0, thickness=2)
            udp.display(img)
    cam.set_manual_focus(512)
    print('focus position =', cam.get_focus_position())
finally:
    udp.release()
    cam.release()
```

### 2\. L2 图像分析：从阈值到几何特征 初级→中级

10 个案例

使用 ImageBuffer 检测接口，完成简单视觉任务。

#### 案例 2-1：二值化（Otsu）

binarize 的最小实用流程。

```
import visiong
img = visiong.ImageBuffer.load('/path/to/input.jpg').to_grayscale()
bw = img.binarize(method='otsu')
bw.save('otsu.jpg', quality=90)
```

#### 案例 2-2：颜色块检测 find\_blobs

在实时画面中定位阈值颜色块。

```
import visiong
cam = visiong.Camera(640, 360, format='bgr')
udp = visiong.DisplayUDP()
thresholds = [(0, 80)]
try:
    cam.skip(8)
    while True:
        img = cam.snapshot()
        if not img.is_valid():
            continue
        for b in img.find_blobs(thresholds, area_threshold=150, pixels_threshold=150):
            img.draw_rectangle(b.x, b.y, b.w, b.h, color=(0, 255, 0), thickness=2)
        udp.display(img)
finally:
    udp.release()
    cam.release()
```

#### 案例 2-3：直线检测 find\_lines

霍夫线检测并绘制。

```
import visiong
cam = visiong.Camera(640, 360, format='gray')
udp = visiong.DisplayUDP()
try:
    cam.skip(8)
    while True:
        img = cam.snapshot()
        if not img.is_valid():
            continue
        vis = img.to_format('bgr888')
        lines = img.find_lines(0, 0, img.width, img.height, threshold=70)
        for ln in lines:
            vis.draw_line(ln.x1, ln.y1, ln.x2, ln.y2, color=(0, 255, 0), thickness=2)
        udp.display(vis)
finally:
    udp.release()
    cam.release()
```

#### 案例 2-4：圆检测 find\_circles

检测圆形目标并标注圆心。

```
import visiong
img = visiong.ImageBuffer.load('/path/to/circle_input.jpg').to_grayscale()
vis = img.to_format('bgr888')
circles = img.find_circles(0, 0, img.width, img.height, threshold=45, r_min=8, r_max=80)
for c in circles:
    vis.draw_circle(c.cx, c.cy, c.r, color=(255, 220, 0), thickness=2)
    vis.draw_cross(c.cx, c.cy, color=(255, 220, 0), size=8, thickness=2)
vis.save('circles.jpg', quality=90)
```

#### 案例 2-5：透视矫正 warp\_perspective

把斜拍区域拉正到固定尺寸。

```
import visiong
src = visiong.ImageBuffer.load('/path/to/doc.jpg')
quad = [(50, 60), (540, 40), (580, 330), (40, 340)]
flat = src.warp_perspective(quad, 480, 320)
flat.save('doc_flat.jpg', quality=90)
```

#### 案例 2-6：方块检测 find\_squares

阈值+轮廓组合的稳定写法。

```
import visiong
img = visiong.ImageBuffer.load('/path/to/square_input.jpg').to_grayscale()
squares = img.find_squares((0, 0, img.width, img.height), threshold_val=120, min_area=400)
print('square count =', len(squares))
```

#### 案例 2-7：多边形检测 find\_polygons

统计多边形候选目标数量。

```
import visiong
img = visiong.ImageBuffer.load('/path/to/polygon_input.jpg').to_grayscale()
items = img.find_polygons(0, 0, img.width, img.height, min_area=100, max_area=100000, min_sides=3, max_sides=10, accuracy='normal')
print('polygon count =', len(items))
```

#### 案例 2-8：二维码定位与连线绘制

读取 payload，同时用四角点连线。

```
import visiong
cam = visiong.Camera(640, 360, format='bgr')
udp = visiong.DisplayUDP()
try:
    cam.skip(8)
    while True:
        img = cam.snapshot()
        if not img.is_valid():
            continue
        for qr in img.find_qrcodes():
            c = qr.corners
            img.draw_line(c[0][0], c[0][1], c[1][0], c[1][1], color=(0, 255, 255), thickness=2)
            img.draw_line(c[1][0], c[1][1], c[2][0], c[2][1], color=(0, 255, 255), thickness=2)
            img.draw_line(c[2][0], c[2][1], c[3][0], c[3][1], color=(0, 255, 255), thickness=2)
            img.draw_line(c[3][0], c[3][1], c[0][0], c[0][1], color=(0, 255, 255), thickness=2)
            txt = qr.payload.decode('utf-8', 'ignore')
            img.draw_string(10, 10, txt, color=(0, 255, 255), scale=1.0, thickness=2)
        udp.display(img)
finally:
    udp.release()
    cam.release()
```

#### 案例 2-9：限定 ROI 的直线检测

只在中间区域做检测，降低噪声干扰。

```
import visiong
cam = visiong.Camera(640, 360, format='gray')
udp = visiong.DisplayUDP()
roi = (120, 60, 400, 240)
try:
    cam.skip(8)
    while True:
        g = cam.snapshot()
        if not g.is_valid():
            continue
        vis = g.to_format('bgr888')
        vis.draw_rectangle(*roi, color=(0, 255, 255), thickness=1)
        for ln in g.find_lines(*roi, threshold=65):
            vis.draw_line(ln.x1, ln.y1, ln.x2, ln.y2, color=(0, 255, 0), thickness=2)
        udp.display(vis)
finally:
    udp.release()
    cam.release()
```

#### 案例 2-10：离线批量预处理

load -> resize -> save，常用于数据准备。

```
import visiong
inputs = ['/path/to/a.jpg', '/path/to/b.jpg', '/path/to/c.jpg']
for i, p in enumerate(inputs, 1):
    img = visiong.ImageBuffer.load(p)
    out = img.resize(320, 320)
    out.save(f'out_{i:02d}.jpg', quality=92)
```

### 3\. L3 IVE 专题：硬件算子组合 中级

10 个案例

熟悉 IVE 常用算子组合，搭建可复用预处理链路。

#### 案例 3-1：5x5 卷积滤波

IVE.filter 处理锐化/平滑核。

```
import visiong
ive = visiong.IVE()
src = visiong.ImageBuffer.load('/path/to/input.jpg').to_grayscale()
sharpen = [0, -1, 0, -1, 5, -1, 0, -1, 0]
dst = ive.filter(src, sharpen)
dst.save('ive_filter.jpg', quality=90)
```

#### 案例 3-2：Sobel + 16bit 转 8bit

sobel 输出再 cast\_16bit\_to\_8bit 可视化。

```
import visiong
ive = visiong.IVE()
gray = visiong.ImageBuffer.load('/path/to/input.jpg').to_grayscale()
gx, gy = ive.sobel(gray, visiong.SobelOutCtrl.BOTH, visiong.ImageTypeIVE.S16C1)
gx8 = ive.cast_16bit_to_8bit(gx, visiong.Cast16to8Mode.S16_TO_U8_ABS)
gy8 = ive.cast_16bit_to_8bit(gy, visiong.Cast16to8Mode.S16_TO_U8_ABS)
gx8.save('sobel_x.jpg', quality=90)
gy8.save('sobel_y.jpg', quality=90)
```

#### 案例 3-3：Canny 边缘

ive.canny 直接输出边缘图。

```
import visiong
ive = visiong.IVE()
gray = visiong.ImageBuffer.load('/path/to/input.jpg').to_grayscale()
edge = ive.canny(gray, high_thresh=120, low_thresh=40)
edge.save('canny.jpg', quality=90)
```

#### 案例 3-4：膨胀 + 腐蚀

形态学后处理去噪。

```
import visiong
ive = visiong.IVE()
gray = visiong.ImageBuffer.load('/path/to/input.jpg').to_grayscale()
bw = gray.binarize(method='otsu')
clean = ive.erode(bw, kernel_size=3)
clean = ive.dilate(clean, kernel_size=3)
clean.save('morph.jpg', quality=90)
```

#### 案例 3-5：图像相加/相减/逻辑与

ive.add / ive.sub / ive.logic\_op 常见组合。

```
import visiong
ive = visiong.IVE()
a = visiong.ImageBuffer.load('/path/to/a.jpg').to_grayscale()
b = visiong.ImageBuffer.load('/path/to/b.jpg').to_grayscale()
mix = ive.add(a, b)
diff = ive.sub(a, b, mode=visiong.SubMode.ABS)
mask = ive.logic_op(a, b, op=visiong.LogicOp.AND)
mix.save('ive_add.jpg', quality=90)
diff.save('ive_sub.jpg', quality=90)
mask.save('ive_and.jpg', quality=90)
```

#### 案例 3-6：角点 + 光流

st\_corner 找特征点，再 lk\_optical\_flow 跟踪。

```
import visiong
ive = visiong.IVE()
prev = visiong.ImageBuffer.load('/path/to/frame_prev.jpg').to_grayscale()
curr = visiong.ImageBuffer.load('/path/to/frame_curr.jpg').to_grayscale()
pts = ive.st_corner(prev, max_corners=120, min_dist=8, quality_level=20)
mvs = ive.lk_optical_flow(prev, curr, pts)
print('track points =', len(mvs))
```

#### 案例 3-7：直方图与均衡化

ive.hist + ive.equalize\_hist。

```
import visiong
ive = visiong.IVE()
gray = visiong.ImageBuffer.load('/path/to/input.jpg').to_grayscale()
hist = ive.hist(gray)
print('hist bins =', len(hist))
eq = ive.equalize_hist(gray)
eq.save('equalized.jpg', quality=90)
```

#### 案例 3-8：LUT 映射增强

ive.map 对灰度图进行查表处理。

```
import visiong
ive = visiong.IVE()
src = visiong.ImageBuffer.load('/path/to/input.jpg').to_grayscale()
lut = [min(255, int(i * 1.2)) for i in range(256)]
dst = ive.map(src, lut)
dst.save('lut_map.jpg', quality=90)
```

#### 案例 3-9：模板相似度 ncc

比较两张图的归一化相关系数。

```
import visiong
ive = visiong.IVE()
a = visiong.ImageBuffer.load('/path/to/template.jpg').to_grayscale()
b = visiong.ImageBuffer.load('/path/to/search.jpg').to_grayscale()
score = ive.ncc(a, b)
print('ncc score =', score)
```

#### 案例 3-10：多尺度金字塔

ive.create\_pyramid 生成多尺度图像序列。

```
import visiong
ive = visiong.IVE()
src = visiong.ImageBuffer.load('/path/to/input.jpg').to_grayscale()
levels = ive.create_pyramid(src, levels=4)
for i, lv in enumerate(levels):
    lv.save(f'pyr_{i}.jpg', quality=90)
```

### 4\. L4 传输与录制：从本地显示到网络推流 中级

10 个案例

掌握 UDP/FB/RTSP/HTTP 与编码录制能力。

#### 案例 4-1：UDP 指定 IP 与质量

DisplayUDP 初始化参数的常用写法。

```
import visiong
cam = visiong.Camera(640, 360, format='bgr')
udp = visiong.DisplayUDP(udp_ip='192.168.1.100', udp_port=8000, jpeg_quality=80)
try:
    cam.skip(8)
    while True:
        img = cam.snapshot()
        if img.is_valid():
            udp.display(img)
finally:
    udp.release()
    cam.release()
```

#### 案例 4-2：Framebuffer 显示

本地屏显示，包含 roi 参数。

```
import visiong
cam = visiong.Camera(640, 360, format='bgr')
fb = visiong.DisplayFB()
try:
    cam.skip(8)
    while True:
        frame = cam.snapshot()
        if frame.is_valid():
            fb.display(frame, (0, 0, frame.width, frame.height))
finally:
    fb.release()
    cam.release()
```

#### 案例 4-3：RTSP 推流

DisplayRTSP 持续推流，退出前 stop。

```
import visiong
cam = visiong.Camera(640, 360, format='yuv')
rtsp = visiong.DisplayRTSP(port=554, path='/live/0', codec='h265', quality=72, fps=25)
try:
    cam.skip(8)
    while True:
        frame = cam.snapshot()
        if frame.is_valid():
            rtsp.display(frame)
finally:
    rtsp.stop()
    cam.release()
```

#### 案例 4-4：HTTP MJPEG

浏览器访问设备 IP:8080 查看实时画面。

```
import visiong
cam = visiong.Camera(640, 360, format='bgr')
http = visiong.DisplayHTTP(port=8080, quality=75, mode='jpg')
try:
    cam.skip(8)
    while True:
        frame = cam.snapshot()
        if frame.is_valid():
            http.display(frame)
finally:
    http.stop()
    cam.release()
```

#### 案例 4-5：HTTP-FLV

DisplayHTTPFLV 发布 /live.flv。

```
import visiong
cam = visiong.Camera(640, 360, format='yuv')
flv = visiong.DisplayHTTPFLV(port=8080, path='/live.flv', codec='h264', fps=25, quality=70)
try:
    cam.skip(8)
    while True:
        frame = cam.snapshot()
        if frame.is_valid():
            flv.display(frame)
finally:
    flv.stop()
    cam.release()
```

#### 案例 4-6：VencRecorder 录 MP4

录制固定时长，结束 close。

```
import visiong
cam = visiong.Camera(640, 360, format='yuv')
rec = visiong.VencRecorder('demo.mp4', codec='h264', container='mp4', quality=72, fps=25)
try:
    cam.skip(8)
    for _ in range(300):
        frame = cam.snapshot()
        if frame.is_valid():
            rec.write(frame)
finally:
    rec.close()
    cam.release()
```

#### 案例 4-7：单帧硬编 JPG

save\_venc\_jpg 在不跑循环时快速验证编码。

```
import visiong
img = visiong.ImageBuffer.load('/path/to/input.jpg')
img.save_venc_jpg('out.jpg', quality=80)
```

#### 案例 4-8：单帧硬编 H264/H265

save\_venc\_h264 与 save\_venc\_h265 对比。

```
import visiong
img = visiong.ImageBuffer.load('/path/to/input.jpg').to_format('yuv420sp')
img.save_venc_h264('out.h264', quality=75, rc_mode='cbr', fps=25, append=False)
img.save_venc_h265('out.h265', quality=75, rc_mode='cbr', fps=25, append=False)
```

#### 案例 4-9：运行中动态调推流参数

set\_fps / set\_quality 用于在线调优。

```
import time
import visiong
cam = visiong.Camera(640, 360, format='yuv')
rtsp = visiong.DisplayRTSP(port=554, path='/live/0', codec='h264', quality=75, fps=30)
try:
    cam.skip(8)
    switched = False
    t0 = time.time()
    while True:
        frame = cam.snapshot()
        if not frame.is_valid():
            continue
        if (not switched) and time.time() - t0 > 10:
            rtsp.set_fps(20)
            rtsp.set_quality(65)
            switched = True
        rtsp.display(frame)
finally:
    rtsp.stop()
    cam.release()
```

#### 案例 4-10：按路径关闭录制器

close\_venc\_recorder 在异常恢复时很实用。

```
import visiong
visiong.close_venc_recorder('demo.mp4')
```

### 5\. L5 NPU 实战：检测、识别、跟踪 中级→高级

10 个案例

从高层 NPU 到 LowLevelNPU，覆盖常见模型流程。

#### 案例 5-1：YOLOv5 检测并绘框

NPU.infer 返回 Detection 列表。

```
import visiong
MODEL = '/path/to/yolov5s.rknn'
LABELS = '/path/to/coco_80_labels_list.txt'
cam = visiong.Camera(640, 360, format='rgb')
udp = visiong.DisplayUDP()
det = visiong.NPU('yolov5', MODEL, LABELS, box=0.25, nms=0.45)
try:
    cam.skip(8)
    while True:
        frame = cam.snapshot()
        if not frame.is_valid():
            continue
        out = frame.to_format('bgr888')
        for r in det.infer(frame, model_format='rgb'):
            x, y, w, h = r.box
            out.draw_rectangle(x, y, w, h, color=(0, 255, 0), thickness=2)
            out.draw_string(x, max(0, y - 20), f'{r.label} {r.score:.2f}', color=(0, 255, 0), scale=0.9, thickness=2)
        udp.display(out)
finally:
    udp.release()
    cam.release()
```

#### 案例 5-2：FaceNet 特征比对

get\_face\_feature + get\_feature\_distance。

```
import visiong
net = visiong.NPU('facenet', '/path/to/facenet.rknn')
a = visiong.ImageBuffer.load('/path/to/face_a.jpg').to_format('rgb888')
b = visiong.ImageBuffer.load('/path/to/face_b.jpg').to_format('rgb888')
fa = net.get_face_feature(a)
fb = net.get_face_feature(b)
dist = visiong.NPU.get_feature_distance(fa, fb)
print('distance =', dist)
```

#### 案例 5-3：LPRNet 车牌识别

NPU.recognize\_plate 最小调用。

```
import visiong
lpr = visiong.NPU('lprnet', '/path/to/lprnet.rknn')
img = visiong.ImageBuffer.load('/path/to/plate.jpg').to_format('rgb888')
text = lpr.recognize_plate(img)
print('plate =', text)
```

#### 案例 5-4：PPOCR 文本识别

det + rec 结果输出 text 与框。

```
import visiong
ocr = visiong.PPOCR(
    det_model_path='/path/to/ppocrv3_det.rknn',
    rec_model_path='/path/to/ppocrv4_rec.rknn',
    dict_path='/path/to/ppocr_keys_v1.txt',
    model_input_format='rgb'
)
img = visiong.ImageBuffer.load('/path/to/text.jpg').to_format('rgb888')
for item in ocr.infer(img):
    print(item.text, item.text_score, item.rect)
```

#### 案例 5-5：NanoTrack 单目标跟踪

初始化 bbox 后持续 track。

```
import visiong
tracker = visiong.NanoTrack('/path/to/T.rknn', '/path/to/X.rknn', '/path/to/H.rknn')
cam = visiong.Camera(640, 360, format='rgb')
udp = visiong.DisplayUDP()
try:
    cam.skip(8)
    first = cam.snapshot()
    tracker.init(first, (220, 120, 100, 100))
    while True:
        frame = cam.snapshot()
        if not frame.is_valid():
            continue
        tr = tracker.track(frame)
        x, y, w, h = tr.box
        vis = frame.to_format('bgr888')
        vis.draw_rectangle(x, y, w, h, color=(0, 255, 0), thickness=2)
        vis.draw_string(10, 10, f'score={tr.score:.2f}', color=(0, 255, 0), scale=1.0, thickness=2)
        udp.display(vis)
finally:
    udp.release()
    cam.release()
```

#### 案例 5-6：LowLevelNPU 数组输入

set\_input\_array + run + output\_array。

```
import numpy as np
import visiong
ll = visiong.LowLevelNPU('/path/to/model.rknn')
x = np.zeros((1, 224, 224, 3), dtype=np.uint8)
ll.set_input_array(0, x, quantize_if_needed=True, zero_pad=True, sync_to_device=True)
ll.run(sync_outputs=True)
y = ll.output_array(0, dequantize_if_needed=True, sync_from_device=True)
print('output shape =', getattr(y, 'shape', None))
```

#### 案例 5-7：ROI 检测推理

infer 传 roi，减少无关区域。

```
import visiong
npu = visiong.NPU('yolov5', '/path/to/model.rknn', '/path/to/labels.txt')
img = visiong.ImageBuffer.load('/path/to/input.jpg').to_format('rgb888')
res = npu.infer(img, roi=(80, 40, 480, 280), model_format='rgb')
print('det count =', len(res))
```

#### 案例 5-8：LowLevelNPU 图像输入

set\_input\_image + output\_float。

```
import visiong
ll = visiong.LowLevelNPU('/path/to/model.rknn')
img = visiong.ImageBuffer.load('/path/to/input.jpg').to_format('rgb888')
ll.set_input_image(0, img, color_order='rgb', keep_aspect=True, pad_value=114, driver_convert=True)
ll.run(sync_outputs=True)
out = ll.output_float(0, dequantize_if_needed=True, sync_from_device=True)
print('len(out) =', len(out))
```

#### 案例 5-9：打印 tensor 元信息

快速核对模型输入输出维度、量化信息。

```
import visiong
ll = visiong.LowLevelNPU('/path/to/model.rknn')
for t in ll.input_tensors():
    print('in', t.index, t.name, t.dims, t.type, t.format, t.scale, t.zero_point)
for t in ll.output_tensors():
    print('out', t.index, t.name, t.dims, t.type, t.format, t.scale, t.zero_point)
```

#### 案例 5-10：异步 run + wait

non\_block 推理模式的最小模板。

```
import numpy as np
import visiong
ll = visiong.LowLevelNPU('/path/to/model.rknn')
x = np.zeros((1, 224, 224, 3), dtype=np.uint8)
ll.set_input_array(0, x)
ll.run(sync_outputs=False, non_block=True, timeout_ms=0)
ok = ll.wait(timeout_ms=1000)
print('wait ok =', ok)
y = ll.output_bytes(0, with_stride=False, sync_from_device=True)
print('output bytes =', len(y))
```

### 6\. L6 系统与交互：PinMux、时钟、触控、GUI 高级

10 个案例

补齐系统控制与交互能力，形成完整端侧应用闭环。

#### 案例 6-1：Pin 名解析与复用查询

parse\_pin + get\_mux + get\_function\_name。

```
import visiong
pm = visiong.PinMux()
try:
    pid = pm.parse_pin('GPIO0_C3')
    print('bank/pin =', pid.bank, pid.pin)
    print('mux =', pm.get_mux(pid.bank, pid.pin))
    print('func =', pm.get_function_name(pid.bank, pid.pin))
finally:
    pm.close()
```

#### 案例 6-2：GPIO 输出闪烁

gpio\_request\_line / gpio\_set\_value / gpio\_release\_line。

```
import time
import visiong
pm = visiong.PinMux()
pid = pm.parse_pin('GPIO0_C3')
cfg = visiong.GpioLineConfig()
cfg.direction = 'out'
cfg.default_value = 0
try:
    pm.gpio_request_line(pid.bank, pid.pin, cfg)
    for i in range(10):
        pm.gpio_set_value(pid.bank, pid.pin, i % 2)
        time.sleep(0.2)
finally:
    pm.gpio_release_line(pid.bank, pid.pin)
    pm.close()
```

#### 案例 6-3：ADC 采样

list\_adc\_channels + read\_adc。

```
import visiong
pm = visiong.PinMux()
try:
    chs = pm.list_adc_channels()
    print('adc channels =', chs)
    s = pm.read_adc(0)
    print('raw =', s.raw, 'mv =', s.millivolts)
finally:
    pm.close()
```

#### 案例 6-4：接口冲突检查

check\_conflict + ensure\_interface 用于切换外设功能。

```
import visiong
pm = visiong.PinMux()
pid = pm.parse_pin('GPIO0_C3')
try:
    rep = pm.check_conflict(pid.bank, pid.pin, target_function_or_group='uart4')
    print('conflict =', rep.conflict, rep.reason)
    ok = pm.ensure_interface('uart4')
    print('ensure uart4 =', ok)
finally:
    pm.close()
```

#### 案例 6-5：NPU 时钟查看与设置

NpuClock.status + set\_rate\_mhz。

```
import visiong
clk = visiong.NpuClock()
st = clk.status()
print('current hz =', st.current_rate_hz)
ret = clk.set_rate_mhz(500, update_cru_clk500m_src=True, unbind_rebind_npu=False, allow_unsafe_rate=False)
print('ok =', ret.ok, 'assigned =', ret.assigned_rate_hz, 'message =', ret.message)
```

#### 案例 6-6：GUI 最小窗口

GUI 基础帧循环。

```
import visiong
W, H = 320, 240
gui = visiong.GUI(W, H)
touch = visiong.Touch()
canvas_img = visiong.ImageBuffer.create(W, H, 'bgr888', (20, 20, 20))
while True:
    gui.begin_frame(touch)
    if gui.begin_window('Demo', 10, 10, 280, 200):
        gui.layout_row_dynamic(28, 1)
        gui.label('Hello VisionG GUI', 'left')
        gui.button('OK')
    gui.end_window()
    gui.end_frame(canvas_img)
```

#### 案例 6-7：GPIO 输入读取

gpio\_get\_value + gpio\_get\_status。

```
import visiong
pm = visiong.PinMux()
pid = pm.parse_pin('GPIO0_C4')
cfg = visiong.GpioLineConfig()
cfg.direction = 'in'
try:
    pm.gpio_request_line(pid.bank, pid.pin, cfg)
    val = pm.gpio_get_value(pid.bank, pid.pin)
    st = pm.gpio_get_status(pid.bank, pid.pin)
    print('value =', val, 'requested =', st.requested)
finally:
    pm.gpio_release_line(pid.bank, pid.pin)
    pm.close()
```

#### 案例 6-8：驱动能力配置

set\_drive\_strength / set\_pull / set\_input\_schmitt。

```
import visiong
pm = visiong.PinMux()
pid = pm.parse_pin('GPIO0_C3')
try:
    pm.set_drive_strength(pid.bank, pid.pin, 3)
    pm.set_pull(pid.bank, pid.pin, 'up')
    pm.set_input_schmitt(pid.bank, pid.pin, True)
    print('drive =', pm.get_drive_strength(pid.bank, pid.pin).level)
    print('pull =', pm.get_pull(pid.bank, pid.pin).mode)
    print('schmitt =', pm.get_input_schmitt(pid.bank, pid.pin).enabled)
finally:
    pm.close()
```

#### 案例 6-9：触摸坐标读取与映射配置

Touch.configure\_geometry + read。

```
import time
import visiong
touch = visiong.Touch()
touch.configure_geometry(240, 320, 270)
try:
    for _ in range(200):
        pts = touch.read()
        if pts:
            print([(p.x, p.y) for p in pts])
        time.sleep(0.02)
finally:
    touch.release()
```

#### 案例 6-10：GUI 滑条控制亮度

GUI + Camera + DisplayFB 联动最小示例。

```
import visiong
W, H = 320, 240
cam = visiong.Camera(W, H, format='bgr')
fb = visiong.DisplayFB()
touch = visiong.Touch()
gui = visiong.GUI(W, H)
brightness = 128
try:
    cam.skip(8)
    while True:
        frame = cam.snapshot()
        if not frame.is_valid():
            continue
        gui.begin_frame(touch)
        if gui.begin_window('Camera Control', 8, 8, 304, 110):
            gui.layout_row_dynamic(28, 1)
            brightness = int(gui.slider('Brightness', brightness, 0, 255, 1))
            cam.set_brightness(brightness)
        gui.end_window()
        gui.end_frame(frame)
        fb.display(frame, (0, 0, frame.width, frame.height))
finally:
    fb.release()
    touch.release()
    cam.release()
```

### 7\. L7 运行诊断与模块函数 中级→高级

10 个案例

补齐模块级函数与运行诊断能力，方便线上排错与自检。

#### 案例 7-1：设备唯一 ID

用 get\_unique\_id 标识设备实例。

```
import visiong
uid = visiong.get_unique_id()
print('unique id =', uid)
```

#### 案例 7-2：兼容值解密

decrypt\_legacy\_value 的最小调用。

```
import visiong
plain = visiong.decrypt_legacy_value('your-secret')
print('decrypted =', plain)
```

#### 案例 7-3：读取 DMA 统计

dma\_state\_metrics 用于观察 DMA 状态。

```
import visiong
m = visiong.dma_state_metrics(reset=False)
print('dma metrics =', m)
```

#### 案例 7-4：重置 DMA 统计

清空历史统计，重新开始采样。

```
import visiong
visiong.dma_state_reset_metrics()
```

#### 案例 7-5：导出 DMA 报告

将统计信息导出为文件留档。

```
import visiong
ok = visiong.dma_state_dump_metrics(output_path='dma_metrics.json', reset_after_dump=False)
print('dump ok =', ok)
```

#### 案例 7-6：关闭指定录制器

防止异常中断后句柄残留。

```
import visiong
visiong.close_venc_recorder('demo.mp4')
```

#### 案例 7-7：关闭全部录制器

服务重启前做统一清理。

```
import visiong
visiong.close_all_venc_recorders()
```

#### 案例 7-8：模块可用性检查

通过 hasattr 检查构建开关是否启用。

```
import visiong
mods = ['IVE', 'NPU', 'PPOCR', 'NanoTrack', 'GUI', 'PinMux']
for name in mods:
    print(name, hasattr(visiong, name))
```

#### 案例 7-9：关键组件初始化自检

启动前快速检查核心能力是否可用。

```
import visiong
cam = visiong.Camera(320, 240, format='bgr')
udp = visiong.DisplayUDP()
npu = visiong.NPU('yolov5', '/path/to/model.rknn', '/path/to/labels.txt')
print('cam ok =', cam.is_initialized())
print('udp ok =', udp.is_initialized())
print('npu ok =', npu.is_initialized())
udp.release()
cam.release()
```

#### 案例 7-10：统一资源清理骨架

学习者可复用的 try/finally 模板。

```
import visiong
cam = udp = rtsp = rec = None
try:
    cam = visiong.Camera(640, 360, format='yuv')
    udp = visiong.DisplayUDP()
    rtsp = visiong.DisplayRTSP(port=554, path='/live/0')
    rec = visiong.VencRecorder('demo.mp4')
    # ... your loop ...
finally:
    if rec is not None:
        rec.close()
    if rtsp is not None:
        rtsp.stop()
    if udp is not None:
        udp.release()
    if cam is not None:
        cam.release()
    visiong.close_all_venc_recorders()
```

### 8\. L8 综合项目模板（端到端） 高级

10 个案例

给出可直接扩展的端到端脚手架，缩短从 Demo 到项目的距离。

#### 案例 8-1：采集 + IVE 边缘 + UDP

实时边缘流模板。

```
import visiong
cam = visiong.Camera(640, 360, format='gray')
udp = visiong.DisplayUDP()
ive = visiong.IVE()
try:
    cam.skip(8)
    while True:
        g = cam.snapshot()
        if not g.is_valid():
            continue
        edge = ive.canny(g, high_thresh=120, low_thresh=40)
        udp.display(edge)
finally:
    udp.release()
    cam.release()
```

#### 案例 8-2：采集 + 检测 + RTSP

目标检测结果叠加后推 RTSP。

```
import visiong
cam = visiong.Camera(640, 360, format='rgb')
rtsp = visiong.DisplayRTSP(port=554, path='/live/0', codec='h264', fps=25)
det = visiong.NPU('yolov5', '/path/to/model.rknn', '/path/to/labels.txt')
try:
    cam.skip(8)
    while True:
        frame = cam.snapshot()
        if not frame.is_valid():
            continue
        vis = frame.to_format('yuv420sp')
        rtsp.display(vis)
finally:
    rtsp.stop()
    cam.release()
```

#### 案例 8-3：采集 + PPOCR + 结果叠字

OCR 结果直接叠加在画面上。

```
import visiong
cam = visiong.Camera(640, 360, format='rgb')
udp = visiong.DisplayUDP()
ocr = visiong.PPOCR('/path/to/det.rknn', '/path/to/rec.rknn', '/path/to/keys.txt', model_input_format='rgb')
try:
    cam.skip(8)
    while True:
        img = cam.snapshot()
        if not img.is_valid():
            continue
        vis = img.to_format('bgr888')
        for item in ocr.infer(img):
            x, y, w, h = item.rect
            vis.draw_rectangle(x, y, w, h, color=(0, 255, 255), thickness=2)
            vis.draw_string(x, max(0, y - 20), item.text, color=(0, 255, 255), scale=1.0, thickness=2)
        udp.display(vis)
finally:
    udp.release()
    cam.release()
```

#### 案例 8-4：采集 + NanoTrack + 丢失重置

跟踪低分时自动 reset 的基础逻辑。

```
import visiong
cam = visiong.Camera(640, 360, format='rgb')
udp = visiong.DisplayUDP()
tracker = visiong.NanoTrack('/path/to/T.rknn', '/path/to/X.rknn', '/path/to/H.rknn')
try:
    cam.skip(8)
    first = cam.snapshot()
    tracker.init(first, (220, 120, 90, 90))
    while True:
        frame = cam.snapshot()
        if not frame.is_valid():
            continue
        tr = tracker.track(frame)
        if tr.score < 0.4:
            tracker.reset()
            tracker.init(frame, (220, 120, 90, 90))
        vis = frame.to_format('bgr888')
        x, y, w, h = tr.box
        vis.draw_rectangle(x, y, w, h, color=(0, 255, 0), thickness=2)
        udp.display(vis)
finally:
    udp.release()
    cam.release()
```

#### 案例 8-5：采集 + LowLevelNPU + 结果日志

底层推理循环模板。

```
import visiong
cam = visiong.Camera(640, 360, format='rgb')
ll = visiong.LowLevelNPU('/path/to/model.rknn')
try:
    cam.skip(8)
    while True:
        img = cam.snapshot()
        if not img.is_valid():
            continue
        ll.set_input_image(0, img, color_order='rgb', keep_aspect=True, pad_value=114, driver_convert=True)
        ll.run(sync_outputs=True)
        out = ll.output_float(0)
        print('out len =', len(out))
finally:
    cam.release()
```

#### 案例 8-6：HTTP 直播 + 定时截图

一边网页直播，一边周期保存样本图。

```
import time
import visiong
cam = visiong.Camera(640, 360, format='bgr')
http = visiong.DisplayHTTP(port=8080, mode='jpg', quality=75)
last_save = 0.0
idx = 0
try:
    cam.skip(8)
    while True:
        frame = cam.snapshot()
        if not frame.is_valid():
            continue
        http.display(frame)
        now = time.time()
        if now - last_save > 5:
            frame.save(f'snap_{idx:04d}.jpg', quality=90)
            idx += 1
            last_save = now
finally:
    http.stop()
    cam.release()
```

#### 案例 8-7：离线数据集预处理 + 推理

批量 load/letterbox/infer 的模板。

```
import visiong
npu = visiong.NPU('yolov5', '/path/to/model.rknn', '/path/to/labels.txt')
images = ['/path/to/a.jpg', '/path/to/b.jpg']
for p in images:
    img = visiong.ImageBuffer.load(p).to_format('rgb888')
    inp = img.letterbox(640, 640, color=(114, 114, 114))
    dets = npu.infer(inp, model_format='rgb')
    print(p, 'det=', len(dets))
```

#### 案例 8-8：GPIO 触发抓图

读取输入脚状态触发 snapshot 保存。

```
import time
import visiong
pm = visiong.PinMux()
pid = pm.parse_pin('GPIO0_C4')
cfg = visiong.GpioLineConfig()
cfg.direction = 'in'
cam = visiong.Camera(640, 360, format='bgr')
try:
    pm.gpio_request_line(pid.bank, pid.pin, cfg)
    cam.skip(8)
    idx = 0
    while True:
        if pm.gpio_get_value(pid.bank, pid.pin) == 1:
            img = cam.snapshot()
            if img.is_valid():
                img.save(f'trigger_{idx:03d}.jpg', quality=90)
                idx += 1
            time.sleep(0.2)
finally:
    pm.gpio_release_line(pid.bank, pid.pin)
    pm.close()
    cam.release()
```

#### 案例 8-9：NPU 时钟调优 + 基准测试

简单比较不同时钟下的推理吞吐。

```
import time
import visiong
clk = visiong.NpuClock()
npu = visiong.NPU('yolov5', '/path/to/model.rknn', '/path/to/labels.txt')
img = visiong.ImageBuffer.load('/path/to/bench.jpg').to_format('rgb888')
for mhz in [400, 500]:
    ret = clk.set_rate_mhz(mhz)
    t0 = time.time()
    for _ in range(20):
        npu.infer(img, model_format='rgb')
    dt = time.time() - t0
    print(mhz, 'MHz', 'ok=', ret.ok, 'fps=', 20 / max(1e-6, dt))
```

#### 案例 8-10：GUI 相机应用骨架

预览+按钮的最小应用框架。

```
import visiong
W, H = 320, 240
cam = visiong.Camera(W, H, format='bgr')
fb = visiong.DisplayFB()
touch = visiong.Touch()
gui = visiong.GUI(W, H)
try:
    cam.skip(8)
    while True:
        frame = cam.snapshot()
        if not frame.is_valid():
            continue
        gui.begin_frame(touch)
        if gui.begin_window('App', 8, 8, 304, 90):
            gui.layout_row_dynamic(28, 2)
            if gui.button('Snap'):
                frame.save('gui_snap.jpg', quality=90)
            gui.button('Exit')
        gui.end_window()
        gui.end_frame(frame)
        fb.display(frame, (0, 0, frame.width, frame.height))
finally:
    fb.release()
    touch.release()
    cam.release()
```

## 6) 按类最小片段

每个类给一个最小调用片段，适合先复制再改参数。

Camera · 最小调用片段

```
import visiong
cam = visiong.Camera(target_width=640, target_height=360)
result = cam.init(target_width=640, target_height=360)
result = cam.skip(10)
result = cam.snapshot()
```

IVEModel · 最小调用片段

```
import visiong
model = visiong.IVEModel(width=640, height=360)
# IVEModel 暂无公开方法
```

MotionVector · 最小调用片段

```
import visiong
mv = visiong.MotionVector()
# MotionVector 暂无公开方法
```

IVE · 最小调用片段

```
import visiong
ive = visiong.IVE()
visiong.IVE.set_log_enabled(enabled=True)
result = visiong.IVE.is_log_enabled()
result = ive.filter(src=img, mask=[[0, -1, 0], [-1, 5, -1], [0, -1, 0]])
```

Blob · 最小调用片段

```
import visiong
blob = visiong.Blob(x=10, y=10, w=120, h=90, cx=160, cy=120, pixels=0)
# Blob 暂无公开方法
```

Line · 最小调用片段

```
import visiong
line = visiong.Line()
# Line 暂无公开方法
```

Circle · 最小调用片段

```
import visiong
circle = visiong.Circle()
# Circle 暂无公开方法
```

QRCode · 最小调用片段

```
import visiong
qRCode = visiong.QRCode()
# QRCode 暂无公开方法
```

ImageBuffer · 最小调用片段

```
import visiong
img = visiong.ImageBuffer()
result = visiong.ImageBuffer.create(width=640, height=360, format='bgr888')
result = img.to_numpy(copy=True)
result = img.numpy_view()
```

DisplayUDP · 最小调用片段

```
import visiong
udp = visiong.DisplayUDP()
result = udp.init(ip_address='127.0.0.1', port=8080)
udp.display(img_buf=img)
udp.release()
```

TouchPoint · 最小调用片段

```
import visiong
touchPoint = visiong.TouchPoint()
# TouchPoint 暂无公开方法
```

Touch · 最小调用片段

```
import visiong
touch = visiong.Touch()
touch.release()
result = touch.is_pressed()
result = touch.read()
```

DisplayFB · 最小调用片段

```
import visiong
fb = visiong.DisplayFB()
fb.display(img_buf=img, roi=(0, 0, 320, 240))
fb.release()
result = fb.is_initialized()
```

Detection · 最小调用片段

```
import visiong
detection = visiong.Detection()
# Detection 暂无公开方法
```

NPU · 最小调用片段

```
import visiong
npu = visiong.NPU(model_type='yolov5', model_path='/path/to/model.rknn')
result = npu.infer(img_buf=img)
result = npu.get_face_feature(face_image=img)
result = npu.recognize_plate(plate_image=img)
```

LowLevelTensorInfo · 最小调用片段

```
import visiong
lowLevelTensorInfo = visiong.LowLevelTensorInfo()
# LowLevelTensorInfo 暂无公开方法
```

LowLevelNPU · 最小调用片段

```
import visiong
llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.is_initialized()
result = llnpu.num_inputs()
result = llnpu.num_outputs()
```

OCRResult · 最小调用片段

```
import visiong
oCRResult = visiong.OCRResult()
# OCRResult 暂无公开方法
```

PPOCR · 最小调用片段

```
import visiong
ocr = visiong.PPOCR(det_model_path='/path/to/det.rknn', rec_model_path='/path/to/rec.rknn')
result = ocr.infer(img_buf=img)
result = ocr.is_initialized()
```

NanoTrackResult · 最小调用片段

```
import visiong
nanoTrackResult = visiong.NanoTrackResult()
# NanoTrackResult 暂无公开方法
```

NanoTrack · 最小调用片段

```
import visiong
tracker = visiong.NanoTrack(template_model=0, search_model=0, head_model='/path/to/head.rknn')
result = tracker.init(img_buf=img, bbox=(60, 40, 120, 90))
result = tracker.track(img_buf=img)
result = tracker.is_initialized()
```

Canvas · 最小调用片段

```
import visiong
canvas = visiong.Canvas()
# Canvas 暂无公开方法
```

GUI · 最小调用片段

```
import visiong
gui = visiong.GUI(width=640, height=360)
result = gui.begin_frame(touch_device=None)
result = gui.begin_window(title='Demo', x=10, y=10, w=120, h=90)
result = gui.end_window()
```

PinId · 最小调用片段

```
import visiong
pinId = visiong.PinId()
# PinId 暂无公开方法
```

PinMuxRegisterInfo · 最小调用片段

```
import visiong
pinMuxRegisterInfo = visiong.PinMuxRegisterInfo()
# PinMuxRegisterInfo 暂无公开方法
```

PinAltFunction · 最小调用片段

```
import visiong
pinAltFunction = visiong.PinAltFunction()
# PinAltFunction 暂无公开方法
```

PinRuntimeStatus · 最小调用片段

```
import visiong
pinRuntimeStatus = visiong.PinRuntimeStatus()
# PinRuntimeStatus 暂无公开方法
```

PinConflictReport · 最小调用片段

```
import visiong
pinConflictReport = visiong.PinConflictReport()
# PinConflictReport 暂无公开方法
```

FunctionInterfaceStatus · 最小调用片段

```
import visiong
functionInterfaceStatus = visiong.FunctionInterfaceStatus()
# FunctionInterfaceStatus 暂无公开方法
```

AdcChannelStatus · 最小调用片段

```
import visiong
adcChannelStatus = visiong.AdcChannelStatus()
# AdcChannelStatus 暂无公开方法
```

GpioLineConfig · 最小调用片段

```
import visiong
gpioLineConfig = visiong.GpioLineConfig()
# GpioLineConfig 暂无公开方法
```

GpioLineStatus · 最小调用片段

```
import visiong
gpioLineStatus = visiong.GpioLineStatus()
# GpioLineStatus 暂无公开方法
```

DriveStrengthStatus · 最小调用片段

```
import visiong
driveStrengthStatus = visiong.DriveStrengthStatus()
# DriveStrengthStatus 暂无公开方法
```

PullStatus · 最小调用片段

```
import visiong
pullStatus = visiong.PullStatus()
# PullStatus 暂无公开方法
```

SchmittStatus · 最小调用片段

```
import visiong
schmittStatus = visiong.SchmittStatus()
# SchmittStatus 暂无公开方法
```

PinMux · 最小调用片段

```
import visiong
pm = visiong.PinMux()
result = pm.is_open()
pm.close()
result = pm.parse_pin(pin_name='GPIO0_C3')
```

NpuClockStatus · 最小调用片段

```
import visiong
npuClockStatus = visiong.NpuClockStatus()
# NpuClockStatus 暂无公开方法
```

NpuClockApplyResult · 最小调用片段

```
import visiong
npuClockApplyResult = visiong.NpuClockApplyResult()
# NpuClockApplyResult 暂无公开方法
```

NpuClock · 最小调用片段

```
import visiong
npu_clock = visiong.NpuClock()
result = npu_clock.status()
result = npu_clock.supported_rates_hz()
result = npu_clock.supported_rates_mhz()
```

DisplayHTTP · 最小调用片段

```
import visiong
http = visiong.DisplayHTTP()
http.stop()
http.display(img=img)
result = http.is_running()
```

DisplayRTSP · 最小调用片段

```
import visiong
rtsp = visiong.DisplayRTSP()
rtsp.stop()
rtsp.set_fps(fps=30)
result = rtsp.get_fps()
```

VencRecorder · 最小调用片段

```
import visiong
rec = visiong.VencRecorder(filepath='/path/to/demo.jpg')
rec.write(img=img)
rec.close()
result = rec.is_open()
```

DisplayHTTPFLV · 最小调用片段

```
import visiong
flv = visiong.DisplayHTTPFLV()
flv.stop()
flv.display(img=img)
result = flv.is_running()
```

## 7) 逐函数示例库（自动生成）

覆盖全部模块函数、类方法和字段。每一项都提供最小可复制代码片段，便于学习者直接改参数后运行。

**自动示例总数：**453

模块级函数 7 snippets

##### `visiong.close_venc_recorder``(filepath)`

Closes a cached MP4 writer for filepath (finalize MP4).

```
import visiong

visiong.close_venc_recorder('demo.mp4')
```

##### `visiong.close_all_venc_recorders``()`

Closes all cached MP4 writers (finalize MP4).

```
import visiong

visiong.close_all_venc_recorders()
```

##### `visiong.get_unique_id``()`

Reads the unique 6-byte chip ID from the RV1106 OTP area and returns it as a 12-character hex string.

```
import visiong

uid = visiong.get_unique_id()
```

##### `visiong.decrypt_legacy_value``(key)`

Decrypts the stored legacy value with a 64-character hex key.

```
import visiong

key = 'your-secret'

value = visiong.decrypt_legacy_value(key)
```

##### `visiong.dma_state_metrics``(reset=...)`

Returns dma-buf state-machine counters as a dict. Set reset=True to clear counters after reading.

```
import visiong

metrics = visiong.dma_state_metrics(reset=False)
```

##### `visiong.dma_state_reset_metrics``()`

Resets dma-buf state-machine counters.

```
import visiong

visiong.dma_state_reset_metrics()
```

##### `visiong.dma_state_dump_metrics``(output_path=..., reset_after_dump=...)`

Returns dma-buf state-machine counters as JSON and optionally writes them to output\_path.

```
import visiong

dump = visiong.dma_state_dump_metrics(output_path='metrics.json')
```

Camera 38 methods · 0 fields

##### `Camera.init``(target_width, target_height, format=..., hdr=..., crop_mode=...)`

Initializes the camera. format defaults to 'yuv'. Supported values: 'bgr', 'rgb', 'yuv'/'yuv420', or 'gray'. crop\_mode: 'auto' (default, follows the camera max-resolution aspect ratio), 'off', or any ratio such as '16:9', '4:3', '1:1', o...

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.init(target_width=640, target_height=360)
```

##### `Camera.skip``(num_frames=...)`

(Reads and discards a specified number of frames from the camera.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.skip(10)
```

##### `Camera.snapshot``()`

Captures a single frame from the camera and returns an ImageBuffer.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.snapshot()
```

##### `Camera.release``()`

Releases the camera and frees resources. Safe to call even if not initialized.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.release()
```

##### `Camera.is_initialized``()`

Returns True if the camera has been successfully initialized.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.is_initialized()
```

##### `Camera.get_capture_width``()`

Returns the actual capture width (alias for actual\_width).

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.get_capture_width()
```

##### `Camera.get_capture_height``()`

Returns the actual capture height (alias for actual\_height).

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.get_capture_height()
```

##### `Camera.set_saturation``(value)`

Sets the image saturation. Range: \[0, 255\]. Raises ValueError on invalid input.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_saturation(value=0)
```

##### `Camera.set_contrast``(value)`

Sets the image contrast. Range: \[0, 255\]. Raises ValueError on invalid input.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_contrast(value=0)
```

##### `Camera.set_brightness``(value)`

Sets the image brightness. Range: \[0, 255\]. Raises ValueError on invalid input.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_brightness(value=0)
```

##### `Camera.set_sharpness``(value)`

Sets the image sharpness. Range: \[0, 100\]. Raises ValueError on invalid input.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_sharpness(value=0)
```

##### `Camera.set_hue``(value)`

Sets the image hue. Range: \[0, 255\]. Raises ValueError on invalid input.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_hue(value=0)
```

##### `Camera.set_white_balance_mode``(mode)`

Sets white balance mode ('auto' or 'manual'). Raises ValueError on invalid mode.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_white_balance_mode(mode='auto')
```

##### `Camera.set_white_balance_temperature``(temp)`

Sets white balance color temperature in manual mode. Raises ValueError on invalid input or RuntimeError if not in manual mode.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_white_balance_temperature(temp=4500)
```

##### `Camera.set_exposure_mode``(mode)`

Sets exposure mode ('auto' or 'manual'). Raises ValueError on invalid mode.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_exposure_mode(mode='auto')
```

##### `Camera.set_exposure_time``(time_in_seconds)`

Sets manual exposure time in seconds. Must be positive. Raises RuntimeError if not in manual mode.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_exposure_time(time_in_seconds=0.01)
```

##### `Camera.set_exposure_gain``(gain)`

Sets manual exposure gain. Typical range: \[0, 127\]. Raises ValueError on invalid input or RuntimeError if not in manual mode.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_exposure_gain(gain=32)
```

##### `Camera.set_spatial_denoise_level``(level)`

Sets the spatial (2D) denoise level. Range: \[0, 100\]. Raises ValueError on invalid input.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_spatial_denoise_level(level=3)
```

##### `Camera.set_temporal_denoise_level``(level)`

Sets the temporal (3D) denoise level. Range: \[0, 100\]. Raises ValueError on invalid input.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_temporal_denoise_level(level=3)
```

##### `Camera.set_frame_rate``(fps)`

Sets the camera frame rate. Range: \[10, 60\] (or 0 for auto). Raises ValueError on invalid input.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_frame_rate(fps=30)
```

##### `Camera.set_power_line_frequency``(mode)`

Sets anti-flicker mode ('50hz', '60hz', or 'off'). Raises ValueError on invalid mode.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_power_line_frequency(mode='auto')
```

##### `Camera.set_flip``(flip, mirror)`

Sets image flip (vertical) and mirror (horizontal).

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_flip(flip=True, mirror=True)
```

##### `Camera.get_saturation``()`

Gets the current image saturation (0-255).

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.get_saturation()
```

##### `Camera.get_contrast``()`

Gets the current image contrast (0-255).

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.get_contrast()
```

##### `Camera.get_brightness``()`

Gets the current image brightness (0-255).

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.get_brightness()
```

##### `Camera.get_sharpness``()`

Gets the current image sharpness (0-100).

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.get_sharpness()
```

##### `Camera.get_hue``()`

Gets the current image hue (0-255).

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.get_hue()
```

##### `Camera.get_white_balance_mode``()`

Gets the current white balance mode ('auto' or 'manual').

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.get_white_balance_mode()
```

##### `Camera.get_white_balance_temperature``()`

Gets the current white balance color temperature.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.get_white_balance_temperature()
```

##### `Camera.get_exposure_mode``()`

Gets the current exposure mode ('auto' or 'manual').

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.get_exposure_mode()
```

##### `Camera.get_exposure_time``()`

Gets the current exposure time in seconds.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.get_exposure_time()
```

##### `Camera.get_exposure_gain``()`

Gets the current exposure gain.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.get_exposure_gain()
```

##### `Camera.lock_focus``()`

Locks the autofocus at its current position. Prevents further automatic adjustments.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.lock_focus()
```

##### `Camera.unlock_focus``()`

Unlocks the autofocus, allowing it to resume its configured mode (e.g., continuous focus).

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.unlock_focus()
```

##### `Camera.trigger_focus``()`

Performs a single, one-shot autofocus search. This is an action, not a mode.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.trigger_focus()
```

##### `Camera.set_focus_mode``(mode)`

Sets the autofocus mode. Supported modes:

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_focus_mode(mode='continuous')
```

##### `Camera.set_manual_focus``(position)`

Moves the lens to a specific motor code position. This implicitly sets the mode to 'manual'.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
cam.set_manual_focus(position=512)
```

##### `Camera.get_focus_position``()`

Returns the current motor code position of the lens. Returns -1 on failure.

```
import visiong

cam = visiong.Camera(target_width=640, target_height=360)
result = cam.get_focus_position()
```

IVEModel 0 methods · 0 fields

该类没有公开方法示例。

MotionVector 1 methods · 3 fields

##### `MotionVector.__repr__``()`

<MotionVector status=

```
import visiong

mv = visiong.MotionVector()
text = repr(mv)
```

##### `MotionVector.status` · field

readonly\_field

```
import visiong

mv = visiong.MotionVector()
value = mv.status
```

##### `MotionVector.mv_x` · field

readonly\_field

```
import visiong

mv = visiong.MotionVector()
value = mv.mv_x
```

##### `MotionVector.mv_y` · field

readonly\_field

```
import visiong

mv = visiong.MotionVector()
value = mv.mv_y
```

IVE 37 methods · 0 fields

##### `IVE.set_log_enabled``(enabled)` · static

Enable or disable IVE internal logs (default off; env VISIONG\_IVE\_LOG=1 enables).

```
import visiong

visiong.IVE.set_log_enabled(enabled=True)
```

##### `IVE.is_log_enabled``()` · static

Returns True if IVE internal logs are enabled.

```
import visiong

result = visiong.IVE.is_log_enabled()
```

##### `IVE.filter``(src, mask)`

5x5 filter. src: GRAY8. mask: 25 ints. Returns GRAY8 - do not pass to cast\_16bit\_to\_8bit. Use a 5x5 Gaussian mask for blur (no separate gaussian\_filter).

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.filter(src=img, mask=[[0, -1, 0], [-1, 5, -1], [0, -1, 0]])
```

##### `IVE.sobel``(src, out_ctrl=..., out_format=...)`

Sobel edge detection. src: GRAY8. Returns (horizontal\_edges, vertical\_edges). When out\_format=S16C1 each result is S16C1 - unpack and pass to cast\_16bit\_to\_8bit for display; when U8C1 each result is GRAY8.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.sobel(src=img)
```

##### `IVE.canny``(src, high_thresh, low_thresh)`

Canny edge detection. src must be GRAY8. high\_thresh and low\_thresh typically in 0-255.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.canny(src=img, high_thresh=120, low_thresh=40)
```

##### `IVE.mag_and_ang``(src, threshold=..., return_magnitude=...)`

Gradient magnitude/angle. return\_magnitude=True -> S16C1 (use cast\_16bit\_to\_8bit for display); False -> U8C1.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.mag_and_ang(src=img)
```

##### `IVE.dilate``(src, kernel_size=...)`

Performs morphological dilation. \`src\` must be GRAY8. \`kernel\_size\` can be 3 or 5.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.dilate(src=img)
```

##### `IVE.erode``(src, kernel_size=...)`

Performs morphological erosion. \`src\` must be GRAY8. \`kernel\_size\` can be 3 or 5.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.erode(src=img)
```

##### `IVE.ordered_stat_filter``(src, mode)`

Performs an ordered statistic filter (Median, Max, Min). \`src\` must be GRAY8.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.ordered_stat_filter(src=img, mode=visiong.OrdStatFilterMode.MEDIAN)
```

##### `IVE.add``(src1, src2)`

Pixel-wise addition. Returns GRAY8; do not pass to cast\_16bit\_to\_8bit.

```
import visiong

ive = visiong.IVE()
img1 = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
img2 = visiong.ImageBuffer.create(320, 240, "bgr888", (255, 255, 255))
result = ive.add(src1=img1, src2=img2)
```

##### `IVE.sub``(src1, src2, mode=...)`

Pixel-wise subtraction. Returns GRAY8; do not pass to cast\_16bit\_to\_8bit. Mode: ABS or SHIFT.

```
import visiong

ive = visiong.IVE()
img1 = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
img2 = visiong.ImageBuffer.create(320, 240, "bgr888", (255, 255, 255))
result = ive.sub(src1=img1, src2=img2)
```

##### `IVE.logic_op``(src1, src2, op)`

Performs a pixel-wise logical operation (AND, OR, XOR) on two images.

```
import visiong

ive = visiong.IVE()
img1 = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
img2 = visiong.ImageBuffer.create(320, 240, "bgr888", (255, 255, 255))
result = ive.logic_op(src1=img1, src2=img2, op=visiong.LogicOp.AND)
```

##### `IVE.threshold``(src)`

Thresholding on GRAY8. kwargs accept low/low\_thresh, high/high\_thresh, mode. Returns GRAY8; do not pass to cast\_16bit\_to\_8bit.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.threshold(src=img)
```

##### `IVE.cast_16bit_to_8bit``(src, mode)`

Casts 16-bit (S16C1/U16C1) to 8-bit. Input must be from sobel(S16C1), mag\_and\_ang(magnitude), norm\_grad, or sad (first element); not from filter, add, sub, threshold, gmm.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.cast_16bit_to_8bit(src=img, mode=visiong.Cast16to8Mode.S16_TO_U8_ABS)
```

##### `IVE.hist``(src)`

Calculates the histogram of a GRAY8 image. Returns a list of 256 integers.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.hist(src=img)
```

##### `IVE.equalize_hist``(src)`

Performs histogram equalization on a GRAY8 image.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.equalize_hist(src=img)
```

##### `IVE.integral``(src, mode=...)`

Integral image. mode: COMBINE, SUM, or SQSUM. Output U64C1.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.integral(src=img)
```

##### `IVE.ccl``(src, min_area=...)`

Connected Components Labeling on a binarized GRAY8 image. Blobs with area < min\_area are filtered. Returns list of Blob.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.ccl(src=img)
```

##### `IVE.ncc``(src1, src2)`

Normalized cross-correlation. If sizes differ, set auto\_resize=True (default) to resize src2 to src1 size.

```
import visiong

ive = visiong.IVE()
img1 = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
img2 = visiong.ImageBuffer.create(320, 240, "bgr888", (255, 255, 255))
result = ive.ncc(src1=img1, src2=img2)
```

##### `IVE.csc``(src, mode)`

Performs Color Space Conversion (e.g., YUV to RGB).

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.csc(src=img, mode=visiong.CscMode.YUV2RGB_BT601_LIMITED)
```

##### `IVE.yuv_to_rgb``(src)`

Converts YUV420SP (or YUV420SP\_VU) to RGB. Use full\_range=True/False, or pass mode=visiong.CscMode.YUV2RGB\_\*.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.yuv_to_rgb(src=img)
```

##### `IVE.yuv_to_hsv``(src)`

Converts YUV420SP (or YUV420SP\_VU) to HSV. Use full\_range=True/False, or pass mode=visiong.CscMode.YUV2HSV\_\*.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.yuv_to_hsv(src=img)
```

##### `IVE.rgb_to_yuv``(src, full_range=...)`

Converts RGB/BGR image to YUV420SP.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.rgb_to_yuv(src=img)
```

##### `IVE.rgb_to_hsv``(src, full_range=...)`

Converts RGB/BGR image to HSV. H:\[0,180\], S:\[0,255\], V:\[0,255\].

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.rgb_to_hsv(src=img)
```

##### `IVE.dma``(src, mode=...)`

Performs a direct memory access operation (e.g., copy).

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.dma(src=img)
```

##### `IVE.cast_8bit_to_8bit``(src, bias, numerator, denominator)`

Scales an 8-bit image using the formula: dst = (src \* numerator / denominator) + bias.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.cast_8bit_to_8bit(src=img, bias=0, numerator=1, denominator=1)
```

##### `IVE.map``(src, lut)`

Pixel LUT mapping. lut: list of 256 integers in \[0,255\]; output\[x\] = lut\[src\[x\]\].

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
lut = [i for i in range(256)]
result = ive.map(src=img, lut=lut)
```

##### `IVE.gmm``(src, model, first_frame=...)`

GMM background subtraction. Returns (foreground, background); both GRAY8 - do not pass to cast\_16bit\_to\_8bit.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
model = visiong.IVEModel(640, 360, 0)
result = ive.gmm(src=img, model=model)
```

##### `IVE.gmm2``(src, factor, model, first_frame=...)`

GMM2 with factor image. Returns (foreground, background); both GRAY8.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
model = visiong.IVEModel(640, 360, 0)
result = ive.gmm2(src=img, factor=0.5, model=model)
```

##### `IVE.lbp``(src, abs_mode=..., threshold=...)`

Calculates the Local Binary Pattern of an image.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.lbp(src=img)
```

##### `IVE.norm_grad``(src)`

Normalized gradient. Returns (horizontal\_grad, vertical\_grad); both S16C1 - use cast\_16bit\_to\_8bit for display.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.norm_grad(src=img)
```

##### `IVE.lk_optical_flow``(prev_img, next_img, points)`

Performs Lucas-Kanade optical flow. points: list of (x,y). Returns list of MotionVector; mv\_x/mv\_y are S9.7 (divide by 128 for pixels). Up to ~500 points.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
points = [(120, 80), (180, 120)]
result = ive.lk_optical_flow(prev_img=img, next_img=img, points=points)
```

##### `IVE.st_corner``(src, max_corners=..., min_dist=..., quality_level=...)`

Performs Shi-Tomasi corner detection. Returns a list of (x, y) corner tuples.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.st_corner(src=img)
```

##### `IVE.match_bg_model``(current_img, bg_model, frame_num)`

Matches the current image against a background model. Returns a foreground flag image.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
img2 = visiong.ImageBuffer.create(320, 240, "bgr888", (255, 255, 255))
result = ive.match_bg_model(current_img=img, bg_model=img2, frame_num=img)
```

##### `IVE.update_bg_model``(current_img, fg_flag, bg_model, frame_num)`

Updates the background model. Returns the new background image.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
img2 = visiong.ImageBuffer.create(320, 240, "bgr888", (255, 255, 255))
result = ive.update_bg_model(current_img=img, fg_flag=img2, bg_model=img2, frame_num=img)
```

##### `IVE.sad``(src1, src2, mode, threshold, min_val=..., max_val=...)`

Sum of Absolute Differences. Returns (sad\_image U16C1, threshold\_image U8C1); cast sad\_image for 8-bit display.

```
import visiong

ive = visiong.IVE()
img1 = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
img2 = visiong.ImageBuffer.create(320, 240, "bgr888", (255, 255, 255))
result = ive.sad(src1=img1, src2=img2, mode=visiong.SadMode.MB_8X8, threshold=120)
```

##### `IVE.create_pyramid``(src, levels)`

Builds an image pyramid with levels levels. Returns list of ImageBuffer from full size down.

```
import visiong

ive = visiong.IVE()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ive.create_pyramid(src=img, levels=3)
```

Blob 1 methods · 0 fields

##### `Blob.__repr__``()`

<Blob rect=(

```
import visiong

blob = visiong.Blob(x=10, y=10, w=120, h=90, cx=160, cy=120, pixels=0)
text = repr(blob)
```

Line 1 methods · 7 fields

##### `Line.__repr__``()`

类方法最小示例

```
import visiong

line = visiong.Line()
text = repr(line)
```

##### `Line.x1` · field

readonly\_field

```
import visiong

line = visiong.Line()
value = line.x1
```

##### `Line.y1` · field

readonly\_field

```
import visiong

line = visiong.Line()
value = line.y1
```

##### `Line.x2` · field

readonly\_field

```
import visiong

line = visiong.Line()
value = line.x2
```

##### `Line.y2` · field

readonly\_field

```
import visiong

line = visiong.Line()
value = line.y2
```

##### `Line.magnitude` · field

readonly\_field

```
import visiong

line = visiong.Line()
value = line.magnitude
```

##### `Line.theta` · field

readonly\_field

```
import visiong

line = visiong.Line()
value = line.theta
```

##### `Line.rho` · field

readonly\_field

```
import visiong

line = visiong.Line()
value = line.rho
```

Circle 1 methods · 4 fields

##### `Circle.__repr__``()`

<Circle center=(

```
import visiong

circle = visiong.Circle()
text = repr(circle)
```

##### `Circle.cx` · field

readonly\_field

```
import visiong

circle = visiong.Circle()
value = circle.cx
```

##### `Circle.cy` · field

readonly\_field

```
import visiong

circle = visiong.Circle()
value = circle.cy
```

##### `Circle.r` · field

readonly\_field

```
import visiong

circle = visiong.Circle()
value = circle.r
```

##### `Circle.magnitude` · field

readonly\_field

```
import visiong

circle = visiong.Circle()
value = circle.magnitude
```

QRCode 1 methods · 2 fields

##### `QRCode.__repr__``()`

from\_numpy\_zero\_copy: width and height must both be even for zero-copy hardware-friendly ImageBuffer wrapping.

```
import visiong

qRCode = visiong.QRCode()
text = repr(qRCode)
```

##### `QRCode.corners` · field

property\_readonly

```
import visiong

qRCode = visiong.QRCode()
value = qRCode.corners
```

##### `QRCode.payload` · field

property\_readonly

```
import visiong

qRCode = visiong.QRCode()
value = qRCode.payload
```

ImageBuffer 39 methods · 0 fields

##### `ImageBuffer.create``(width, height, format, color=...)` · static

Creates a new ImageBuffer filled with the given color. format: e.g. 'rgb888', 'bgr888', 'gray8'. color: (R,G,B) or (R,G,B,A).

```
import visiong

result = visiong.ImageBuffer.create(width=640, height=360, format='bgr888')
```

##### `ImageBuffer.__array__``(copy=..., dtype=...)`

Allows NumPy conversion via np.asarray(img). copy=False returns a read-only array pinned to the ImageBuffer lifetime; color exports use a BGR-shaped view over the current CPU-readable storage, which may be a cached converted backing stor...

```
import visiong
import numpy as np

img = visiong.ImageBuffer()
arr = np.asarray(img)
```

##### `ImageBuffer.to_numpy``(copy=...)`

Returns a NumPy array: 2D (H,W) for grayscale, 3D (H,W,3) BGR-shaped for color. copy=False returns a read-only lifetime-pinned view over the current CPU-readable storage.

```
import visiong

img = visiong.ImageBuffer()
result = img.to_numpy(copy=True)
```

##### `ImageBuffer.numpy_view``()`

Returns a read-only lifetime-pinned NumPy view over the current CPU-readable storage.

```
import visiong

img = visiong.ImageBuffer()
result = img.numpy_view()
```

##### `ImageBuffer.from_numpy``(array, format=..., copy=...)` · static

Creates an ImageBuffer from a NumPy array. format='auto' infers: 2D or 1-channel -> GRAY8, 3-channel -> BGR888, 4-channel -> BGRA8888. copy=False uses zero-copy when the input is uint8, C-contiguous, and has even width/height.

```
import visiong
import numpy as np

arr = np.zeros((1, 224, 224, 3), dtype=np.uint8)
result = visiong.ImageBuffer.from_numpy(array=arr)
```

##### `ImageBuffer.from_numpy_zero_copy``(array, format=...)` · static

Strict zero-copy import from a NumPy array. Requires dtype=uint8, C-contiguous layout, and even width/height; otherwise raises instead of silently copying.

```
import visiong
import numpy as np

arr = np.zeros((1, 224, 224, 3), dtype=np.uint8)
result = visiong.ImageBuffer.from_numpy_zero_copy(array=arr)
```

##### `ImageBuffer.is_valid``()`

Returns True if the buffer holds valid image data.

```
import visiong

img = visiong.ImageBuffer()
result = img.is_valid()
```

##### `ImageBuffer.copy``()`

Creates and returns a deep copy of the image buffer.

```
import visiong

img = visiong.ImageBuffer()
result = img.copy()
```

##### `ImageBuffer.load``(filepath)` · static

Loads an image from a file into a new ImageBuffer. Supports JPEG, PNG, and BMP (via stb\_image).

```
import visiong

result = visiong.ImageBuffer.load(filepath='/path/to/demo.jpg')
```

##### `ImageBuffer.save``(filepath, quality=...)`

Saves the ImageBuffer to a file. Supports JPEG, PNG, and BMP. Quality (1-100) applies to JPEG/PNG. Uses software encoding (stb\_image).

```
import visiong

img = visiong.ImageBuffer()
result = img.save(filepath='/path/to/demo.jpg')
```

##### `ImageBuffer.save_hsv_bin``(filepath)`

Converts YUV420SP image to HSV using IVE hardware and saves raw HSV binary data to the given filepath. Input must be YUV420SP or YUV420SP\_VU.

```
import visiong

img = visiong.ImageBuffer()
result = img.save_hsv_bin(filepath='/path/to/demo.jpg')
```

##### `ImageBuffer.save_venc_jpg``(filepath, quality=...)`

it checks if size and format match; otherwise, it auto-initializes the encoder.

```
import visiong

img = visiong.ImageBuffer()
result = img.save_venc_jpg(filepath='/path/to/demo.jpg')
```

##### `ImageBuffer.save_venc_h264``(filepath, quality=..., rc_mode=..., fps=..., append=..., container=..., mp4_faststart=...)`

it muxes into MP4 and caches the writer until close\_venc\_recorder/close\_all\_venc\_recorders or process exit.

```
import visiong

img = visiong.ImageBuffer()
result = img.save_venc_h264(filepath='/path/to/demo.jpg')
```

##### `ImageBuffer.save_venc_h265``(filepath, quality=..., rc_mode=..., fps=..., append=..., container=..., mp4_faststart=...)`

it muxes into MP4 and caches the writer until close\_venc\_recorder/close\_all\_venc\_recorders or process exit.

```
import visiong

img = visiong.ImageBuffer()
result = img.save_venc_h265(filepath='/path/to/demo.jpg')
```

##### `ImageBuffer.to_format``(new_format)`

Converts the image to the given pixel format string (e.g. 'rgb888', 'bgr888', 'gray8', 'yuv420sp').

```
import visiong

img = visiong.ImageBuffer()
result = img.to_format(new_format='rgb')
```

##### `ImageBuffer.to_grayscale``()`

Converts the image to GRAY8. Uses RGA for color images.

```
import visiong

img = visiong.ImageBuffer()
result = img.to_grayscale()
```

##### `ImageBuffer.resize``(new_width, new_height)`

Resizes the image to new\_width x new\_height using RGA.

```
import visiong

img = visiong.ImageBuffer()
result = img.resize(new_width=320, new_height=240)
```

##### `ImageBuffer.crop``(x, y, w, h)`

Crops to the region (x, y, w, h). rect\_tuple: (x, y, w, h). Uses RGA.

```
import visiong

img = visiong.ImageBuffer()
result = img.crop(x=10, y=10, w=120, h=90)
```

##### `ImageBuffer.rotate``(angle_degrees)`

Rotates the image by 90, 180, or 270 degrees using hardware acceleration.

```
import visiong

img = visiong.ImageBuffer()
result = img.rotate(angle_degrees=90)
```

##### `ImageBuffer.flip``(horizontal, vertical)`

Flips the image horizontally and/or vertically using hardware acceleration.

```
import visiong

img = visiong.ImageBuffer()
result = img.flip(horizontal=True, vertical=False)
```

##### `ImageBuffer.find_blobs``(thresholds, invert=..., roi=..., x_stride=..., y_stride=..., area_threshold=..., pixels_threshold=..., merge=..., margin=..., mode=..., erode_size=..., dilate_size=...)`

(Finds blobs by color thresholds. thresholds: list of 6-tuples (H\_min,H\_max,S\_min,S\_max,V\_min,V\_max) for HSV; mode 0=HSV, 1=LAB. For grayscale use the overload with \[(gray\_min, gray\_max)\].)

```
import visiong

img = visiong.ImageBuffer()
result = img.find_blobs(thresholds=[(0, 80)])
```

##### `ImageBuffer.find_lines``(x, y, w, h, x_stride=..., y_stride=..., threshold=..., rho_resolution_px=..., theta_resolution_deg=..., canny_low_thresh=..., canny_high_thresh=...)`

theta\_resolution\_deg

```
import visiong

img = visiong.ImageBuffer()
result = img.find_lines(x=10, y=10, w=120, h=90)
```

##### `ImageBuffer.find_circles``(x, y, w, h, x_stride=..., y_stride=..., threshold=..., r_min=..., r_max=..., r_step=..., canny_low_thresh=..., canny_high_thresh=...)`

canny\_high\_thresh

```
import visiong

img = visiong.ImageBuffer()
result = img.find_circles(x=10, y=10, w=120, h=90)
```

##### `ImageBuffer.find_polygons``(x, y, w, h, min_area=..., max_area=..., min_sides=..., max_sides=..., accuracy=...)`

accuracy must be 'fast', 'normal', or 'accurate'.

```
import visiong

img = visiong.ImageBuffer()
result = img.find_polygons(x=10, y=10, w=120, h=90)
```

##### `ImageBuffer.find_qrcodes``()`

Finds and decodes QR codes in the image. The image is automatically converted to grayscale if needed.

```
import visiong

img = visiong.ImageBuffer()
result = img.find_qrcodes()
```

##### `ImageBuffer.find_squares``(roi=..., threshold_val, min_area=..., approx_epsilon=..., corner_sample_radius=..., corner_ratio_thresh=..., edge_check_offset=..., area_sample_points=..., area_white_thresh=..., area_morph_close_kernel_size=..., duplicate_center_thresh=..., duplicate_area_thresh=...)`

(Finds squares in the image using a robust corner-based algorithm.

```
import visiong

img = visiong.ImageBuffer()
result = img.find_squares(threshold_val=120)
```

##### `ImageBuffer.binarize``(method=..., threshold_range=..., invert=..., adaptive_block_size=..., adaptive_c=..., pre_blur_kernel_size=..., post_morph_kernel_size=...)`

(Performs image binarization with adjustable denoising strength.

```
import visiong

img = visiong.ImageBuffer()
result = img.binarize(method='otsu')
```

##### `ImageBuffer.warp_perspective``(quad, out_width, out_height)`

(Performs a perspective warp transformation.

```
import visiong

img = visiong.ImageBuffer()
result = img.warp_perspective(quad=[(0, 0), (319, 0), (319, 239), (0, 239)], out_width=320, out_height=240)
```

##### `ImageBuffer.letterbox``(target_width, target_height, color=...)`

Scales the image to fit inside target dimensions while preserving aspect ratio, then pads with color to fill target\_width x target\_height. Uses RGA.

```
import visiong

img = visiong.ImageBuffer()
result = img.letterbox(target_width=640, target_height=360)
```

##### `ImageBuffer.draw_line``(x0, y0, x1, y1, color=..., thickness=...)`

Draws a line on the image in-place and returns itself.

```
import visiong

img = visiong.ImageBuffer()
img.draw_line(x0=10, y0=10, x1=120, y1=90)
```

##### `ImageBuffer.draw_rectangle``(x, y, w, h, color=..., thickness=..., fill=...)`

Draws a rectangle on the image in-place and returns itself.

```
import visiong

img = visiong.ImageBuffer()
img.draw_rectangle(x=10, y=10, w=120, h=90)
```

##### `ImageBuffer.draw_circle``(cx, cy, radius, color=..., thickness=..., fill=...)`

Draws a circle on the image in-place and returns itself.

```
import visiong

img = visiong.ImageBuffer()
img.draw_circle(cx=160, cy=120, radius=30)
```

##### `ImageBuffer.draw_string``(x, y, text, color=..., scale=..., thickness=...)`

Draws text at (x,y). color (R,G,B). scale and thickness affect size. In-place, returns self.

```
import visiong

img = visiong.ImageBuffer()
img.draw_string(x=10, y=10, text='hello visiong')
```

##### `ImageBuffer.set_text_font``(font_path=..., predefine_chars=..., glyph_budget=...)` · static

Configures the shared UTF-8 font used by draw\_string (e.g. for Chinese).

```
import visiong

value = 0
visiong.ImageBuffer.set_text_font(value)
```

##### `ImageBuffer.clear_text_font``()` · static

Clears draw\_string shared font configuration.

```
import visiong

visiong.ImageBuffer.clear_text_font()
```

##### `ImageBuffer.draw_cross``(cx, cy, color=..., size=..., thickness=...)`

Draws a cross on the image in-place and returns itself.

```
import visiong

img = visiong.ImageBuffer()
img.draw_cross(cx=160, cy=120)
```

##### `ImageBuffer.paste``(img_to_paste, x, y)`

Pastes another image onto this one at the specified (x, y) coordinates.

```
import visiong

img = visiong.ImageBuffer()
img2 = visiong.ImageBuffer.create(320, 240, "bgr888", (255, 255, 255))
result = img.paste(img_to_paste=img2, x=10, y=10)
```

##### `ImageBuffer.blend``(img_to_blend, x=..., y=...)`

Blends an RGBA image onto this image using its alpha channel. This is a CPU operation.

```
import visiong

img = visiong.ImageBuffer()
img2 = visiong.ImageBuffer.create(320, 240, "bgr888", (255, 255, 255))
result = img.blend(img_to_blend=img2)
```

##### `ImageBuffer.__repr__``()`

<ImageBuffer

```
import visiong

img = visiong.ImageBuffer()
text = repr(img)
```

DisplayUDP 4 methods · 0 fields

##### `DisplayUDP.init``(ip_address, port, jpeg_quality=...)`

Initializes or re-initializes the UDP sender (target IP, port, JPEG quality 1-100).

```
import visiong

udp = visiong.DisplayUDP()
result = udp.init(ip_address='127.0.0.1', port=8080)
```

##### `DisplayUDP.display``(img_buf)`

Encodes the ImageBuffer to JPEG (VENC) and sends it via UDP to the configured address. The DisplayUDP-local lock may convert color format and black-pad smaller frames before encoding.

```
import visiong

udp = visiong.DisplayUDP()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
udp.display(img_buf=img)
```

##### `DisplayUDP.release``()`

Releases DisplayUDP resources.

```
import visiong

udp = visiong.DisplayUDP()
udp.release()
```

##### `DisplayUDP.is_initialized``()`

Checks if DisplayUDP is initialized.

```
import visiong

udp = visiong.DisplayUDP()
result = udp.is_initialized()
```

TouchPoint 1 methods · 2 fields

##### `TouchPoint.__repr__``()`

<TouchPoint x=

```
import visiong

touchPoint = visiong.TouchPoint()
text = repr(touchPoint)
```

##### `TouchPoint.x` · field

readonly\_field

```
import visiong

touchPoint = visiong.TouchPoint()
value = touchPoint.x
```

##### `TouchPoint.y` · field

readonly\_field

```
import visiong

touchPoint = visiong.TouchPoint()
value = touchPoint.y
```

Touch 4 methods · 0 fields

##### `Touch.release``()`

Releases the touch device (closes I2C).

```
import visiong

touch = visiong.Touch()
touch.release()
```

##### `Touch.is_pressed``()`

Returns True if at least one finger is on the screen.

```
import visiong

touch = visiong.Touch()
result = touch.is_pressed()
```

##### `Touch.read``()`

Reads all active touch points and returns a list of TouchPoint objects.

```
import visiong

touch = visiong.Touch()
result = touch.read()
```

##### `Touch.configure_geometry``(original_width, original_height, rotation_degrees)`

Re-configures the screen geometry and coordinate rotation at runtime.

```
import visiong

touch = visiong.Touch()
result = touch.configure_geometry(original_width=640, original_height=360, rotation_degrees=270)
```

DisplayFB 4 methods · 0 fields

##### `DisplayFB.display``(img_buf, roi)`

Displays the full image on the framebuffer (non-blocking). Returns True on success.

```
import visiong

fb = visiong.DisplayFB()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
fb.display(img_buf=img, roi=(0, 0, 320, 240))
```

##### `DisplayFB.release``()`

Releases framebuffer resources.

```
import visiong

fb = visiong.DisplayFB()
fb.release()
```

##### `DisplayFB.is_initialized``()`

Returns True if the framebuffer is initialized.

```
import visiong

fb = visiong.DisplayFB()
result = fb.is_initialized()
```

##### `DisplayFB.__repr__``()`

DisplayFB(screen\_width={}, screen\_height={})

```
import visiong

fb = visiong.DisplayFB()
text = repr(fb)
```

Detection 1 methods · 7 fields

##### `Detection.__repr__``()`

<Detection label='

```
import visiong

detection = visiong.Detection()
text = repr(detection)
```

##### `Detection.box` · field

readonly\_field

```
import visiong

detection = visiong.Detection()
value = detection.box
```

##### `Detection.score` · field

readonly\_field

```
import visiong

detection = visiong.Detection()
value = detection.score
```

##### `Detection.class_id` · field

readonly\_field

```
import visiong

detection = visiong.Detection()
value = detection.class_id
```

##### `Detection.label` · field

readonly\_field

```
import visiong

detection = visiong.Detection()
value = detection.label
```

##### `Detection.landmarks` · field

readonly\_field

```
import visiong

detection = visiong.Detection()
value = detection.landmarks
```

##### `Detection.keypoints` · field

readonly\_field

```
import visiong

detection = visiong.Detection()
value = detection.keypoints
```

##### `Detection.mask_points` · field

readonly\_field

```
import visiong

detection = visiong.Detection()
value = detection.mask_points
```

NPU 5 methods · 0 fields

##### `NPU.infer``(img_buf, roi=..., model_format=...)`

Runs inference. For detection/pose models (YOLOv5, RetinaFace, YOLO11, YOLO11\_SEG, YOLO11\_POSE) returns a list of Detection.

```
import visiong

npu = visiong.NPU(model_type='yolov5', model_path='/path/to/model.rknn')
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = npu.infer(img_buf=img)
```

##### `NPU.get_face_feature``(face_image)`

Extracts a 128-dimensional feature vector from a cropped face image. Requires FACENET model\_type; raises RuntimeError otherwise.

```
import visiong

npu = visiong.NPU(model_type='yolov5', model_path='/path/to/model.rknn')
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = npu.get_face_feature(face_image=img)
```

##### `NPU.recognize_plate``(plate_image)`

Recognizes a license plate from a cropped image. For use with LPRNET models. Returns a string.

```
import visiong

npu = visiong.NPU(model_type='yolov5', model_path='/path/to/model.rknn')
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = npu.recognize_plate(plate_image=img)
```

##### `NPU.get_feature_distance``(feature1, feature2)` · static

Euclidean distance between two 128-D face feature vectors. Returns 100.0 if either length is not 128.

```
import visiong

feature1 = [0.0] * 512
feature2 = [0.0] * 512
result = visiong.NPU.get_feature_distance(feature1=feature1, feature2=feature2)
```

##### `NPU.is_initialized``()`

Checks if the NPU is initialized.

```
import visiong

npu = visiong.NPU(model_type='yolov5', model_path='/path/to/model.rknn')
result = npu.is_initialized()
```

LowLevelTensorInfo 0 methods · 14 fields

该类没有公开方法示例。

##### `LowLevelTensorInfo.index` · field

readonly\_field

```
import visiong

lowLevelTensorInfo = visiong.LowLevelTensorInfo()
value = lowLevelTensorInfo.index
```

##### `LowLevelTensorInfo.name` · field

readonly\_field

```
import visiong

lowLevelTensorInfo = visiong.LowLevelTensorInfo()
value = lowLevelTensorInfo.name
```

##### `LowLevelTensorInfo.dims` · field

readonly\_field

```
import visiong

lowLevelTensorInfo = visiong.LowLevelTensorInfo()
value = lowLevelTensorInfo.dims
```

##### `LowLevelTensorInfo.format` · field

readonly\_field

```
import visiong

lowLevelTensorInfo = visiong.LowLevelTensorInfo()
value = lowLevelTensorInfo.format
```

##### `LowLevelTensorInfo.type` · field

readonly\_field

```
import visiong

lowLevelTensorInfo = visiong.LowLevelTensorInfo()
value = lowLevelTensorInfo.type
```

##### `LowLevelTensorInfo.quant_type` · field

readonly\_field

```
import visiong

lowLevelTensorInfo = visiong.LowLevelTensorInfo()
value = lowLevelTensorInfo.quant_type
```

##### `LowLevelTensorInfo.zero_point` · field

readonly\_field

```
import visiong

lowLevelTensorInfo = visiong.LowLevelTensorInfo()
value = lowLevelTensorInfo.zero_point
```

##### `LowLevelTensorInfo.scale` · field

readonly\_field

```
import visiong

lowLevelTensorInfo = visiong.LowLevelTensorInfo()
value = lowLevelTensorInfo.scale
```

##### `LowLevelTensorInfo.num_elements` · field

readonly\_field

```
import visiong

lowLevelTensorInfo = visiong.LowLevelTensorInfo()
value = lowLevelTensorInfo.num_elements
```

##### `LowLevelTensorInfo.size_bytes` · field

readonly\_field

```
import visiong

lowLevelTensorInfo = visiong.LowLevelTensorInfo()
value = lowLevelTensorInfo.size_bytes
```

##### `LowLevelTensorInfo.size_with_stride_bytes` · field

readonly\_field

```
import visiong

lowLevelTensorInfo = visiong.LowLevelTensorInfo()
value = lowLevelTensorInfo.size_with_stride_bytes
```

##### `LowLevelTensorInfo.w_stride` · field

readonly\_field

```
import visiong

lowLevelTensorInfo = visiong.LowLevelTensorInfo()
value = lowLevelTensorInfo.w_stride
```

##### `LowLevelTensorInfo.h_stride` · field

readonly\_field

```
import visiong

lowLevelTensorInfo = visiong.LowLevelTensorInfo()
value = lowLevelTensorInfo.h_stride
```

##### `LowLevelTensorInfo.pass_through` · field

readonly\_field

```
import visiong

lowLevelTensorInfo = visiong.LowLevelTensorInfo()
value = lowLevelTensorInfo.pass_through
```

LowLevelNPU 26 methods · 0 fields

##### `LowLevelNPU.is_initialized``()`

is\_initialized

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.is_initialized()
```

##### `LowLevelNPU.num_inputs``()`

类方法最小示例

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.num_inputs()
```

##### `LowLevelNPU.num_outputs``()`

类方法最小示例

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.num_outputs()
```

##### `LowLevelNPU.input_tensors``()`

input\_tensors

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.input_tensors()
```

##### `LowLevelNPU.output_tensors``()`

output\_tensors

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.output_tensors()
```

##### `LowLevelNPU.input_tensor``(index)`

input\_tensor

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.input_tensor(index=0)
```

##### `LowLevelNPU.output_tensor``(index)`

output\_tensor

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.output_tensor(index=0)
```

##### `LowLevelNPU.input_shape``(index)`

类方法最小示例

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.input_shape(index=0)
```

##### `LowLevelNPU.output_shape``(index)`

output\_shape

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.output_shape(index=0)
```

##### `LowLevelNPU.sdk_versions``()`

sdk\_versions

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.sdk_versions()
```

##### `LowLevelNPU.set_core_mask``(core_mask)`

Sets RKNN core mask. Accepts: 'auto', '0', '1', '2', '0\_1', '0\_1\_2'.

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
llnpu.set_core_mask(core_mask='auto')
```

##### `LowLevelNPU.set_input_attr``(index, tensor_type, tensor_format, pass_through)`

Rebinds one input tensor attr. Example: set\_input\_attr(0, 'uint8', 'nhwc', False).

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
llnpu.set_input_attr(index=0, tensor_type=0, tensor_format='rgb', pass_through=True)
```

##### `LowLevelNPU.reset_input_attr``(index)`

Resets an input tensor attr to its startup value.

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.reset_input_attr(index=0)
```

##### `LowLevelNPU.set_input_bytes``(index, payload, zero_pad=..., sync_to_device=...)`

Writes raw bytes into an input tensor buffer.

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
llnpu.set_input_bytes(index=0, payload='payload')
```

##### `LowLevelNPU.set_input_array``(index, array, quantize_if_needed=..., zero_pad=..., sync_to_device=...)`

Writes a numpy array into input tensor memory. Float arrays can be auto-quantized.

```
import visiong
import numpy as np

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
arr = np.zeros((1, 224, 224, 3), dtype=np.uint8)
llnpu.set_input_array(index=0, array=arr)
```

##### `LowLevelNPU.set_input_image``(index, image, color_order=..., keep_aspect=..., pad_value=..., driver_convert=...)`

Writes ImageBuffer into input tensor memory using RGA path when possible.

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
llnpu.set_input_image(index=0, image=img)
```

##### `LowLevelNPU.sync_input_to_device``(index)`

sync\_input\_to\_device

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.sync_input_to_device(index=0)
```

##### `LowLevelNPU.sync_output_from_device``(index)`

sync\_output\_from\_device

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.sync_output_from_device(index=0)
```

##### `LowLevelNPU.sync_all_outputs_from_device``()`

sync\_all\_outputs\_from\_device

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.sync_all_outputs_from_device()
```

##### `LowLevelNPU.run``(sync_outputs=..., non_block=..., timeout_ms=...)`

Runs RKNN. Optional sync\_outputs controls output cache sync.

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.run(sync_outputs=True)
```

##### `LowLevelNPU.wait``(timeout_ms=...)`

类方法最小示例

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.wait(timeout_ms=0)
```

##### `LowLevelNPU.output_bytes``(index, with_stride=..., sync_from_device=...)`

Returns raw output bytes.

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.output_bytes(index=0)
```

##### `LowLevelNPU.output_float``(index, dequantize_if_needed=..., sync_from_device=...)`

Returns output tensor as float32 numpy array (dequantized when possible).

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.output_float(index=0)
```

##### `LowLevelNPU.output_array``(index, dequantize_if_needed=..., sync_from_device=...)`

Returns output as float array (default) or raw uint8 vector when dequantize\_if\_needed=False.

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.output_array(index=0)
```

##### `LowLevelNPU.input_dma_fd``(index)`

input\_dma\_fd

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.input_dma_fd(index=0)
```

##### `LowLevelNPU.output_dma_fd``(index)`

output\_dma\_fd

```
import visiong

llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')
result = llnpu.output_dma_fd(index=0)
```

OCRResult 1 methods · 5 fields

##### `OCRResult.__repr__``()`

<OCRResult text='

```
import visiong

oCRResult = visiong.OCRResult()
text = repr(oCRResult)
```

##### `OCRResult.quad` · field

readonly\_field

```
import visiong

oCRResult = visiong.OCRResult()
value = oCRResult.quad
```

##### `OCRResult.rect` · field

readonly\_field

```
import visiong

oCRResult = visiong.OCRResult()
value = oCRResult.rect
```

##### `OCRResult.det_score` · field

readonly\_field

```
import visiong

oCRResult = visiong.OCRResult()
value = oCRResult.det_score
```

##### `OCRResult.text` · field

readonly\_field

```
import visiong

oCRResult = visiong.OCRResult()
value = oCRResult.text
```

##### `OCRResult.text_score` · field

readonly\_field

```
import visiong

oCRResult = visiong.OCRResult()
value = oCRResult.text_score
```

PPOCR 2 methods · 0 fields

##### `PPOCR.infer``(img_buf)`

Runs DET+REC OCR on one image and returns a list of OCRResult.

```
import visiong

ocr = visiong.PPOCR(det_model_path='/path/to/det.rknn', rec_model_path='/path/to/rec.rknn')
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = ocr.infer(img_buf=img)
```

##### `PPOCR.is_initialized``()`

Checks whether PPOCR runtime is initialized.

```
import visiong

ocr = visiong.PPOCR(det_model_path='/path/to/det.rknn', rec_model_path='/path/to/rec.rknn')
result = ocr.is_initialized()
```

NanoTrackResult 0 methods · 2 fields

该类没有公开方法示例。

##### `NanoTrackResult.box` · field

readonly\_field

```
import visiong

nanoTrackResult = visiong.NanoTrackResult()
value = nanoTrackResult.box
```

##### `NanoTrackResult.score` · field

readonly\_field

```
import visiong

nanoTrackResult = visiong.NanoTrackResult()
value = nanoTrackResult.score
```

NanoTrack 4 methods · 0 fields

##### `NanoTrack.init``(img_buf, bbox)`

Initializes tracker state with the first frame and initial bbox (x, y, w, h).

```
import visiong

tracker = visiong.NanoTrack(template_model=0, search_model=0, head_model='/path/to/head.rknn')
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = tracker.init(img_buf=img, bbox=(60, 40, 120, 90))
```

##### `NanoTrack.track``(img_buf)`

Tracks target on a new frame and returns NanoTrackResult.

```
import visiong

tracker = visiong.NanoTrack(template_model=0, search_model=0, head_model='/path/to/head.rknn')
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = tracker.track(img_buf=img)
```

##### `NanoTrack.is_initialized``()`

Returns True after init() succeeds.

```
import visiong

tracker = visiong.NanoTrack(template_model=0, search_model=0, head_model='/path/to/head.rknn')
result = tracker.is_initialized()
```

##### `NanoTrack.reset``()`

Clears tracker state. You must call init() again before track().

```
import visiong

tracker = visiong.NanoTrack(template_model=0, search_model=0, head_model='/path/to/head.rknn')
result = tracker.reset()
```

Canvas 0 methods · 0 fields

该类没有公开方法示例。

GUI 63 methods · 0 fields

##### `GUI.begin_frame``(touch_device)`

Starts a new frame. Pass Touch device or None. Call before any widgets.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.begin_frame(touch_device=None)
```

##### `GUI.begin_window``(title, x, y, w, h, flags=...)`

Begins a window. x,y,w,h are floats. Returns True if the window is visible.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.begin_window(title='Demo', x=10, y=10, w=120, h=90)
```

##### `GUI.end_window``()`

Ends the current window.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.end_window()
```

##### `GUI.end_frame``(target_image)`

Ends the frame and renders the GUI into the given ImageBuffer.

```
import visiong

gui = visiong.GUI(width=640, height=360)
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = gui.end_frame(target_image=img)
```

##### `GUI.layout_row_dynamic``(height, cols=...)`

Starts a row with dynamic column widths. cols: number of widgets in the row.

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.layout_row_dynamic(height=360)
```

##### `GUI.layout_row_static``(height, item_width, cols)`

Starts a row with fixed widget width.

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.layout_row_static(height=360, item_width=640, cols=1)
```

##### `GUI.layout_row_begin``(format, row_height, cols)`

Begins a row with custom format (e.g. percentage). Use layout\_row\_push then layout\_row\_end.

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.layout_row_begin(format='bgr888', row_height=360, cols=1)
```

##### `GUI.layout_row_push``(value)`

Pushes a column size (after layout\_row\_begin).

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.layout_row_push(value=0)
```

##### `GUI.layout_row_end``()`

Ends the row started by layout\_row\_begin.

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.layout_row_end()
```

##### `GUI.group_begin``(title, flags=...)`

Begins a group. Returns True if the group is visible. Must call group\_end after.

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.group_begin(title='Demo')
```

##### `GUI.group_end``()`

Ends the current group.

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.group_end()
```

##### `GUI.label``(text, align=...)`

Draws a label. align: 'left', 'right', 'center'.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.label(text='hello visiong')
```

##### `GUI.label_wrap``(text)`

Draws a label with word wrapping.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.label_wrap(text='hello visiong')
```

##### `GUI.button``(label)`

Returns True if the button was clicked.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.button(label='object')
```

##### `GUI.slider``(label, value, min, max, step)`

Slider widget. Touch interaction is relative-drag based: press anywhere in range, then slide horizontally to adjust.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.slider(label='object', value=0, min=0, max=100, step=0)
```

##### `GUI.checkbox``(label, is_checked)`

Checkbox. Returns the new checked state (bool).

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.checkbox(label='object', is_checked=True)
```

##### `GUI.option``(label, is_active)`

Radio option. Returns True if this option is selected.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.option(label='object', is_active=True)
```

##### `GUI.edit_string``(text, max_len=...)`

Single-line text edit. Returns (changed: bool, text: str).

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.edit_string(text='hello visiong')
```

##### `GUI.progress``(current, max, is_modifyable=...)`

Progress bar. If modifiable, touch interaction is relative-drag based rather than absolute jump-to-position.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.progress(current=30, max=100)
```

##### `GUI.button_image``(image)`

Creates a clickable button from an ImageBuffer. Returns True if clicked.

```
import visiong

gui = visiong.GUI(width=640, height=360)
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
result = gui.button_image(image=img)
```

##### `GUI.tree_node``(title, is_expanded)`

Begins a tree node. Returns True if expanded. Call tree\_pop when done with children.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.tree_node(title='Demo', is_expanded=True)
```

##### `GUI.tree_pop``()`

Ends the current tree node.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.tree_pop()
```

##### `GUI.property_int``(name, value, min, max, step, inc_per_pixel=...)`

Integer property control. Touch drag adjusts by horizontal delta instead of jumping to an absolute position.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.property_int(name='demo', value=0, min=0, max=100, step=0)
```

##### `GUI.property_float``(name, value, min, max, step, inc_per_pixel=...)`

Float property control. Touch drag adjusts by horizontal delta instead of jumping to an absolute position.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.property_float(name='demo', value=0, min=0, max=100, step=0)
```

##### `GUI.combo_begin``(text, width, height)`

Begins a combo box. Popup placement is touch-first and constrained within the parent window.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.combo_begin(text='hello visiong', width=640, height=360)
```

##### `GUI.combo_item``(text)`

Adds an item to the current combo. Returns True if selected.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.combo_item(text='hello visiong')
```

##### `GUI.combo_end``()`

Ends the combo box.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.combo_end()
```

##### `GUI.contextual_begin``(width, height)`

Begins a contextual menu. On touch devices this is typically opened via long press, with popup placement constrained to the parent window.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.contextual_begin(width=640, height=360)
```

##### `GUI.contextual_item``(text)`

Adds an item to the contextual menu. Returns True if clicked.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.contextual_item(text='hello visiong')
```

##### `GUI.contextual_end``()`

Ends the contextual menu.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.contextual_end()
```

##### `GUI.chart_begin``(type, count, min_val, max_val)`

Begins a chart section. Type can be 'lines' or 'columns'.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.chart_begin(type='lines', count=0, min_val=0, max_val=100)
```

##### `GUI.chart_push``(value)`

Pushes a new value to the active chart.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.chart_push(value=0)
```

##### `GUI.chart_end``()`

Ends the chart section.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.chart_end()
```

##### `GUI.menubar_begin``()`

Begins a menubar at the top of the current window.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.menubar_begin()
```

##### `GUI.menubar_end``()`

Ends the menubar section.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.menubar_end()
```

##### `GUI.menu_begin``(label, width, height)`

Begins a dropdown menu.

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.menu_begin(label='object', width=640, height=360)
```

##### `GUI.input_is_pointer_down_in_rect``(rect, primary_pointer=...)`

Touch-first alias of input\_is\_mouse\_down\_in\_rect. Returns True if the primary pointer is held inside the rect.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.input_is_pointer_down_in_rect(rect=(60, 40, 120, 90))
```

##### `GUI.input_is_pointer_dragging_in_rect``()`

Touch-first alias of input\_is\_mouse\_dragging\_in\_rect. Returns drag state and delta, including momentum when active.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.input_is_pointer_dragging_in_rect()
```

##### `GUI.is_title_bar_active``()`

Touch-first alias of is\_title\_bar\_pressed. Returns True while the title bar is held or being dragged.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.is_title_bar_active()
```

##### `GUI.get_scroll_delta_y``()`

Touch-first alias of get\_smart\_scroll\_dy. Returns current scroll delta, including momentum when active.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.get_scroll_delta_y()
```

##### `GUI.menu_item``(label)`

Adds a clickable item to a menu.

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.menu_item(label='object')
```

##### `GUI.menu_end``()`

Ends the menu section.

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.menu_end()
```

##### `GUI.tooltip``(text)`

Shows a tooltip for the previously declared widget. On touch devices it appears on long press.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.tooltip(text='hello visiong')
```

##### `GUI.get_canvas``()`

Returns the current window's Canvas for custom drawing (stroke\_line, fill\_rect, draw\_text, etc.).

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.get_canvas()
```

##### `GUI.widget_bounds``(canvas)`

Returns (x, y, w, h) of the last laid-out widget. Pass the canvas from get\_canvas.

```
import visiong

gui = visiong.GUI(width=640, height=360)
canvas = visiong.ImageBuffer.create(320, 240, "bgr888", (20, 20, 20))
result = gui.widget_bounds(canvas=canvas)
```

##### `GUI.input_is_mouse_down_in_rect``(rect, left_mouse=...)`

Returns True if the primary pointer is held inside the given (x,y,w,h) rect. On touch devices this follows finger contact.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.input_is_mouse_down_in_rect(rect=(60, 40, 120, 90))
```

##### `GUI.window_set_focus``(name)`

Sets focus to the window with the given name.

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.window_set_focus(name='demo')
```

##### `GUI.window_drag_from_pos``(canvas)`

Direct-manipulation window drag. On touch devices this moves the current window while dragging its title bar.

```
import visiong

gui = visiong.GUI(width=640, height=360)
canvas = visiong.ImageBuffer.create(320, 240, "bgr888", (20, 20, 20))
gui.window_drag_from_pos(canvas=canvas)
```

##### `GUI.window_set_scroll``(scroll_y)`

Sets the current window's vertical scroll offset.

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.window_set_scroll(scroll_y=0)
```

##### `GUI.input_is_mouse_dragging_in_rect``()`

Returns (is\_dragging, scroll\_dy, (x,y,w,h) content\_rect). On touch devices scroll\_dy includes locked-axis drag and fling momentum.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.input_is_mouse_dragging_in_rect()
```

##### `GUI.is_title_bar_pressed``()`

Returns True if the current window title bar is actively held or dragged.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.is_title_bar_pressed()
```

##### `GUI.get_content_height``()`

Returns the content area height of the current layout.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.get_content_height()
```

##### `GUI.push_style_vec2``(name, x, y)`

Pushes a vec2 style. name: 'padding' or 'spacing'. Must be popped with pop\_style.

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.push_style_vec2(name='demo', x=10, y=10)
```

##### `GUI.pop_style``()`

Pops the last pushed style (e.g. vec2).

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.pop_style()
```

##### `GUI.get_smart_scroll_dy``()`

Returns touch-first scroll delta for the current window, including momentum when active.

```
import visiong

gui = visiong.GUI(width=640, height=360)
result = gui.get_smart_scroll_dy()
```

##### `GUI.stroke_line``(canvas, x0, y0, x1, y1, thickness, color)`

Draws a line on the canvas. color: (R,G,B,A).

```
import visiong

gui = visiong.GUI(width=640, height=360)
canvas = visiong.ImageBuffer.create(320, 240, "bgr888", (20, 20, 20))
result = gui.stroke_line(canvas=canvas, x0=10, y0=10, x1=120, y1=90, thickness=1, color=(0, 255, 0))
```

##### `GUI.stroke_rect``(canvas, x, y, w, h, rounding, thickness, color)`

Draws a rectangle outline. color: (R,G,B,A).

```
import visiong

gui = visiong.GUI(width=640, height=360)
canvas = visiong.ImageBuffer.create(320, 240, "bgr888", (20, 20, 20))
result = gui.stroke_rect(canvas=canvas, x=10, y=10, w=120, h=90, rounding=4, thickness=1, color=(0, 255, 0))
```

##### `GUI.fill_rect``(canvas, x, y, w, h, rounding, color)`

Fills a rectangle. color: (R,G,B,A).

```
import visiong

gui = visiong.GUI(width=640, height=360)
canvas = visiong.ImageBuffer.create(320, 240, "bgr888", (20, 20, 20))
result = gui.fill_rect(canvas=canvas, x=10, y=10, w=120, h=90, rounding=4, color=(0, 255, 0))
```

##### `GUI.draw_text``(canvas, x, y, text, color)`

Draws text at (x,y). color: (R,G,B,A).

```
import visiong

gui = visiong.GUI(width=640, height=360)
canvas = visiong.ImageBuffer.create(320, 240, "bgr888", (20, 20, 20))
gui.draw_text(canvas=canvas, x=10, y=10, text='hello visiong', color=(0, 255, 0))
```

##### `GUI.set_style_color``(property_name, color)`

Sets a theme color. property\_name: 'text', 'header\_bg', 'button\_normal', 'button\_hover', 'button\_active'. color: (R,G,B,A).

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.set_style_color(property_name='name', color=(0, 255, 0))
```

##### `GUI.set_style_button_rounding``(rounding)`

Sets button corner rounding radius.

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.set_style_button_rounding(rounding=4)
```

##### `GUI.set_style_window_rounding``(rounding)`

Sets window corner rounding radius.

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.set_style_window_rounding(rounding=4)
```

##### `GUI.set_window_background_color``(color)`

Sets the current window background color. color: (R,G,B,A).

```
import visiong

gui = visiong.GUI(width=640, height=360)
gui.set_window_background_color(color=(0, 255, 0))
```

PinId 1 methods · 2 fields

##### `PinId.__repr__``()`

类方法最小示例

```
import visiong

pinId = visiong.PinId()
text = repr(pinId)
```

##### `PinId.bank` · field

readonly\_field

```
import visiong

pinId = visiong.PinId()
value = pinId.bank
```

##### `PinId.pin` · field

readonly\_field

```
import visiong

pinId = visiong.PinId()
value = pinId.pin
```

PinMuxRegisterInfo 1 methods · 8 fields

##### `PinMuxRegisterInfo.__repr__``()`

PinMuxRegisterInfo(domain='

```
import visiong

pinMuxRegisterInfo = visiong.PinMuxRegisterInfo()
text = repr(pinMuxRegisterInfo)
```

##### `PinMuxRegisterInfo.domain` · field

readonly\_field

```
import visiong

pinMuxRegisterInfo = visiong.PinMuxRegisterInfo()
value = pinMuxRegisterInfo.domain
```

##### `PinMuxRegisterInfo.base_addr` · field

readonly\_field

```
import visiong

pinMuxRegisterInfo = visiong.PinMuxRegisterInfo()
value = pinMuxRegisterInfo.base_addr
```

##### `PinMuxRegisterInfo.reg_offset` · field

readonly\_field

```
import visiong

pinMuxRegisterInfo = visiong.PinMuxRegisterInfo()
value = pinMuxRegisterInfo.reg_offset
```

##### `PinMuxRegisterInfo.absolute_addr` · field

readonly\_field

```
import visiong

pinMuxRegisterInfo = visiong.PinMuxRegisterInfo()
value = pinMuxRegisterInfo.absolute_addr
```

##### `PinMuxRegisterInfo.bit` · field

readonly\_field

```
import visiong

pinMuxRegisterInfo = visiong.PinMuxRegisterInfo()
value = pinMuxRegisterInfo.bit
```

##### `PinMuxRegisterInfo.width` · field

readonly\_field

```
import visiong

pinMuxRegisterInfo = visiong.PinMuxRegisterInfo()
value = pinMuxRegisterInfo.width
```

##### `PinMuxRegisterInfo.mask` · field

readonly\_field

```
import visiong

pinMuxRegisterInfo = visiong.PinMuxRegisterInfo()
value = pinMuxRegisterInfo.mask
```

##### `PinMuxRegisterInfo.gpio_only` · field

readonly\_field

```
import visiong

pinMuxRegisterInfo = visiong.PinMuxRegisterInfo()
value = pinMuxRegisterInfo.gpio_only
```

PinAltFunction 1 methods · 3 fields

##### `PinAltFunction.__repr__``()`

PinAltFunction(function='

```
import visiong

pinAltFunction = visiong.PinAltFunction()
text = repr(pinAltFunction)
```

##### `PinAltFunction.function` · field

readonly\_field

```
import visiong

pinAltFunction = visiong.PinAltFunction()
value = pinAltFunction.function
```

##### `PinAltFunction.group` · field

readonly\_field

```
import visiong

pinAltFunction = visiong.PinAltFunction()
value = pinAltFunction.group
```

##### `PinAltFunction.mux` · field

readonly\_field

```
import visiong

pinAltFunction = visiong.PinAltFunction()
value = pinAltFunction.mux
```

PinRuntimeStatus 0 methods · 7 fields

该类没有公开方法示例。

##### `PinRuntimeStatus.found` · field

readonly\_field

```
import visiong

pinRuntimeStatus = visiong.PinRuntimeStatus()
value = pinRuntimeStatus.found
```

##### `PinRuntimeStatus.bank` · field

readonly\_field

```
import visiong

pinRuntimeStatus = visiong.PinRuntimeStatus()
value = pinRuntimeStatus.bank
```

##### `PinRuntimeStatus.pin` · field

readonly\_field

```
import visiong

pinRuntimeStatus = visiong.PinRuntimeStatus()
value = pinRuntimeStatus.pin
```

##### `PinRuntimeStatus.mux_owner` · field

readonly\_field

```
import visiong

pinRuntimeStatus = visiong.PinRuntimeStatus()
value = pinRuntimeStatus.mux_owner
```

##### `PinRuntimeStatus.gpio_owner` · field

readonly\_field

```
import visiong

pinRuntimeStatus = visiong.PinRuntimeStatus()
value = pinRuntimeStatus.gpio_owner
```

##### `PinRuntimeStatus.function` · field

readonly\_field

```
import visiong

pinRuntimeStatus = visiong.PinRuntimeStatus()
value = pinRuntimeStatus.function
```

##### `PinRuntimeStatus.group` · field

readonly\_field

```
import visiong

pinRuntimeStatus = visiong.PinRuntimeStatus()
value = pinRuntimeStatus.group
```

PinConflictReport 0 methods · 3 fields

该类没有公开方法示例。

##### `PinConflictReport.conflict` · field

readonly\_field

```
import visiong

pinConflictReport = visiong.PinConflictReport()
value = pinConflictReport.conflict
```

##### `PinConflictReport.reason` · field

readonly\_field

```
import visiong

pinConflictReport = visiong.PinConflictReport()
value = pinConflictReport.reason
```

##### `PinConflictReport.runtime` · field

readonly\_field

```
import visiong

pinConflictReport = visiong.PinConflictReport()
value = pinConflictReport.runtime
```

FunctionInterfaceStatus 0 methods · 7 fields

该类没有公开方法示例。

##### `FunctionInterfaceStatus.request` · field

readonly\_field

```
import visiong

functionInterfaceStatus = visiong.FunctionInterfaceStatus()
value = functionInterfaceStatus.request
```

##### `FunctionInterfaceStatus.function` · field

readonly\_field

```
import visiong

functionInterfaceStatus = visiong.FunctionInterfaceStatus()
value = functionInterfaceStatus.function
```

##### `FunctionInterfaceStatus.group` · field

readonly\_field

```
import visiong

functionInterfaceStatus = visiong.FunctionInterfaceStatus()
value = functionInterfaceStatus.group
```

##### `FunctionInterfaceStatus.owner` · field

readonly\_field

```
import visiong

functionInterfaceStatus = visiong.FunctionInterfaceStatus()
value = functionInterfaceStatus.owner
```

##### `FunctionInterfaceStatus.owner_bound` · field

readonly\_field

```
import visiong

functionInterfaceStatus = visiong.FunctionInterfaceStatus()
value = functionInterfaceStatus.owner_bound
```

##### `FunctionInterfaceStatus.interfaces` · field

readonly\_field

```
import visiong

functionInterfaceStatus = visiong.FunctionInterfaceStatus()
value = functionInterfaceStatus.interfaces
```

##### `FunctionInterfaceStatus.note` · field

readonly\_field

```
import visiong

functionInterfaceStatus = visiong.FunctionInterfaceStatus()
value = functionInterfaceStatus.note
```

AdcChannelStatus 0 methods · 10 fields

该类没有公开方法示例。

##### `AdcChannelStatus.available` · field

readonly\_field

```
import visiong

adcChannelStatus = visiong.AdcChannelStatus()
value = adcChannelStatus.available
```

##### `AdcChannelStatus.channel` · field

readonly\_field

```
import visiong

adcChannelStatus = visiong.AdcChannelStatus()
value = adcChannelStatus.channel
```

##### `AdcChannelStatus.raw` · field

readonly\_field

```
import visiong

adcChannelStatus = visiong.AdcChannelStatus()
value = adcChannelStatus.raw
```

##### `AdcChannelStatus.scale` · field

readonly\_field

```
import visiong

adcChannelStatus = visiong.AdcChannelStatus()
value = adcChannelStatus.scale
```

##### `AdcChannelStatus.millivolts` · field

readonly\_field

```
import visiong

adcChannelStatus = visiong.AdcChannelStatus()
value = adcChannelStatus.millivolts
```

##### `AdcChannelStatus.device` · field

readonly\_field

```
import visiong

adcChannelStatus = visiong.AdcChannelStatus()
value = adcChannelStatus.device
```

##### `AdcChannelStatus.raw_path` · field

readonly\_field

```
import visiong

adcChannelStatus = visiong.AdcChannelStatus()
value = adcChannelStatus.raw_path
```

##### `AdcChannelStatus.scale_path` · field

readonly\_field

```
import visiong

adcChannelStatus = visiong.AdcChannelStatus()
value = adcChannelStatus.scale_path
```

##### `AdcChannelStatus.pin_hint` · field

readonly\_field

```
import visiong

adcChannelStatus = visiong.AdcChannelStatus()
value = adcChannelStatus.pin_hint
```

##### `AdcChannelStatus.note` · field

readonly\_field

```
import visiong

adcChannelStatus = visiong.AdcChannelStatus()
value = adcChannelStatus.note
```

GpioLineConfig 0 methods · 0 fields

该类没有公开方法示例。

GpioLineStatus 0 methods · 7 fields

该类没有公开方法示例。

##### `GpioLineStatus.requested` · field

readonly\_field

```
import visiong

gpioLineStatus = visiong.GpioLineStatus()
value = gpioLineStatus.requested
```

##### `GpioLineStatus.value` · field

readonly\_field

```
import visiong

gpioLineStatus = visiong.GpioLineStatus()
value = gpioLineStatus.value
```

##### `GpioLineStatus.bank` · field

readonly\_field

```
import visiong

gpioLineStatus = visiong.GpioLineStatus()
value = gpioLineStatus.bank
```

##### `GpioLineStatus.pin` · field

readonly\_field

```
import visiong

gpioLineStatus = visiong.GpioLineStatus()
value = gpioLineStatus.pin
```

##### `GpioLineStatus.gpiochip` · field

readonly\_field

```
import visiong

gpioLineStatus = visiong.GpioLineStatus()
value = gpioLineStatus.gpiochip
```

##### `GpioLineStatus.config` · field

readonly\_field

```
import visiong

gpioLineStatus = visiong.GpioLineStatus()
value = gpioLineStatus.config
```

##### `GpioLineStatus.note` · field

readonly\_field

```
import visiong

gpioLineStatus = visiong.GpioLineStatus()
value = gpioLineStatus.note
```

DriveStrengthStatus 0 methods · 7 fields

该类没有公开方法示例。

##### `DriveStrengthStatus.available` · field

readonly\_field

```
import visiong

driveStrengthStatus = visiong.DriveStrengthStatus()
value = driveStrengthStatus.available
```

##### `DriveStrengthStatus.level` · field

readonly\_field

```
import visiong

driveStrengthStatus = visiong.DriveStrengthStatus()
value = driveStrengthStatus.level
```

##### `DriveStrengthStatus.raw` · field

readonly\_field

```
import visiong

driveStrengthStatus = visiong.DriveStrengthStatus()
value = driveStrengthStatus.raw
```

##### `DriveStrengthStatus.reg_offset` · field

readonly\_field

```
import visiong

driveStrengthStatus = visiong.DriveStrengthStatus()
value = driveStrengthStatus.reg_offset
```

##### `DriveStrengthStatus.absolute_addr` · field

readonly\_field

```
import visiong

driveStrengthStatus = visiong.DriveStrengthStatus()
value = driveStrengthStatus.absolute_addr
```

##### `DriveStrengthStatus.domain` · field

readonly\_field

```
import visiong

driveStrengthStatus = visiong.DriveStrengthStatus()
value = driveStrengthStatus.domain
```

##### `DriveStrengthStatus.note` · field

readonly\_field

```
import visiong

driveStrengthStatus = visiong.DriveStrengthStatus()
value = driveStrengthStatus.note
```

PullStatus 0 methods · 7 fields

该类没有公开方法示例。

##### `PullStatus.available` · field

readonly\_field

```
import visiong

pullStatus = visiong.PullStatus()
value = pullStatus.available
```

##### `PullStatus.mode` · field

readonly\_field

```
import visiong

pullStatus = visiong.PullStatus()
value = pullStatus.mode
```

##### `PullStatus.raw` · field

readonly\_field

```
import visiong

pullStatus = visiong.PullStatus()
value = pullStatus.raw
```

##### `PullStatus.reg_offset` · field

readonly\_field

```
import visiong

pullStatus = visiong.PullStatus()
value = pullStatus.reg_offset
```

##### `PullStatus.absolute_addr` · field

readonly\_field

```
import visiong

pullStatus = visiong.PullStatus()
value = pullStatus.absolute_addr
```

##### `PullStatus.domain` · field

readonly\_field

```
import visiong

pullStatus = visiong.PullStatus()
value = pullStatus.domain
```

##### `PullStatus.note` · field

readonly\_field

```
import visiong

pullStatus = visiong.PullStatus()
value = pullStatus.note
```

SchmittStatus 0 methods · 12 fields

该类没有公开方法示例。

##### `SchmittStatus.available` · field

readonly\_field

```
import visiong

schmittStatus = visiong.SchmittStatus()
value = schmittStatus.available
```

##### `SchmittStatus.enabled` · field

readonly\_field

```
import visiong

schmittStatus = visiong.SchmittStatus()
value = schmittStatus.enabled
```

##### `SchmittStatus.raw` · field

readonly\_field

```
import visiong

schmittStatus = visiong.SchmittStatus()
value = schmittStatus.raw
```

##### `SchmittStatus.reg_offset` · field

readonly\_field

```
import visiong

schmittStatus = visiong.SchmittStatus()
value = schmittStatus.reg_offset
```

##### `SchmittStatus.absolute_addr` · field

readonly\_field

```
import visiong

schmittStatus = visiong.SchmittStatus()
value = schmittStatus.absolute_addr
```

##### `SchmittStatus.domain` · field

readonly\_field

```
import visiong

schmittStatus = visiong.SchmittStatus()
value = schmittStatus.domain
```

##### `SchmittStatus.note` · field

readonly\_field

```
import visiong

schmittStatus = visiong.SchmittStatus()
value = schmittStatus.note
```

##### `SchmittStatus.bank` · field

readonly\_field

```
import visiong

schmittStatus = visiong.SchmittStatus()
value = schmittStatus.bank
```

##### `SchmittStatus.pin` · field

readonly\_field

```
import visiong

schmittStatus = visiong.SchmittStatus()
value = schmittStatus.pin
```

##### `SchmittStatus.drive_supported` · field

readonly\_field

```
import visiong

schmittStatus = visiong.SchmittStatus()
value = schmittStatus.drive_supported
```

##### `SchmittStatus.pull_supported` · field

readonly\_field

```
import visiong

schmittStatus = visiong.SchmittStatus()
value = schmittStatus.pull_supported
```

##### `SchmittStatus.schmitt_supported` · field

readonly\_field

```
import visiong

schmittStatus = visiong.SchmittStatus()
value = schmittStatus.schmitt_supported
```

PinMux 32 methods · 0 fields

##### `PinMux.is_open``()`

Returns True if memory mappings are active.

```
import visiong

pm = visiong.PinMux()
result = pm.is_open()
```

##### `PinMux.close``()`

Closes /dev/mem mappings.

```
import visiong

pm = visiong.PinMux()
pm.close()
```

##### `PinMux.parse_pin``(pin_name)`

Parses a pin string like 'GPIO1\_C4', 'gpio1-20', or '1:20'.

```
import visiong

pm = visiong.PinMux()
result = pm.parse_pin(pin_name='GPIO0_C3')
```

##### `PinMux.get_mux``(bank, pin)`

Reads current mux value from register field.

```
import visiong

pm = visiong.PinMux()
result = pm.get_mux(bank=0, pin=0)
```

##### `PinMux.set_mux``(bank, pin, mux)`

Writes mux value using Rockchip write-mask semantics (no reboot required).

```
import visiong

pm = visiong.PinMux()
pm.set_mux(bank=0, pin=0, mux=0)
```

##### `PinMux.get_register_info``(bank, pin)`

Returns register address/bitfield info used for this pin.

```
import visiong

pm = visiong.PinMux()
result = pm.get_register_info(bank=0, pin=0)
```

##### `PinMux.list_functions``(bank, pin)`

Lists available alternate functions by parsing /proc/device-tree/pinctrl.

```
import visiong

pm = visiong.PinMux()
result = pm.list_functions(bank=0, pin=0)
```

##### `PinMux.get_runtime_status``(bank, pin)`

Reads mux/gpio owner and current function/group from debugfs pinctrl.

```
import visiong

pm = visiong.PinMux()
result = pm.get_runtime_status(bank=0, pin=0)
```

##### `PinMux.check_conflict``(bank, pin, target_function_or_group=...)`

Checks whether switching this pin may conflict with current mux/gpio owners.

```
import visiong

pm = visiong.PinMux()
result = pm.check_conflict(bank=0, pin=0)
```

##### `PinMux.release_conflict``(bank, pin)`

Attempts to unbind current mux owner device. Returns False if release is incomplete.

```
import visiong

pm = visiong.PinMux()
pm.release_conflict(bank=0, pin=0)
```

##### `PinMux.get_interface_status``(function_or_group)`

Reports whether Linux has exposed usable interfaces (/dev/\* or /sys/class/\*) for the function.

```
import visiong

pm = visiong.PinMux()
result = pm.get_interface_status(function_or_group='uart4')
```

##### `PinMux.ensure_interface``(function_or_group)`

Attempts to bind the inferred owner device and re-check userspace interface visibility.

```
import visiong

pm = visiong.PinMux()
result = pm.ensure_interface(function_or_group='uart4')
```

##### `PinMux.list_overlays``()`

Lists currently active device-tree overlays from configfs.

```
import visiong

pm = visiong.PinMux()
result = pm.list_overlays()
```

##### `PinMux.apply_overlay``(dtbo_path, overlay_name=...)`

Applies a DT overlay (.dtbo) through configfs and returns created overlay entry name.

```
import visiong

pm = visiong.PinMux()
result = pm.apply_overlay(dtbo_path='/path/to/overlay.dtbo')
```

##### `PinMux.remove_overlay``(overlay_name)`

Removes an applied configfs overlay by name.

```
import visiong

pm = visiong.PinMux()
result = pm.remove_overlay(overlay_name='demo')
```

##### `PinMux.list_adc_channels``()`

Lists available SARADC channels from IIO sysfs and reads current values.

```
import visiong

pm = visiong.PinMux()
result = pm.list_adc_channels()
```

##### `PinMux.read_adc``(channel)`

Reads one ADC channel by numeric index.

```
import visiong

pm = visiong.PinMux()
result = pm.read_adc(channel=0)
```

##### `PinMux.gpio_request_line``(bank, pin, config=...)`

Requests one GPIO line with direction/bias/drive options.

```
import visiong

pm = visiong.PinMux()
result = pm.gpio_request_line(bank=0, pin=0)
```

##### `PinMux.gpio_release_line``(bank, pin)`

Releases a previously requested GPIO line.

```
import visiong

pm = visiong.PinMux()
result = pm.gpio_release_line(bank=0, pin=0)
```

##### `PinMux.gpio_set_value``(bank, pin, value)`

Sets value on a requested GPIO output line.

```
import visiong

pm = visiong.PinMux()
result = pm.gpio_set_value(bank=0, pin=0, value=0)
```

##### `PinMux.gpio_get_value``(bank, pin)`

Reads value from a requested GPIO line.

```
import visiong

pm = visiong.PinMux()
result = pm.gpio_get_value(bank=0, pin=0)
```

##### `PinMux.gpio_get_status``(bank, pin)`

Returns runtime status of requested GPIO line.

```
import visiong

pm = visiong.PinMux()
result = pm.gpio_get_status(bank=0, pin=0)
```

##### `PinMux.set_drive_strength``(bank, pin, level)`

Sets RV1106 IOC drive strength level (0..7) for a pin.

```
import visiong

pm = visiong.PinMux()
pm.set_drive_strength(bank=0, pin=0, level=3)
```

##### `PinMux.get_drive_strength``(bank, pin)`

Reads RV1106 IOC drive strength level/raw register for a pin.

```
import visiong

pm = visiong.PinMux()
result = pm.get_drive_strength(bank=0, pin=0)
```

##### `PinMux.set_pull``(bank, pin, mode)`

Sets pull mode (disable/pull\_up/pull\_down/bus\_hold or 0..3).

```
import visiong

pm = visiong.PinMux()
pm.set_pull(bank=0, pin=0, mode='up')
```

##### `PinMux.get_pull``(bank, pin)`

Reads pull mode/raw register for a pin.

```
import visiong

pm = visiong.PinMux()
result = pm.get_pull(bank=0, pin=0)
```

##### `PinMux.set_input_schmitt``(bank, pin, enable)`

Enables/disables input schmitt for a pin.

```
import visiong

pm = visiong.PinMux()
pm.set_input_schmitt(bank=0, pin=0, enable=True)
```

##### `PinMux.get_input_schmitt``(bank, pin)`

Reads input schmitt state/raw register for a pin.

```
import visiong

pm = visiong.PinMux()
result = pm.get_input_schmitt(bank=0, pin=0)
```

##### `PinMux.probe_electrical_capability``(bank, pin, active_test=...)`

Probes drive/pull/schmitt capability for one pin. active\_test=True performs write-restore checks.

```
import visiong

pm = visiong.PinMux()
result = pm.probe_electrical_capability(bank=0, pin=0)
```

##### `PinMux.probe_electrical_capabilities``(active_test=...)`

Probes drive/pull/schmitt capability for all pins.

```
import visiong

pm = visiong.PinMux()
result = pm.probe_electrical_capabilities()
```

##### `PinMux.get_function_name``(bank, pin)`

Returns best-effort function name matching current mux.

```
import visiong

pm = visiong.PinMux()
result = pm.get_function_name(bank=0, pin=0)
```

##### `PinMux.set_function``(bank, pin, function_or_group)`

Sets mux by function name (e.g. 'uart4', 'pwm1') or group name (e.g. 'uart4m1-xfer').

```
import visiong

pm = visiong.PinMux()
pm.set_function(bank=0, pin=0, function_or_group='uart4')
```

NpuClockStatus 0 methods · 8 fields

该类没有公开方法示例。

##### `NpuClockStatus.npu_node_present` · field

readonly\_field

```
import visiong

npuClockStatus = visiong.NpuClockStatus()
value = npuClockStatus.npu_node_present
```

##### `NpuClockStatus.debugfs_available` · field

readonly\_field

```
import visiong

npuClockStatus = visiong.NpuClockStatus()
value = npuClockStatus.debugfs_available
```

##### `NpuClockStatus.overlay_configfs_available` · field

readonly\_field

```
import visiong

npuClockStatus = visiong.NpuClockStatus()
value = npuClockStatus.overlay_configfs_available
```

##### `NpuClockStatus.assigned_rate_hz` · field

readonly\_field

```
import visiong

npuClockStatus = visiong.NpuClockStatus()
value = npuClockStatus.assigned_rate_hz
```

##### `NpuClockStatus.current_rate_hz` · field

readonly\_field

```
import visiong

npuClockStatus = visiong.NpuClockStatus()
value = npuClockStatus.current_rate_hz
```

##### `NpuClockStatus.npu_root_rate_hz` · field

readonly\_field

```
import visiong

npuClockStatus = visiong.NpuClockStatus()
value = npuClockStatus.npu_root_rate_hz
```

##### `NpuClockStatus.clk500m_src_rate_hz` · field

readonly\_field

```
import visiong

npuClockStatus = visiong.NpuClockStatus()
value = npuClockStatus.clk500m_src_rate_hz
```

##### `NpuClockStatus.note` · field

readonly\_field

```
import visiong

npuClockStatus = visiong.NpuClockStatus()
value = npuClockStatus.note
```

NpuClockApplyResult 0 methods · 11 fields

该类没有公开方法示例。

##### `NpuClockApplyResult.ok` · field

readonly\_field

```
import visiong

npuClockApplyResult = visiong.NpuClockApplyResult()
value = npuClockApplyResult.ok
```

##### `NpuClockApplyResult.rebind_attempted` · field

readonly\_field

```
import visiong

npuClockApplyResult = visiong.NpuClockApplyResult()
value = npuClockApplyResult.rebind_attempted
```

##### `NpuClockApplyResult.rebind_ok` · field

readonly\_field

```
import visiong

npuClockApplyResult = visiong.NpuClockApplyResult()
value = npuClockApplyResult.rebind_ok
```

##### `NpuClockApplyResult.reboot_required` · field

readonly\_field

```
import visiong

npuClockApplyResult = visiong.NpuClockApplyResult()
value = npuClockApplyResult.reboot_required
```

##### `NpuClockApplyResult.requested_rate_hz` · field

readonly\_field

```
import visiong

npuClockApplyResult = visiong.NpuClockApplyResult()
value = npuClockApplyResult.requested_rate_hz
```

##### `NpuClockApplyResult.assigned_rate_hz` · field

readonly\_field

```
import visiong

npuClockApplyResult = visiong.NpuClockApplyResult()
value = npuClockApplyResult.assigned_rate_hz
```

##### `NpuClockApplyResult.current_rate_hz` · field

readonly\_field

```
import visiong

npuClockApplyResult = visiong.NpuClockApplyResult()
value = npuClockApplyResult.current_rate_hz
```

##### `NpuClockApplyResult.npu_root_rate_hz` · field

readonly\_field

```
import visiong

npuClockApplyResult = visiong.NpuClockApplyResult()
value = npuClockApplyResult.npu_root_rate_hz
```

##### `NpuClockApplyResult.clk500m_src_rate_hz` · field

readonly\_field

```
import visiong

npuClockApplyResult = visiong.NpuClockApplyResult()
value = npuClockApplyResult.clk500m_src_rate_hz
```

##### `NpuClockApplyResult.overlay_name` · field

readonly\_field

```
import visiong

npuClockApplyResult = visiong.NpuClockApplyResult()
value = npuClockApplyResult.overlay_name
```

##### `NpuClockApplyResult.message` · field

readonly\_field

```
import visiong

npuClockApplyResult = visiong.NpuClockApplyResult()
value = npuClockApplyResult.message
```

NpuClock 8 methods · 0 fields

##### `NpuClock.status``()`

Reads assigned/runtime NPU clock status.

```
import visiong

npu_clock = visiong.NpuClock()
result = npu_clock.status()
```

##### `NpuClock.supported_rates_hz``()`

Returns conservative validated NPU rates in Hz.

```
import visiong

npu_clock = visiong.NpuClock()
result = npu_clock.supported_rates_hz()
```

##### `NpuClock.supported_rates_mhz``()`

Returns conservative validated NPU rates in MHz.

```
import visiong

npu_clock = visiong.NpuClock()
result = npu_clock.supported_rates_mhz()
```

##### `NpuClock.list_overlays``(prefix=...)`

Lists active DT overlays with the given prefix.

```
import visiong

npu_clock = visiong.NpuClock()
result = npu_clock.list_overlays()
```

##### `NpuClock.remove_overlay``(overlay_name)`

Removes one DT overlay by name.

```
import visiong

npu_clock = visiong.NpuClock()
result = npu_clock.remove_overlay(overlay_name='demo')
```

##### `NpuClock.set_rate_hz``(rate_hz, update_cru_clk500m_src=..., unbind_rebind_npu=..., allow_unsafe_rate=...)`

Applies NPU assigned-clock-rates in Hz. Can optionally update CRU CLK\_500M\_SRC and rebind NPU driver.

```
import visiong

npu_clock = visiong.NpuClock()
npu_clock.set_rate_hz(rate_hz=500000000)
```

##### `NpuClock.set_rate_mhz``(rate_mhz, update_cru_clk500m_src=..., unbind_rebind_npu=..., allow_unsafe_rate=...)`

Applies NPU assigned-clock-rates in MHz.

```
import visiong

npu_clock = visiong.NpuClock()
npu_clock.set_rate_mhz(rate_mhz=500)
```

##### `NpuClock.request_reboot``()`

Requests immediate system reboot (sync + reboot).

```
import visiong

npu_clock = visiong.NpuClock()
result = npu_clock.request_reboot()
```

DisplayHTTP 7 methods · 0 fields

##### `DisplayHTTP.stop``()`

Stops the HTTP server and disconnects all clients. This is automatically called when the object is garbage collected.

```
import visiong

http = visiong.DisplayHTTP()
http.stop()
```

##### `DisplayHTTP.display``(img)`

Encodes the ImageBuffer to JPEG (VENC) and pushes the frame to all connected MJPEG clients. In mode='jpg', DisplayHTTP keeps a local JPEG lock and may convert color format or black-pad smaller frames before encoding.

```
import visiong

http = visiong.DisplayHTTP()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
http.display(img=img)
```

##### `DisplayHTTP.is_running``()`

Returns True if the server is currently running.

```
import visiong

http = visiong.DisplayHTTP()
result = http.is_running()
```

##### `DisplayHTTP.set_fps``(fps)`

Sets max FPS. 0 disables limiting.

```
import visiong

http = visiong.DisplayHTTP()
http.set_fps(fps=30)
```

##### `DisplayHTTP.get_fps``()`

Returns current max FPS.

```
import visiong

http = visiong.DisplayHTTP()
result = http.get_fps()
```

##### `DisplayHTTP.set_quality``(quality)`

Sets JPEG quality (1-100).

```
import visiong

http = visiong.DisplayHTTP()
http.set_quality(quality=75)
```

##### `DisplayHTTP.get_quality``()`

Returns current JPEG quality (1-100).

```
import visiong

http = visiong.DisplayHTTP()
result = http.get_quality()
```

DisplayRTSP 11 methods · 0 fields

##### `DisplayRTSP.stop``()`

Stops the RTSP server. This is automatically called when the object is garbage collected.

```
import visiong

rtsp = visiong.DisplayRTSP()
rtsp.stop()
```

##### `DisplayRTSP.set_fps``(fps)`

Sets max frames per second. 0 disables limiting.

```
import visiong

rtsp = visiong.DisplayRTSP()
rtsp.set_fps(fps=30)
```

##### `DisplayRTSP.get_fps``()`

Returns current max frames per second.

```
import visiong

rtsp = visiong.DisplayRTSP()
result = rtsp.get_fps()
```

##### `DisplayRTSP.set_quality``(quality)`

Sets encoding quality (1-100). Takes effect on the next display() call.

```
import visiong

rtsp = visiong.DisplayRTSP()
rtsp.set_quality(quality=75)
```

##### `DisplayRTSP.get_quality``()`

Returns current encoding quality (1-100).

```
import visiong

rtsp = visiong.DisplayRTSP()
result = rtsp.get_quality()
```

##### `DisplayRTSP.set_rc_mode``(rc_mode)`

Sets rate control mode: 'cbr' or 'vbr'. Takes effect on next display() call.

```
import visiong

rtsp = visiong.DisplayRTSP()
rtsp.set_rc_mode(rc_mode='cbr')
```

##### `DisplayRTSP.get_rc_mode``()`

Returns current rate control mode as string ('cbr' or 'vbr').

```
import visiong

rtsp = visiong.DisplayRTSP()
result = rtsp.get_rc_mode()
```

##### `DisplayRTSP.set_logs``(logs)`

Sets logs: 1 enable, 0 suppress.

```
import visiong

rtsp = visiong.DisplayRTSP()
rtsp.set_logs(logs=1)
```

##### `DisplayRTSP.get_logs``()`

Returns 1 if logs are enabled, otherwise 0.

```
import visiong

rtsp = visiong.DisplayRTSP()
result = rtsp.get_logs()
```

##### `DisplayRTSP.display``(img)`

Encodes the ImageBuffer and pushes the frame to all connected RTSP clients.

```
import visiong

rtsp = visiong.DisplayRTSP()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
rtsp.display(img=img)
```

##### `DisplayRTSP.is_running``()`

Returns True if the server is currently running.

```
import visiong

rtsp = visiong.DisplayRTSP()
result = rtsp.is_running()
```

VencRecorder 4 methods · 0 fields

##### `VencRecorder.write``(img)`

Encodes and writes one frame.

```
import visiong

rec = visiong.VencRecorder(filepath='/path/to/demo.jpg')
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
rec.write(img=img)
```

##### `VencRecorder.close``()`

Closes and finalizes the output file (required for MP4).

```
import visiong

rec = visiong.VencRecorder(filepath='/path/to/demo.jpg')
rec.close()
```

##### `VencRecorder.is_open``()`

Returns True if recorder is open.

```
import visiong

rec = visiong.VencRecorder(filepath='/path/to/demo.jpg')
result = rec.is_open()
```

##### `VencRecorder.path``()`

Returns output filepath.

```
import visiong

rec = visiong.VencRecorder(filepath='/path/to/demo.jpg')
result = rec.path()
```

DisplayHTTPFLV 9 methods · 0 fields

##### `DisplayHTTPFLV.stop``()`

Stops the server.

```
import visiong

flv = visiong.DisplayHTTPFLV()
flv.stop()
```

##### `DisplayHTTPFLV.display``(img)`

Encodes and pushes one frame to all connected viewers.

```
import visiong

flv = visiong.DisplayHTTPFLV()
img = visiong.ImageBuffer.create(320, 240, "bgr888", (0, 0, 0))
flv.display(img=img)
```

##### `DisplayHTTPFLV.is_running``()`

Returns True if server is running.

```
import visiong

flv = visiong.DisplayHTTPFLV()
result = flv.is_running()
```

##### `DisplayHTTPFLV.set_fps``(fps)`

Sets max FPS. 0 disables limiting.

```
import visiong

flv = visiong.DisplayHTTPFLV()
flv.set_fps(fps=30)
```

##### `DisplayHTTPFLV.get_fps``()`

Returns current max FPS.

```
import visiong

flv = visiong.DisplayHTTPFLV()
result = flv.get_fps()
```

##### `DisplayHTTPFLV.set_quality``(quality)`

Sets encoding quality 1-100.

```
import visiong

flv = visiong.DisplayHTTPFLV()
flv.set_quality(quality=75)
```

##### `DisplayHTTPFLV.get_quality``()`

Returns current encoding quality.

```
import visiong

flv = visiong.DisplayHTTPFLV()
result = flv.get_quality()
```

##### `DisplayHTTPFLV.set_rc_mode``(rc_mode)`

Sets rate control mode: 'cbr' or 'vbr'. Takes effect on next display() call.

```
import visiong

flv = visiong.DisplayHTTPFLV()
flv.set_rc_mode(rc_mode='cbr')
```

##### `DisplayHTTPFLV.get_rc_mode``()`

Returns current rate control mode as string ('cbr' or 'vbr').

```
import visiong

flv = visiong.DisplayHTTPFLV()
result = flv.get_rc_mode()
```

## 8) 全量 API（逐函数调用模板）

支持搜索类名、函数名、字段名。每个模块函数、类方法、字段都提供了调用模板。

### 8.1 模块级函数

|函数|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`close_venc_recorder`|`(filepath)`|None|Closes a cached MP4 writer for filepath (finalize MP4).|`visiong.close_venc_recorder('demo.mp4')`|
|`close_all_venc_recorders`|`()`|None|Closes all cached MP4 writers (finalize MP4).|`visiong.close_all_venc_recorders()`|
|`get_unique_id`|`()`|对象 / 数值|Reads the unique 6-byte chip ID from the RV1106 OTP area and returns it as a 12-character hex string.|`uid = visiong.get_unique_id()`|
|`decrypt_legacy_value`|`(key)`|对象 / 数值|Decrypts the stored legacy value with a 64-character hex key.|`value = visiong.decrypt_legacy_value(key)`|
|`dma_state_metrics`|`(reset=...)`|dict / 统计对象|Returns dma-buf state-machine counters as a dict. Set reset=True to clear counters after reading.|`metrics = visiong.dma_state_metrics(reset=False)`|
|`dma_state_reset_metrics`|`()`|None|Resets dma-buf state-machine counters.|`visiong.dma_state_reset_metrics()`|
|`dma_state_dump_metrics`|`(output_path=..., reset_after_dump=...)`|字符串 / 文件路径|Returns dma-buf state-machine counters as JSON and optionally writes them to output\_path.|`dump = visiong.dma_state_dump_metrics(output_path='metrics.json')`|

### 8.2 类与方法

Camera 38 methods · 0 properties

无类级描述。

**构造模板：** `cam = visiong.Camera(target_width=640, target_height=360)`

**构造签名：** `(target_width, target_height, format=..., hdr=..., crop_mode=...)`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`init`|`(target_width, target_height, format=..., hdr=..., crop_mode=...)`|对象 / 数值|Initializes the camera. format defaults to 'yuv'. Supported values: 'bgr', 'rgb', 'yuv'/'yuv420', or 'gray'. crop\_mode: 'auto' (default, follows the camera max-resolution aspect ratio), 'off', or any ratio such as '16:9', '4:3', '1:1', o...|`result = cam.init(target_width=640, target_height=360)`|
|`skip`|`(num_frames=...)`|对象 / 数值|(Reads and discards a specified number of frames from the camera.|`result = cam.skip(10)`|
|`snapshot`|`()`|图像 / 数组对象|Captures a single frame from the camera and returns an ImageBuffer.|`result = cam.snapshot()`|
|`release`|`()`|None / 状态码|Releases the camera and frees resources. Safe to call even if not initialized.|`cam.release()`|
|`is_initialized`|`()`|bool|Returns True if the camera has been successfully initialized.|`result = cam.is_initialized()`|
|`get_capture_width`|`()`|对象 / 数值|Returns the actual capture width (alias for actual\_width).|`result = cam.get_capture_width()`|
|`get_capture_height`|`()`|对象 / 数值|Returns the actual capture height (alias for actual\_height).|`result = cam.get_capture_height()`|
|`set_saturation`|`(value)`|None / 状态码|Sets the image saturation. Range: \[0, 255\]. Raises ValueError on invalid input.|`cam.set_saturation(value=0)`|
|`set_contrast`|`(value)`|None / 状态码|Sets the image contrast. Range: \[0, 255\]. Raises ValueError on invalid input.|`cam.set_contrast(value=0)`|
|`set_brightness`|`(value)`|None / 状态码|Sets the image brightness. Range: \[0, 255\]. Raises ValueError on invalid input.|`cam.set_brightness(value=0)`|
|`set_sharpness`|`(value)`|None / 状态码|Sets the image sharpness. Range: \[0, 100\]. Raises ValueError on invalid input.|`cam.set_sharpness(value=0)`|
|`set_hue`|`(value)`|None / 状态码|Sets the image hue. Range: \[0, 255\]. Raises ValueError on invalid input.|`cam.set_hue(value=0)`|
|`set_white_balance_mode`|`(mode)`|None / 状态码|Sets white balance mode ('auto' or 'manual'). Raises ValueError on invalid mode.|`cam.set_white_balance_mode(mode='auto')`|
|`set_white_balance_temperature`|`(temp)`|None / 状态码|Sets white balance color temperature in manual mode. Raises ValueError on invalid input or RuntimeError if not in manual mode.|`cam.set_white_balance_temperature(temp=4500)`|
|`set_exposure_mode`|`(mode)`|None / 状态码|Sets exposure mode ('auto' or 'manual'). Raises ValueError on invalid mode.|`cam.set_exposure_mode(mode='auto')`|
|`set_exposure_time`|`(time_in_seconds)`|None / 状态码|Sets manual exposure time in seconds. Must be positive. Raises RuntimeError if not in manual mode.|`cam.set_exposure_time(time_in_seconds=0.01)`|
|`set_exposure_gain`|`(gain)`|None / 状态码|Sets manual exposure gain. Typical range: \[0, 127\]. Raises ValueError on invalid input or RuntimeError if not in manual mode.|`cam.set_exposure_gain(gain=32)`|
|`set_spatial_denoise_level`|`(level)`|None / 状态码|Sets the spatial (2D) denoise level. Range: \[0, 100\]. Raises ValueError on invalid input.|`cam.set_spatial_denoise_level(level=3)`|
|`set_temporal_denoise_level`|`(level)`|None / 状态码|Sets the temporal (3D) denoise level. Range: \[0, 100\]. Raises ValueError on invalid input.|`cam.set_temporal_denoise_level(level=3)`|
|`set_frame_rate`|`(fps)`|None / 状态码|Sets the camera frame rate. Range: \[10, 60\] (or 0 for auto). Raises ValueError on invalid input.|`cam.set_frame_rate(fps=30)`|
|`set_power_line_frequency`|`(mode)`|None / 状态码|Sets anti-flicker mode ('50hz', '60hz', or 'off'). Raises ValueError on invalid mode.|`cam.set_power_line_frequency(mode='auto')`|
|`set_flip`|`(flip, mirror)`|None / 状态码|Sets image flip (vertical) and mirror (horizontal).|`cam.set_flip(flip=True, mirror=True)`|
|`get_saturation`|`()`|对象 / 数值|Gets the current image saturation (0-255).|`result = cam.get_saturation()`|
|`get_contrast`|`()`|对象 / 数值|Gets the current image contrast (0-255).|`result = cam.get_contrast()`|
|`get_brightness`|`()`|对象 / 数值|Gets the current image brightness (0-255).|`result = cam.get_brightness()`|
|`get_sharpness`|`()`|对象 / 数值|Gets the current image sharpness (0-100).|`result = cam.get_sharpness()`|
|`get_hue`|`()`|对象 / 数值|Gets the current image hue (0-255).|`result = cam.get_hue()`|
|`get_white_balance_mode`|`()`|对象 / 数值|Gets the current white balance mode ('auto' or 'manual').|`result = cam.get_white_balance_mode()`|
|`get_white_balance_temperature`|`()`|对象 / 数值|Gets the current white balance color temperature.|`result = cam.get_white_balance_temperature()`|
|`get_exposure_mode`|`()`|对象 / 数值|Gets the current exposure mode ('auto' or 'manual').|`result = cam.get_exposure_mode()`|
|`get_exposure_time`|`()`|对象 / 数值|Gets the current exposure time in seconds.|`result = cam.get_exposure_time()`|
|`get_exposure_gain`|`()`|对象 / 数值|Gets the current exposure gain.|`result = cam.get_exposure_gain()`|
|`lock_focus`|`()`|对象 / 数值|Locks the autofocus at its current position. Prevents further automatic adjustments.|`result = cam.lock_focus()`|
|`unlock_focus`|`()`|对象 / 数值|Unlocks the autofocus, allowing it to resume its configured mode (e.g., continuous focus).|`result = cam.unlock_focus()`|
|`trigger_focus`|`()`|对象 / 数值|Performs a single, one-shot autofocus search. This is an action, not a mode.|`result = cam.trigger_focus()`|
|`set_focus_mode`|`(mode)`|None / 状态码|Sets the autofocus mode. Supported modes:|`cam.set_focus_mode(mode='continuous')`|
|`set_manual_focus`|`(position)`|None / 状态码|Moves the lens to a specific motor code position. This implicitly sets the mode to 'manual'.|`cam.set_manual_focus(position=512)`|
|`get_focus_position`|`()`|对象 / 数值|Returns the current motor code position of the lens. Returns -1 on failure.|`result = cam.get_focus_position()`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

IVEModel 0 methods · 0 properties

Manages a persistent memory block for stateful IVE algorithms like GMM or background modeling.

**构造模板：** `model = visiong.IVEModel(width=640, height=360)`

**构造签名：** `(width, height, model_size=...)`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|\-|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

MotionVector 1 methods · 3 properties

Holds the result of a Lucas-Kanade optical flow tracking point.

**构造模板：** `mv = visiong.MotionVector()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`__repr__`|`()`|对象 / 数值|<MotionVector status=|`text = repr(mv)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`status`|readonly\_field|`value = mv.status`|
|`mv_x`|readonly\_field|`value = mv.mv_x`|
|`mv_y`|readonly\_field|`value = mv.mv_y`|

IVE 37 methods · 0 properties

Hardware-accelerated image processing using Rockchip IVE.

**构造模板：** `ive = visiong.IVE()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`static set_log_enabled`|`(enabled)`|None / 状态码|Enable or disable IVE internal logs (default off; env VISIONG\_IVE\_LOG=1 enables).|`visiong.IVE.set_log_enabled(enabled=True)`|
|`static is_log_enabled`|`()`|bool|Returns True if IVE internal logs are enabled.|`result = visiong.IVE.is_log_enabled()`|
|`filter`|`(src, mask)`|对象 / 数值|5x5 filter. src: GRAY8. mask: 25 ints. Returns GRAY8 - do not pass to cast\_16bit\_to\_8bit. Use a 5x5 Gaussian mask for blur (no separate gaussian\_filter).|`result = ive.filter(src=img, mask=[[0, -1, 0], [-1, 5, -1], [0, -1, 0]])`|
|`sobel`|`(src, out_ctrl=..., out_format=...)`|对象 / 数值|Sobel edge detection. src: GRAY8. Returns (horizontal\_edges, vertical\_edges). When out\_format=S16C1 each result is S16C1 - unpack and pass to cast\_16bit\_to\_8bit for display; when U8C1 each result is GRAY8.|`result = ive.sobel(src=img)`|
|`canny`|`(src, high_thresh, low_thresh)`|对象 / 数值|Canny edge detection. src must be GRAY8. high\_thresh and low\_thresh typically in 0-255.|`result = ive.canny(src=img, high_thresh=120, low_thresh=40)`|
|`mag_and_ang`|`(src, threshold=..., return_magnitude=...)`|对象 / 数值|Gradient magnitude/angle. return\_magnitude=True -> S16C1 (use cast\_16bit\_to\_8bit for display); False -> U8C1.|`result = ive.mag_and_ang(src=img)`|
|`dilate`|`(src, kernel_size=...)`|对象 / 数值|Performs morphological dilation. \`src\` must be GRAY8. \`kernel\_size\` can be 3 or 5.|`result = ive.dilate(src=img)`|
|`erode`|`(src, kernel_size=...)`|对象 / 数值|Performs morphological erosion. \`src\` must be GRAY8. \`kernel\_size\` can be 3 or 5.|`result = ive.erode(src=img)`|
|`ordered_stat_filter`|`(src, mode)`|对象 / 数值|Performs an ordered statistic filter (Median, Max, Min). \`src\` must be GRAY8.|`result = ive.ordered_stat_filter(src=img, mode=visiong.OrdStatFilterMode.MEDIAN)`|
|`add`|`(src1, src2)`|对象 / 数值|Pixel-wise addition. Returns GRAY8; do not pass to cast\_16bit\_to\_8bit.|`result = ive.add(src1=img1, src2=img2)`|
|`sub`|`(src1, src2, mode=...)`|对象 / 数值|Pixel-wise subtraction. Returns GRAY8; do not pass to cast\_16bit\_to\_8bit. Mode: ABS or SHIFT.|`result = ive.sub(src1=img1, src2=img2)`|
|`logic_op`|`(src1, src2, op)`|对象 / 数值|Performs a pixel-wise logical operation (AND, OR, XOR) on two images.|`result = ive.logic_op(src1=img1, src2=img2, op=visiong.LogicOp.AND)`|
|`threshold`|`(src)`|对象 / 数值|Thresholding on GRAY8. kwargs accept low/low\_thresh, high/high\_thresh, mode. Returns GRAY8; do not pass to cast\_16bit\_to\_8bit.|`result = ive.threshold(src=img)`|
|`cast_16bit_to_8bit`|`(src, mode)`|对象 / 数值|Casts 16-bit (S16C1/U16C1) to 8-bit. Input must be from sobel(S16C1), mag\_and\_ang(magnitude), norm\_grad, or sad (first element); not from filter, add, sub, threshold, gmm.|`result = ive.cast_16bit_to_8bit(src=img, mode=visiong.Cast16to8Mode.S16_TO_U8_ABS)`|
|`hist`|`(src)`|对象 / 数值|Calculates the histogram of a GRAY8 image. Returns a list of 256 integers.|`result = ive.hist(src=img)`|
|`equalize_hist`|`(src)`|对象 / 数值|Performs histogram equalization on a GRAY8 image.|`result = ive.equalize_hist(src=img)`|
|`integral`|`(src, mode=...)`|对象 / 数值|Integral image. mode: COMBINE, SUM, or SQSUM. Output U64C1.|`result = ive.integral(src=img)`|
|`ccl`|`(src, min_area=...)`|对象 / 数值|Connected Components Labeling on a binarized GRAY8 image. Blobs with area < min\_area are filtered. Returns list of Blob.|`result = ive.ccl(src=img)`|
|`ncc`|`(src1, src2)`|对象 / 数值|Normalized cross-correlation. If sizes differ, set auto\_resize=True (default) to resize src2 to src1 size.|`result = ive.ncc(src1=img1, src2=img2)`|
|`csc`|`(src, mode)`|对象 / 数值|Performs Color Space Conversion (e.g., YUV to RGB).|`result = ive.csc(src=img, mode=visiong.CscMode.YUV2RGB_BT601_LIMITED)`|
|`yuv_to_rgb`|`(src)`|对象 / 数值|Converts YUV420SP (or YUV420SP\_VU) to RGB. Use full\_range=True/False, or pass mode=visiong.CscMode.YUV2RGB\_\*.|`result = ive.yuv_to_rgb(src=img)`|
|`yuv_to_hsv`|`(src)`|对象 / 数值|Converts YUV420SP (or YUV420SP\_VU) to HSV. Use full\_range=True/False, or pass mode=visiong.CscMode.YUV2HSV\_\*.|`result = ive.yuv_to_hsv(src=img)`|
|`rgb_to_yuv`|`(src, full_range=...)`|对象 / 数值|Converts RGB/BGR image to YUV420SP.|`result = ive.rgb_to_yuv(src=img)`|
|`rgb_to_hsv`|`(src, full_range=...)`|对象 / 数值|Converts RGB/BGR image to HSV. H:\[0,180\], S:\[0,255\], V:\[0,255\].|`result = ive.rgb_to_hsv(src=img)`|
|`dma`|`(src, mode=...)`|对象 / 数值|Performs a direct memory access operation (e.g., copy).|`result = ive.dma(src=img)`|
|`cast_8bit_to_8bit`|`(src, bias, numerator, denominator)`|对象 / 数值|Scales an 8-bit image using the formula: dst = (src \* numerator / denominator) + bias.|`result = ive.cast_8bit_to_8bit(src=img, bias=0, numerator=1, denominator=1)`|
|`map`|`(src, lut)`|对象 / 数值|Pixel LUT mapping. lut: list of 256 integers in \[0,255\]; output\[x\] = lut\[src\[x\]\].|`result = ive.map(src=img, lut=lut)`|
|`gmm`|`(src, model, first_frame=...)`|对象 / 数值|GMM background subtraction. Returns (foreground, background); both GRAY8 - do not pass to cast\_16bit\_to\_8bit.|`result = ive.gmm(src=img, model=model)`|
|`gmm2`|`(src, factor, model, first_frame=...)`|对象 / 数值|GMM2 with factor image. Returns (foreground, background); both GRAY8.|`result = ive.gmm2(src=img, factor=0.5, model=model)`|
|`lbp`|`(src, abs_mode=..., threshold=...)`|对象 / 数值|Calculates the Local Binary Pattern of an image.|`result = ive.lbp(src=img)`|
|`norm_grad`|`(src)`|对象 / 数值|Normalized gradient. Returns (horizontal\_grad, vertical\_grad); both S16C1 - use cast\_16bit\_to\_8bit for display.|`result = ive.norm_grad(src=img)`|
|`lk_optical_flow`|`(prev_img, next_img, points)`|对象 / 数值|Performs Lucas-Kanade optical flow. points: list of (x,y). Returns list of MotionVector; mv\_x/mv\_y are S9.7 (divide by 128 for pixels). Up to ~500 points.|`result = ive.lk_optical_flow(prev_img=img, next_img=img, points=points)`|
|`st_corner`|`(src, max_corners=..., min_dist=..., quality_level=...)`|对象 / 数值|Performs Shi-Tomasi corner detection. Returns a list of (x, y) corner tuples.|`result = ive.st_corner(src=img)`|
|`match_bg_model`|`(current_img, bg_model, frame_num)`|对象 / 数值|Matches the current image against a background model. Returns a foreground flag image.|`result = ive.match_bg_model(current_img=img, bg_model=img2, frame_num=img)`|
|`update_bg_model`|`(current_img, fg_flag, bg_model, frame_num)`|对象 / 数值|Updates the background model. Returns the new background image.|`result = ive.update_bg_model(current_img=img, fg_flag=img2, bg_model=img2, frame_num=img)`|
|`sad`|`(src1, src2, mode, threshold, min_val=..., max_val=...)`|对象 / 数值|Sum of Absolute Differences. Returns (sad\_image U16C1, threshold\_image U8C1); cast sad\_image for 8-bit display.|`result = ive.sad(src1=img1, src2=img2, mode=visiong.SadMode.MB_8X8, threshold=120)`|
|`create_pyramid`|`(src, levels)`|对象 / 数值|Builds an image pyramid with levels levels. Returns list of ImageBuffer from full size down.|`result = ive.create_pyramid(src=img, levels=3)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

Blob 1 methods · 0 properties

Connected component from find\_blobs or IVE ccl: bounding box (x,y,w,h), center (cx,cy), pixel count, optional label code.

**构造模板：** `blob = visiong.Blob(x=10, y=10, w=120, h=90, cx=160, cy=120, pixels=0)`

**构造签名：** `(x, y, w, h, cx, cy, pixels, code=...)`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`__repr__`|`()`|对象 / 数值|<Blob rect=(|`text = repr(blob)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

Line 1 methods · 7 properties

Line segment from find\_lines: endpoints (x1,y1)-(x2,y2), magnitude, theta (angle), rho.

**构造模板：** `line = visiong.Line()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`__repr__`|`()`|对象 / 数值|\-|`text = repr(line)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`x1`|readonly\_field|`value = line.x1`|
|`y1`|readonly\_field|`value = line.y1`|
|`x2`|readonly\_field|`value = line.x2`|
|`y2`|readonly\_field|`value = line.y2`|
|`magnitude`|readonly\_field|`value = line.magnitude`|
|`theta`|readonly\_field|`value = line.theta`|
|`rho`|readonly\_field|`value = line.rho`|

Circle 1 methods · 4 properties

Circle from find\_circles: center (cx,cy), radius r, magnitude.

**构造模板：** `circle = visiong.Circle()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`__repr__`|`()`|对象 / 数值|<Circle center=(|`text = repr(circle)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`cx`|readonly\_field|`value = circle.cx`|
|`cy`|readonly\_field|`value = circle.cy`|
|`r`|readonly\_field|`value = circle.r`|
|`magnitude`|readonly\_field|`value = circle.magnitude`|

QRCode 1 methods · 2 properties

无类级描述。

**构造模板：** `qRCode = visiong.QRCode()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`__repr__`|`()`|对象 / 数值|from\_numpy\_zero\_copy: width and height must both be even for zero-copy hardware-friendly ImageBuffer wrapping.|`text = repr(qRCode)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`corners`|property\_readonly|`value = qRCode.corners`|
|`payload`|property\_readonly|`value = qRCode.payload`|

ImageBuffer 39 methods · 0 properties

无类级描述。

**构造模板：** `img = visiong.ImageBuffer()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`static create`|`(width, height, format, color=...)`|对象 / 数值|Creates a new ImageBuffer filled with the given color. format: e.g. 'rgb888', 'bgr888', 'gray8'. color: (R,G,B) or (R,G,B,A).|`result = visiong.ImageBuffer.create(width=640, height=360, format='bgr888')`|
|`__array__`|`(copy=..., dtype=...)`|对象 / 数值|Allows NumPy conversion via np.asarray(img). copy=False returns a read-only array pinned to the ImageBuffer lifetime; color exports use a BGR-shaped view over the current CPU-readable storage, which may be a cached converted backing stor...|`arr = np.asarray(img)`|
|`to_numpy`|`(copy=...)`|图像 / 数组对象|Returns a NumPy array: 2D (H,W) for grayscale, 3D (H,W,3) BGR-shaped for color. copy=False returns a read-only lifetime-pinned view over the current CPU-readable storage.|`result = img.to_numpy(copy=True)`|
|`numpy_view`|`()`|对象 / 数值|Returns a read-only lifetime-pinned NumPy view over the current CPU-readable storage.|`result = img.numpy_view()`|
|`static from_numpy`|`(array, format=..., copy=...)`|图像 / 数组对象|Creates an ImageBuffer from a NumPy array. format='auto' infers: 2D or 1-channel -> GRAY8, 3-channel -> BGR888, 4-channel -> BGRA8888. copy=False uses zero-copy when the input is uint8, C-contiguous, and has even width/height.|`result = visiong.ImageBuffer.from_numpy(array=arr)`|
|`static from_numpy_zero_copy`|`(array, format=...)`|图像 / 数组对象|Strict zero-copy import from a NumPy array. Requires dtype=uint8, C-contiguous layout, and even width/height; otherwise raises instead of silently copying.|`result = visiong.ImageBuffer.from_numpy_zero_copy(array=arr)`|
|`is_valid`|`()`|bool|Returns True if the buffer holds valid image data.|`result = img.is_valid()`|
|`copy`|`()`|图像 / 数组对象|Creates and returns a deep copy of the image buffer.|`result = img.copy()`|
|`static load`|`(filepath)`|图像 / 数组对象|Loads an image from a file into a new ImageBuffer. Supports JPEG, PNG, and BMP (via stb\_image).|`result = visiong.ImageBuffer.load(filepath='/path/to/demo.jpg')`|
|`save`|`(filepath, quality=...)`|对象 / 数值|Saves the ImageBuffer to a file. Supports JPEG, PNG, and BMP. Quality (1-100) applies to JPEG/PNG. Uses software encoding (stb\_image).|`result = img.save(filepath='/path/to/demo.jpg')`|
|`save_hsv_bin`|`(filepath)`|对象 / 数值|Converts YUV420SP image to HSV using IVE hardware and saves raw HSV binary data to the given filepath. Input must be YUV420SP or YUV420SP\_VU.|`result = img.save_hsv_bin(filepath='/path/to/demo.jpg')`|
|`save_venc_jpg`|`(filepath, quality=...)`|对象 / 数值|it checks if size and format match; otherwise, it auto-initializes the encoder.|`result = img.save_venc_jpg(filepath='/path/to/demo.jpg')`|
|`save_venc_h264`|`(filepath, quality=..., rc_mode=..., fps=..., append=..., container=..., mp4_faststart=...)`|对象 / 数值|it muxes into MP4 and caches the writer until close\_venc\_recorder/close\_all\_venc\_recorders or process exit.|`result = img.save_venc_h264(filepath='/path/to/demo.jpg')`|
|`save_venc_h265`|`(filepath, quality=..., rc_mode=..., fps=..., append=..., container=..., mp4_faststart=...)`|对象 / 数值|it muxes into MP4 and caches the writer until close\_venc\_recorder/close\_all\_venc\_recorders or process exit.|`result = img.save_venc_h265(filepath='/path/to/demo.jpg')`|
|`to_format`|`(new_format)`|图像 / 数组对象|Converts the image to the given pixel format string (e.g. 'rgb888', 'bgr888', 'gray8', 'yuv420sp').|`result = img.to_format(new_format='rgb')`|
|`to_grayscale`|`()`|图像 / 数组对象|Converts the image to GRAY8. Uses RGA for color images.|`result = img.to_grayscale()`|
|`resize`|`(new_width, new_height)`|图像 / 数组对象|Resizes the image to new\_width x new\_height using RGA.|`result = img.resize(new_width=320, new_height=240)`|
|`crop` ×2|`(x, y, w, h)`|图像 / 数组对象|Crops to the region (x, y, w, h). rect\_tuple: (x, y, w, h). Uses RGA.|`result = img.crop(x=10, y=10, w=120, h=90)`|
|`rotate`|`(angle_degrees)`|图像 / 数组对象|Rotates the image by 90, 180, or 270 degrees using hardware acceleration.|`result = img.rotate(angle_degrees=90)`|
|`flip`|`(horizontal, vertical)`|图像 / 数组对象|Flips the image horizontally and/or vertically using hardware acceleration.|`result = img.flip(horizontal=True, vertical=False)`|
|`find_blobs` ×2|`(thresholds, invert=..., roi=..., x_stride=..., y_stride=..., area_threshold=..., pixels_threshold=..., merge=..., margin=..., mode=..., erode_size=..., dilate_size=...)`|结果列表 / 检测对象|(Finds blobs by color thresholds. thresholds: list of 6-tuples (H\_min,H\_max,S\_min,S\_max,V\_min,V\_max) for HSV; mode 0=HSV, 1=LAB. For grayscale use the overload with \[(gray\_min, gray\_max)\].)|`result = img.find_blobs(thresholds=[(0, 80)])`|
|`find_lines` ×2|`(x, y, w, h, x_stride=..., y_stride=..., threshold=..., rho_resolution_px=..., theta_resolution_deg=..., canny_low_thresh=..., canny_high_thresh=...)`|结果列表 / 检测对象|theta\_resolution\_deg|`result = img.find_lines(x=10, y=10, w=120, h=90)`|
|`find_circles` ×2|`(x, y, w, h, x_stride=..., y_stride=..., threshold=..., r_min=..., r_max=..., r_step=..., canny_low_thresh=..., canny_high_thresh=...)`|结果列表 / 检测对象|canny\_high\_thresh|`result = img.find_circles(x=10, y=10, w=120, h=90)`|
|`find_polygons` ×2|`(x, y, w, h, min_area=..., max_area=..., min_sides=..., max_sides=..., accuracy=...)`|结果列表 / 检测对象|accuracy must be 'fast', 'normal', or 'accurate'.|`result = img.find_polygons(x=10, y=10, w=120, h=90)`|
|`find_qrcodes`|`()`|结果列表 / 检测对象|Finds and decodes QR codes in the image. The image is automatically converted to grayscale if needed.|`result = img.find_qrcodes()`|
|`find_squares`|`(roi=..., threshold_val, min_area=..., approx_epsilon=..., corner_sample_radius=..., corner_ratio_thresh=..., edge_check_offset=..., area_sample_points=..., area_white_thresh=..., area_morph_close_kernel_size=..., duplicate_center_thresh=..., duplicate_area_thresh=...)`|结果列表 / 检测对象|(Finds squares in the image using a robust corner-based algorithm.|`result = img.find_squares(threshold_val=120)`|
|`binarize`|`(method=..., threshold_range=..., invert=..., adaptive_block_size=..., adaptive_c=..., pre_blur_kernel_size=..., post_morph_kernel_size=...)`|对象 / 数值|(Performs image binarization with adjustable denoising strength.|`result = img.binarize(method='otsu')`|
|`warp_perspective`|`(quad, out_width, out_height)`|对象 / 数值|(Performs a perspective warp transformation.|`result = img.warp_perspective(quad=[(0, 0), (319, 0), (319, 239), (0, 239)], out_width=320, out_height=240)`|
|`letterbox`|`(target_width, target_height, color=...)`|对象 / 数值|Scales the image to fit inside target dimensions while preserving aspect ratio, then pads with color to fill target\_width x target\_height. Uses RGA.|`result = img.letterbox(target_width=640, target_height=360)`|
|`draw_line`|`(x0, y0, x1, y1, color=..., thickness=...)`|None / 状态码|Draws a line on the image in-place and returns itself.|`img.draw_line(x0=10, y0=10, x1=120, y1=90)`|
|`draw_rectangle` ×2|`(x, y, w, h, color=..., thickness=..., fill=...)`|None / 状态码|Draws a rectangle on the image in-place and returns itself.|`img.draw_rectangle(x=10, y=10, w=120, h=90)`|
|`draw_circle`|`(cx, cy, radius, color=..., thickness=..., fill=...)`|None / 状态码|Draws a circle on the image in-place and returns itself.|`img.draw_circle(cx=160, cy=120, radius=30)`|
|`draw_string`|`(x, y, text, color=..., scale=..., thickness=...)`|None / 状态码|Draws text at (x,y). color (R,G,B). scale and thickness affect size. In-place, returns self.|`img.draw_string(x=10, y=10, text='hello visiong')`|
|`static set_text_font`|`(font_path=..., predefine_chars=..., glyph_budget=...)`|None / 状态码|Configures the shared UTF-8 font used by draw\_string (e.g. for Chinese).|`visiong.ImageBuffer.set_text_font(value)`|
|`static clear_text_font`|`()`|对象 / 数值|Clears draw\_string shared font configuration.|`visiong.ImageBuffer.clear_text_font()`|
|`draw_cross`|`(cx, cy, color=..., size=..., thickness=...)`|None / 状态码|Draws a cross on the image in-place and returns itself.|`img.draw_cross(cx=160, cy=120)`|
|`paste`|`(img_to_paste, x, y)`|对象 / 数值|Pastes another image onto this one at the specified (x, y) coordinates.|`result = img.paste(img_to_paste=img2, x=10, y=10)`|
|`blend`|`(img_to_blend, x=..., y=...)`|对象 / 数值|Blends an RGBA image onto this image using its alpha channel. This is a CPU operation.|`result = img.blend(img_to_blend=img2)`|
|`__repr__`|`()`|对象 / 数值|<ImageBuffer|`text = repr(img)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

DisplayUDP 4 methods · 0 properties

无类级描述。

**构造模板：** `udp = visiong.DisplayUDP()`

**构造签名：** `(udp_ip=..., udp_port=..., jpeg_quality=...)`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`init`|`(ip_address, port, jpeg_quality=...)`|对象 / 数值|Initializes or re-initializes the UDP sender (target IP, port, JPEG quality 1-100).|`result = udp.init(ip_address='127.0.0.1', port=8080)`|
|`display`|`(img_buf)`|None / 状态码|Encodes the ImageBuffer to JPEG (VENC) and sends it via UDP to the configured address. The DisplayUDP-local lock may convert color format and black-pad smaller frames before encoding.|`udp.display(img_buf=img)`|
|`release`|`()`|None / 状态码|Releases DisplayUDP resources.|`udp.release()`|
|`is_initialized`|`()`|bool|Checks if DisplayUDP is initialized.|`result = udp.is_initialized()`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

TouchPoint 1 methods · 2 properties

Represents a single touch coordinate.

**构造模板：** `touchPoint = visiong.TouchPoint()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`__repr__`|`()`|对象 / 数值|<TouchPoint x=|`text = repr(touchPoint)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`x`|readonly\_field|`value = touchPoint.x`|
|`y`|readonly\_field|`value = touchPoint.y`|

Touch 4 methods · 0 properties

Interface for I2C touch screen devices.

**构造模板：** `touch = visiong.Touch()`

**构造签名：** `(chip_model=..., i2c_bus=..., original_width=..., original_height=..., rotation_degrees=...)`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`release`|`()`|None / 状态码|Releases the touch device (closes I2C).|`touch.release()`|
|`is_pressed`|`()`|bool|Returns True if at least one finger is on the screen.|`result = touch.is_pressed()`|
|`read`|`()`|对象 / 数值|Reads all active touch points and returns a list of TouchPoint objects.|`result = touch.read()`|
|`configure_geometry`|`(original_width, original_height, rotation_degrees)`|对象 / 数值|Re-configures the screen geometry and coordinate rotation at runtime.|`result = touch.configure_geometry(original_width=640, original_height=360, rotation_degrees=270)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

DisplayFB 4 methods · 0 properties

Framebuffer display. mode: 'high' (default) or 'low' refresh.

**构造模板：** `fb = visiong.DisplayFB()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`display` ×2|`(img_buf, roi)`|None / 状态码|Displays the full image on the framebuffer (non-blocking). Returns True on success.|`fb.display(img_buf=img, roi=(0, 0, 320, 240))`|
|`release`|`()`|None / 状态码|Releases framebuffer resources.|`fb.release()`|
|`is_initialized`|`()`|bool|Returns True if the framebuffer is initialized.|`result = fb.is_initialized()`|
|`__repr__`|`()`|对象 / 数值|DisplayFB(screen\_width={}, screen\_height={})|`text = repr(fb)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

Detection 1 methods · 7 properties

Single detection from YOLOv5/RetinaFace/YOLO11/YOLO11\_SEG/YOLO11\_POSE inference.

**构造模板：** `detection = visiong.Detection()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`__repr__`|`()`|对象 / 数值|<Detection label='|`text = repr(detection)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`box`|readonly\_field|`value = detection.box`|
|`score`|readonly\_field|`value = detection.score`|
|`class_id`|readonly\_field|`value = detection.class_id`|
|`label`|readonly\_field|`value = detection.label`|
|`landmarks`|readonly\_field|`value = detection.landmarks`|
|`keypoints`|readonly\_field|`value = detection.keypoints`|
|`mask_points`|readonly\_field|`value = detection.mask_points`|

NPU 5 methods · 0 properties

无类级描述。

**构造模板：** `npu = visiong.NPU(model_type='yolov5', model_path='/path/to/model.rknn')`

**构造签名：** `(model_type, model_path, label_path=..., box=..., nms=...)`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`infer`|`(img_buf, roi=..., model_format=...)`|结果列表 / 检测对象|Runs inference. For detection/pose models (YOLOv5, RetinaFace, YOLO11, YOLO11\_SEG, YOLO11\_POSE) returns a list of Detection.|`result = npu.infer(img_buf=img)`|
|`get_face_feature`|`(face_image)`|对象 / 数值|Extracts a 128-dimensional feature vector from a cropped face image. Requires FACENET model\_type; raises RuntimeError otherwise.|`result = npu.get_face_feature(face_image=img)`|
|`recognize_plate`|`(plate_image)`|对象 / 数值|Recognizes a license plate from a cropped image. For use with LPRNET models. Returns a string.|`result = npu.recognize_plate(plate_image=img)`|
|`static get_feature_distance`|`(feature1, feature2)`|对象 / 数值|Euclidean distance between two 128-D face feature vectors. Returns 100.0 if either length is not 128.|`result = visiong.NPU.get_feature_distance(feature1=feature1, feature2=feature2)`|
|`is_initialized`|`()`|bool|Checks if the NPU is initialized.|`result = npu.is_initialized()`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

LowLevelTensorInfo 0 methods · 14 properties

Low-level RKNN tensor metadata.

**构造模板：** `lowLevelTensorInfo = visiong.LowLevelTensorInfo()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|\-|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`index`|readonly\_field|`value = lowLevelTensorInfo.index`|
|`name`|readonly\_field|`value = lowLevelTensorInfo.name`|
|`dims`|readonly\_field|`value = lowLevelTensorInfo.dims`|
|`format`|readonly\_field|`value = lowLevelTensorInfo.format`|
|`type`|readonly\_field|`value = lowLevelTensorInfo.type`|
|`quant_type`|readonly\_field|`value = lowLevelTensorInfo.quant_type`|
|`zero_point`|readonly\_field|`value = lowLevelTensorInfo.zero_point`|
|`scale`|readonly\_field|`value = lowLevelTensorInfo.scale`|
|`num_elements`|readonly\_field|`value = lowLevelTensorInfo.num_elements`|
|`size_bytes`|readonly\_field|`value = lowLevelTensorInfo.size_bytes`|
|`size_with_stride_bytes`|readonly\_field|`value = lowLevelTensorInfo.size_with_stride_bytes`|
|`w_stride`|readonly\_field|`value = lowLevelTensorInfo.w_stride`|
|`h_stride`|readonly\_field|`value = lowLevelTensorInfo.h_stride`|
|`pass_through`|readonly\_field|`value = lowLevelTensorInfo.pass_through`|

LowLevelNPU 26 methods · 0 properties

Low-level RKNN runtime wrapper for teaching and custom tensor IO.

**构造模板：** `llnpu = visiong.LowLevelNPU(model_path='/path/to/model.rknn')`

**构造签名：** `(model_path, init_flags=...)`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`is_initialized`|`()`|bool|is\_initialized|`result = llnpu.is_initialized()`|
|`num_inputs`|`()`|对象 / 数值|\-|`result = llnpu.num_inputs()`|
|`num_outputs`|`()`|对象 / 数值|\-|`result = llnpu.num_outputs()`|
|`input_tensors`|`()`|对象 / 数值|input\_tensors|`result = llnpu.input_tensors()`|
|`output_tensors`|`()`|对象 / 数值|output\_tensors|`result = llnpu.output_tensors()`|
|`input_tensor`|`(index)`|对象 / 数值|input\_tensor|`result = llnpu.input_tensor(index=0)`|
|`output_tensor`|`(index)`|对象 / 数值|output\_tensor|`result = llnpu.output_tensor(index=0)`|
|`input_shape`|`(index)`|对象 / 数值|\-|`result = llnpu.input_shape(index=0)`|
|`output_shape`|`(index)`|对象 / 数值|output\_shape|`result = llnpu.output_shape(index=0)`|
|`sdk_versions`|`()`|对象 / 数值|sdk\_versions|`result = llnpu.sdk_versions()`|
|`set_core_mask`|`(core_mask)`|None / 状态码|Sets RKNN core mask. Accepts: 'auto', '0', '1', '2', '0\_1', '0\_1\_2'.|`llnpu.set_core_mask(core_mask='auto')`|
|`set_input_attr`|`(index, tensor_type, tensor_format, pass_through)`|None / 状态码|Rebinds one input tensor attr. Example: set\_input\_attr(0, 'uint8', 'nhwc', False).|`llnpu.set_input_attr(index=0, tensor_type=0, tensor_format='rgb', pass_through=True)`|
|`reset_input_attr`|`(index)`|对象 / 数值|Resets an input tensor attr to its startup value.|`result = llnpu.reset_input_attr(index=0)`|
|`set_input_bytes`|`(index, payload, zero_pad=..., sync_to_device=...)`|None / 状态码|Writes raw bytes into an input tensor buffer.|`llnpu.set_input_bytes(index=0, payload='payload')`|
|`set_input_array`|`(index, array, quantize_if_needed=..., zero_pad=..., sync_to_device=...)`|None / 状态码|Writes a numpy array into input tensor memory. Float arrays can be auto-quantized.|`llnpu.set_input_array(index=0, array=arr)`|
|`set_input_image`|`(index, image, color_order=..., keep_aspect=..., pad_value=..., driver_convert=...)`|None / 状态码|Writes ImageBuffer into input tensor memory using RGA path when possible.|`llnpu.set_input_image(index=0, image=img)`|
|`sync_input_to_device`|`(index)`|对象 / 数值|sync\_input\_to\_device|`result = llnpu.sync_input_to_device(index=0)`|
|`sync_output_from_device`|`(index)`|对象 / 数值|sync\_output\_from\_device|`result = llnpu.sync_output_from_device(index=0)`|
|`sync_all_outputs_from_device`|`()`|对象 / 数值|sync\_all\_outputs\_from\_device|`result = llnpu.sync_all_outputs_from_device()`|
|`run`|`(sync_outputs=..., non_block=..., timeout_ms=...)`|执行结果|Runs RKNN. Optional sync\_outputs controls output cache sync.|`result = llnpu.run(sync_outputs=True)`|
|`wait`|`(timeout_ms=...)`|执行结果|\-|`result = llnpu.wait(timeout_ms=0)`|
|`output_bytes`|`(index, with_stride=..., sync_from_device=...)`|对象 / 数值|Returns raw output bytes.|`result = llnpu.output_bytes(index=0)`|
|`output_float`|`(index, dequantize_if_needed=..., sync_from_device=...)`|对象 / 数值|Returns output tensor as float32 numpy array (dequantized when possible).|`result = llnpu.output_float(index=0)`|
|`output_array`|`(index, dequantize_if_needed=..., sync_from_device=...)`|对象 / 数值|Returns output as float array (default) or raw uint8 vector when dequantize\_if\_needed=False.|`result = llnpu.output_array(index=0)`|
|`input_dma_fd`|`(index)`|对象 / 数值|input\_dma\_fd|`result = llnpu.input_dma_fd(index=0)`|
|`output_dma_fd`|`(index)`|对象 / 数值|output\_dma\_fd|`result = llnpu.output_dma_fd(index=0)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

OCRResult 1 methods · 5 properties

Single OCR result with text and quadrilateral location.

**构造模板：** `oCRResult = visiong.OCRResult()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`__repr__`|`()`|对象 / 数值|<OCRResult text='|`text = repr(oCRResult)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`quad`|readonly\_field|`value = oCRResult.quad`|
|`rect`|readonly\_field|`value = oCRResult.rect`|
|`det_score`|readonly\_field|`value = oCRResult.det_score`|
|`text`|readonly\_field|`value = oCRResult.text`|
|`text_score`|readonly\_field|`value = oCRResult.text_score`|

PPOCR 2 methods · 0 properties

无类级描述。

**构造模板：** `ocr = visiong.PPOCR(det_model_path='/path/to/det.rknn', rec_model_path='/path/to/rec.rknn')`

**构造签名：** `(det_model_path, rec_model_path, dict_path=..., det_threshold=..., box_threshold=..., use_dilate=..., rec_fast_model_path=..., rec_fast_max_ratio=..., rec_fast_enable_fallback=..., rec_fast_fallback_score_thresh=..., model_input_format=..., det_unclip_ratio=...)`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`infer`|`(img_buf)`|结果列表 / 检测对象|Runs DET+REC OCR on one image and returns a list of OCRResult.|`result = ocr.infer(img_buf=img)`|
|`is_initialized`|`()`|bool|Checks whether PPOCR runtime is initialized.|`result = ocr.is_initialized()`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

NanoTrackResult 0 methods · 2 properties

Single frame tracking output from NanoTrack.

**构造模板：** `nanoTrackResult = visiong.NanoTrackResult()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|\-|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`box`|readonly\_field|`value = nanoTrackResult.box`|
|`score`|readonly\_field|`value = nanoTrackResult.score`|

NanoTrack 4 methods · 0 properties

无类级描述。

**构造模板：** `tracker = visiong.NanoTrack(template_model=0, search_model=0, head_model='/path/to/head.rknn')`

**构造签名：** `(template_model, search_model, head_model)`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`init`|`(img_buf, bbox)`|对象 / 数值|Initializes tracker state with the first frame and initial bbox (x, y, w, h).|`result = tracker.init(img_buf=img, bbox=(60, 40, 120, 90))`|
|`track`|`(img_buf)`|结果列表 / 检测对象|Tracks target on a new frame and returns NanoTrackResult.|`result = tracker.track(img_buf=img)`|
|`is_initialized`|`()`|bool|Returns True after init() succeeds.|`result = tracker.is_initialized()`|
|`reset`|`()`|对象 / 数值|Clears tracker state. You must call init() again before track().|`result = tracker.reset()`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

Canvas 0 methods · 0 properties

An opaque handle to a Nuklear drawing canvas.

**构造模板：** `canvas = visiong.Canvas()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|\-|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

GUI 63 methods · 0 properties

Nuklear GUI Manager

**构造模板：** `gui = visiong.GUI(width=640, height=360)`

**构造签名：** `(width, height, font=..., predefine_chars=...)`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`begin_frame`|`(touch_device)`|对象 / 数值|Starts a new frame. Pass Touch device or None. Call before any widgets.|`result = gui.begin_frame(touch_device=None)`|
|`begin_window`|`(title, x, y, w, h, flags=...)`|对象 / 数值|Begins a window. x,y,w,h are floats. Returns True if the window is visible.|`result = gui.begin_window(title='Demo', x=10, y=10, w=120, h=90)`|
|`end_window`|`()`|对象 / 数值|Ends the current window.|`result = gui.end_window()`|
|`end_frame`|`(target_image)`|对象 / 数值|Ends the frame and renders the GUI into the given ImageBuffer.|`result = gui.end_frame(target_image=img)`|
|`layout_row_dynamic`|`(height, cols=...)`|对象 / 数值|Starts a row with dynamic column widths. cols: number of widgets in the row.|`gui.layout_row_dynamic(height=360)`|
|`layout_row_static`|`(height, item_width, cols)`|对象 / 数值|Starts a row with fixed widget width.|`gui.layout_row_static(height=360, item_width=640, cols=1)`|
|`layout_row_begin`|`(format, row_height, cols)`|对象 / 数值|Begins a row with custom format (e.g. percentage). Use layout\_row\_push then layout\_row\_end.|`gui.layout_row_begin(format='bgr888', row_height=360, cols=1)`|
|`layout_row_push`|`(value)`|对象 / 数值|Pushes a column size (after layout\_row\_begin).|`gui.layout_row_push(value=0)`|
|`layout_row_end`|`()`|对象 / 数值|Ends the row started by layout\_row\_begin.|`gui.layout_row_end()`|
|`group_begin`|`(title, flags=...)`|对象 / 数值|Begins a group. Returns True if the group is visible. Must call group\_end after.|`gui.group_begin(title='Demo')`|
|`group_end`|`()`|对象 / 数值|Ends the current group.|`gui.group_end()`|
|`label`|`(text, align=...)`|对象 / 数值|Draws a label. align: 'left', 'right', 'center'.|`result = gui.label(text='hello visiong')`|
|`label_wrap`|`(text)`|对象 / 数值|Draws a label with word wrapping.|`result = gui.label_wrap(text='hello visiong')`|
|`button`|`(label)`|对象 / 数值|Returns True if the button was clicked.|`result = gui.button(label='object')`|
|`slider`|`(label, value, min, max, step)`|对象 / 数值|Slider widget. Touch interaction is relative-drag based: press anywhere in range, then slide horizontally to adjust.|`result = gui.slider(label='object', value=0, min=0, max=100, step=0)`|
|`checkbox`|`(label, is_checked)`|对象 / 数值|Checkbox. Returns the new checked state (bool).|`result = gui.checkbox(label='object', is_checked=True)`|
|`option`|`(label, is_active)`|对象 / 数值|Radio option. Returns True if this option is selected.|`result = gui.option(label='object', is_active=True)`|
|`edit_string`|`(text, max_len=...)`|对象 / 数值|Single-line text edit. Returns (changed: bool, text: str).|`result = gui.edit_string(text='hello visiong')`|
|`progress`|`(current, max, is_modifyable=...)`|对象 / 数值|Progress bar. If modifiable, touch interaction is relative-drag based rather than absolute jump-to-position.|`result = gui.progress(current=30, max=100)`|
|`button_image`|`(image)`|对象 / 数值|Creates a clickable button from an ImageBuffer. Returns True if clicked.|`result = gui.button_image(image=img)`|
|`tree_node`|`(title, is_expanded)`|对象 / 数值|Begins a tree node. Returns True if expanded. Call tree\_pop when done with children.|`result = gui.tree_node(title='Demo', is_expanded=True)`|
|`tree_pop`|`()`|对象 / 数值|Ends the current tree node.|`result = gui.tree_pop()`|
|`property_int`|`(name, value, min, max, step, inc_per_pixel=...)`|对象 / 数值|Integer property control. Touch drag adjusts by horizontal delta instead of jumping to an absolute position.|`result = gui.property_int(name='demo', value=0, min=0, max=100, step=0)`|
|`property_float`|`(name, value, min, max, step, inc_per_pixel=...)`|对象 / 数值|Float property control. Touch drag adjusts by horizontal delta instead of jumping to an absolute position.|`result = gui.property_float(name='demo', value=0, min=0, max=100, step=0)`|
|`combo_begin`|`(text, width, height)`|对象 / 数值|Begins a combo box. Popup placement is touch-first and constrained within the parent window.|`result = gui.combo_begin(text='hello visiong', width=640, height=360)`|
|`combo_item`|`(text)`|对象 / 数值|Adds an item to the current combo. Returns True if selected.|`result = gui.combo_item(text='hello visiong')`|
|`combo_end`|`()`|对象 / 数值|Ends the combo box.|`result = gui.combo_end()`|
|`contextual_begin`|`(width, height)`|对象 / 数值|Begins a contextual menu. On touch devices this is typically opened via long press, with popup placement constrained to the parent window.|`result = gui.contextual_begin(width=640, height=360)`|
|`contextual_item`|`(text)`|对象 / 数值|Adds an item to the contextual menu. Returns True if clicked.|`result = gui.contextual_item(text='hello visiong')`|
|`contextual_end`|`()`|对象 / 数值|Ends the contextual menu.|`result = gui.contextual_end()`|
|`chart_begin`|`(type, count, min_val, max_val)`|对象 / 数值|Begins a chart section. Type can be 'lines' or 'columns'.|`result = gui.chart_begin(type='lines', count=0, min_val=0, max_val=100)`|
|`chart_push`|`(value)`|对象 / 数值|Pushes a new value to the active chart.|`result = gui.chart_push(value=0)`|
|`chart_end`|`()`|对象 / 数值|Ends the chart section.|`result = gui.chart_end()`|
|`menubar_begin`|`()`|对象 / 数值|Begins a menubar at the top of the current window.|`result = gui.menubar_begin()`|
|`menubar_end`|`()`|对象 / 数值|Ends the menubar section.|`result = gui.menubar_end()`|
|`menu_begin`|`(label, width, height)`|对象 / 数值|Begins a dropdown menu.|`gui.menu_begin(label='object', width=640, height=360)`|
|`input_is_pointer_down_in_rect`|`(rect, primary_pointer=...)`|对象 / 数值|Touch-first alias of input\_is\_mouse\_down\_in\_rect. Returns True if the primary pointer is held inside the rect.|`result = gui.input_is_pointer_down_in_rect(rect=(60, 40, 120, 90))`|
|`input_is_pointer_dragging_in_rect`|`()`|对象 / 数值|Touch-first alias of input\_is\_mouse\_dragging\_in\_rect. Returns drag state and delta, including momentum when active.|`result = gui.input_is_pointer_dragging_in_rect()`|
|`is_title_bar_active`|`()`|bool|Touch-first alias of is\_title\_bar\_pressed. Returns True while the title bar is held or being dragged.|`result = gui.is_title_bar_active()`|
|`get_scroll_delta_y`|`()`|对象 / 数值|Touch-first alias of get\_smart\_scroll\_dy. Returns current scroll delta, including momentum when active.|`result = gui.get_scroll_delta_y()`|
|`menu_item`|`(label)`|对象 / 数值|Adds a clickable item to a menu.|`gui.menu_item(label='object')`|
|`menu_end`|`()`|对象 / 数值|Ends the menu section.|`gui.menu_end()`|
|`tooltip`|`(text)`|对象 / 数值|Shows a tooltip for the previously declared widget. On touch devices it appears on long press.|`result = gui.tooltip(text='hello visiong')`|
|`get_canvas`|`()`|对象 / 数值|Returns the current window's Canvas for custom drawing (stroke\_line, fill\_rect, draw\_text, etc.).|`result = gui.get_canvas()`|
|`widget_bounds`|`(canvas)`|对象 / 数值|Returns (x, y, w, h) of the last laid-out widget. Pass the canvas from get\_canvas.|`result = gui.widget_bounds(canvas=canvas)`|
|`input_is_mouse_down_in_rect`|`(rect, left_mouse=...)`|对象 / 数值|Returns True if the primary pointer is held inside the given (x,y,w,h) rect. On touch devices this follows finger contact.|`result = gui.input_is_mouse_down_in_rect(rect=(60, 40, 120, 90))`|
|`window_set_focus`|`(name)`|对象 / 数值|Sets focus to the window with the given name.|`gui.window_set_focus(name='demo')`|
|`window_drag_from_pos`|`(canvas)`|对象 / 数值|Direct-manipulation window drag. On touch devices this moves the current window while dragging its title bar.|`gui.window_drag_from_pos(canvas=canvas)`|
|`window_set_scroll`|`(scroll_y)`|对象 / 数值|Sets the current window's vertical scroll offset.|`gui.window_set_scroll(scroll_y=0)`|
|`input_is_mouse_dragging_in_rect`|`()`|对象 / 数值|Returns (is\_dragging, scroll\_dy, (x,y,w,h) content\_rect). On touch devices scroll\_dy includes locked-axis drag and fling momentum.|`result = gui.input_is_mouse_dragging_in_rect()`|
|`is_title_bar_pressed`|`()`|bool|Returns True if the current window title bar is actively held or dragged.|`result = gui.is_title_bar_pressed()`|
|`get_content_height`|`()`|对象 / 数值|Returns the content area height of the current layout.|`result = gui.get_content_height()`|
|`push_style_vec2`|`(name, x, y)`|对象 / 数值|Pushes a vec2 style. name: 'padding' or 'spacing'. Must be popped with pop\_style.|`gui.push_style_vec2(name='demo', x=10, y=10)`|
|`pop_style`|`()`|对象 / 数值|Pops the last pushed style (e.g. vec2).|`gui.pop_style()`|
|`get_smart_scroll_dy`|`()`|对象 / 数值|Returns touch-first scroll delta for the current window, including momentum when active.|`result = gui.get_smart_scroll_dy()`|
|`stroke_line`|`(canvas, x0, y0, x1, y1, thickness, color)`|对象 / 数值|Draws a line on the canvas. color: (R,G,B,A).|`result = gui.stroke_line(canvas=canvas, x0=10, y0=10, x1=120, y1=90, thickness=1, color=(0, 255, 0))`|
|`stroke_rect`|`(canvas, x, y, w, h, rounding, thickness, color)`|对象 / 数值|Draws a rectangle outline. color: (R,G,B,A).|`result = gui.stroke_rect(canvas=canvas, x=10, y=10, w=120, h=90, rounding=4, thickness=1, color=(0, 255, 0))`|
|`fill_rect`|`(canvas, x, y, w, h, rounding, color)`|对象 / 数值|Fills a rectangle. color: (R,G,B,A).|`result = gui.fill_rect(canvas=canvas, x=10, y=10, w=120, h=90, rounding=4, color=(0, 255, 0))`|
|`draw_text`|`(canvas, x, y, text, color)`|None / 状态码|Draws text at (x,y). color: (R,G,B,A).|`gui.draw_text(canvas=canvas, x=10, y=10, text='hello visiong', color=(0, 255, 0))`|
|`set_style_color`|`(property_name, color)`|None / 状态码|Sets a theme color. property\_name: 'text', 'header\_bg', 'button\_normal', 'button\_hover', 'button\_active'. color: (R,G,B,A).|`gui.set_style_color(property_name='name', color=(0, 255, 0))`|
|`set_style_button_rounding`|`(rounding)`|None / 状态码|Sets button corner rounding radius.|`gui.set_style_button_rounding(rounding=4)`|
|`set_style_window_rounding`|`(rounding)`|None / 状态码|Sets window corner rounding radius.|`gui.set_style_window_rounding(rounding=4)`|
|`set_window_background_color`|`(color)`|None / 状态码|Sets the current window background color. color: (R,G,B,A).|`gui.set_window_background_color(color=(0, 255, 0))`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

PinId 1 methods · 2 properties

Resolved pin identifier (bank + pin index).

**构造模板：** `pinId = visiong.PinId()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`__repr__`|`()`|对象 / 数值|\-|`text = repr(pinId)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`bank`|readonly\_field|`value = pinId.bank`|
|`pin`|readonly\_field|`value = pinId.pin`|

PinMuxRegisterInfo 1 methods · 8 properties

Raw IOMUX register field information for one pin.

**构造模板：** `pinMuxRegisterInfo = visiong.PinMuxRegisterInfo()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`__repr__`|`()`|对象 / 数值|PinMuxRegisterInfo(domain='|`text = repr(pinMuxRegisterInfo)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`domain`|readonly\_field|`value = pinMuxRegisterInfo.domain`|
|`base_addr`|readonly\_field|`value = pinMuxRegisterInfo.base_addr`|
|`reg_offset`|readonly\_field|`value = pinMuxRegisterInfo.reg_offset`|
|`absolute_addr`|readonly\_field|`value = pinMuxRegisterInfo.absolute_addr`|
|`bit`|readonly\_field|`value = pinMuxRegisterInfo.bit`|
|`width`|readonly\_field|`value = pinMuxRegisterInfo.width`|
|`mask`|readonly\_field|`value = pinMuxRegisterInfo.mask`|
|`gpio_only`|readonly\_field|`value = pinMuxRegisterInfo.gpio_only`|

PinAltFunction 1 methods · 3 properties

Alternative function description for one pin.

**构造模板：** `pinAltFunction = visiong.PinAltFunction()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`__repr__`|`()`|对象 / 数值|PinAltFunction(function='|`text = repr(pinAltFunction)`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`function`|readonly\_field|`value = pinAltFunction.function`|
|`group`|readonly\_field|`value = pinAltFunction.group`|
|`mux`|readonly\_field|`value = pinAltFunction.mux`|

PinRuntimeStatus 0 methods · 7 properties

Runtime pin ownership status from debugfs pinctrl.

**构造模板：** `pinRuntimeStatus = visiong.PinRuntimeStatus()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|\-|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`found`|readonly\_field|`value = pinRuntimeStatus.found`|
|`bank`|readonly\_field|`value = pinRuntimeStatus.bank`|
|`pin`|readonly\_field|`value = pinRuntimeStatus.pin`|
|`mux_owner`|readonly\_field|`value = pinRuntimeStatus.mux_owner`|
|`gpio_owner`|readonly\_field|`value = pinRuntimeStatus.gpio_owner`|
|`function`|readonly\_field|`value = pinRuntimeStatus.function`|
|`group`|readonly\_field|`value = pinRuntimeStatus.group`|

PinConflictReport 0 methods · 3 properties

Pin conflict detection report.

**构造模板：** `pinConflictReport = visiong.PinConflictReport()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|\-|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`conflict`|readonly\_field|`value = pinConflictReport.conflict`|
|`reason`|readonly\_field|`value = pinConflictReport.reason`|
|`runtime`|readonly\_field|`value = pinConflictReport.runtime`|

FunctionInterfaceStatus 0 methods · 7 properties

无类级描述。

**构造模板：** `functionInterfaceStatus = visiong.FunctionInterfaceStatus()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|\-|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`request`|readonly\_field|`value = functionInterfaceStatus.request`|
|`function`|readonly\_field|`value = functionInterfaceStatus.function`|
|`group`|readonly\_field|`value = functionInterfaceStatus.group`|
|`owner`|readonly\_field|`value = functionInterfaceStatus.owner`|
|`owner_bound`|readonly\_field|`value = functionInterfaceStatus.owner_bound`|
|`interfaces`|readonly\_field|`value = functionInterfaceStatus.interfaces`|
|`note`|readonly\_field|`value = functionInterfaceStatus.note`|

AdcChannelStatus 0 methods · 10 properties

IIO ADC channel readout status.

**构造模板：** `adcChannelStatus = visiong.AdcChannelStatus()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|\-|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`available`|readonly\_field|`value = adcChannelStatus.available`|
|`channel`|readonly\_field|`value = adcChannelStatus.channel`|
|`raw`|readonly\_field|`value = adcChannelStatus.raw`|
|`scale`|readonly\_field|`value = adcChannelStatus.scale`|
|`millivolts`|readonly\_field|`value = adcChannelStatus.millivolts`|
|`device`|readonly\_field|`value = adcChannelStatus.device`|
|`raw_path`|readonly\_field|`value = adcChannelStatus.raw_path`|
|`scale_path`|readonly\_field|`value = adcChannelStatus.scale_path`|
|`pin_hint`|readonly\_field|`value = adcChannelStatus.pin_hint`|
|`note`|readonly\_field|`value = adcChannelStatus.note`|

GpioLineConfig 0 methods · 0 properties

GPIO line request options (linux gpio-v2).

**构造模板：** `gpioLineConfig = visiong.GpioLineConfig()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|\-|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

GpioLineStatus 0 methods · 7 properties

Requested GPIO line runtime status.

**构造模板：** `gpioLineStatus = visiong.GpioLineStatus()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|\-|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`requested`|readonly\_field|`value = gpioLineStatus.requested`|
|`value`|readonly\_field|`value = gpioLineStatus.value`|
|`bank`|readonly\_field|`value = gpioLineStatus.bank`|
|`pin`|readonly\_field|`value = gpioLineStatus.pin`|
|`gpiochip`|readonly\_field|`value = gpioLineStatus.gpiochip`|
|`config`|readonly\_field|`value = gpioLineStatus.config`|
|`note`|readonly\_field|`value = gpioLineStatus.note`|

DriveStrengthStatus 0 methods · 7 properties

RV1106 IOC drive strength register status.

**构造模板：** `driveStrengthStatus = visiong.DriveStrengthStatus()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|\-|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`available`|readonly\_field|`value = driveStrengthStatus.available`|
|`level`|readonly\_field|`value = driveStrengthStatus.level`|
|`raw`|readonly\_field|`value = driveStrengthStatus.raw`|
|`reg_offset`|readonly\_field|`value = driveStrengthStatus.reg_offset`|
|`absolute_addr`|readonly\_field|`value = driveStrengthStatus.absolute_addr`|
|`domain`|readonly\_field|`value = driveStrengthStatus.domain`|
|`note`|readonly\_field|`value = driveStrengthStatus.note`|

PullStatus 0 methods · 7 properties

RV1106 IOC pull-up/down register status.

**构造模板：** `pullStatus = visiong.PullStatus()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|\-|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`available`|readonly\_field|`value = pullStatus.available`|
|`mode`|readonly\_field|`value = pullStatus.mode`|
|`raw`|readonly\_field|`value = pullStatus.raw`|
|`reg_offset`|readonly\_field|`value = pullStatus.reg_offset`|
|`absolute_addr`|readonly\_field|`value = pullStatus.absolute_addr`|
|`domain`|readonly\_field|`value = pullStatus.domain`|
|`note`|readonly\_field|`value = pullStatus.note`|

SchmittStatus 0 methods · 12 properties

RV1106 IOC input schmitt register status.

**构造模板：** `schmittStatus = visiong.SchmittStatus()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|\-|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`available`|readonly\_field|`value = schmittStatus.available`|
|`enabled`|readonly\_field|`value = schmittStatus.enabled`|
|`raw`|readonly\_field|`value = schmittStatus.raw`|
|`reg_offset`|readonly\_field|`value = schmittStatus.reg_offset`|
|`absolute_addr`|readonly\_field|`value = schmittStatus.absolute_addr`|
|`domain`|readonly\_field|`value = schmittStatus.domain`|
|`note`|readonly\_field|`value = schmittStatus.note`|
|`bank`|readonly\_field|`value = schmittStatus.bank`|
|`pin`|readonly\_field|`value = schmittStatus.pin`|
|`drive_supported`|readonly\_field|`value = schmittStatus.drive_supported`|
|`pull_supported`|readonly\_field|`value = schmittStatus.pull_supported`|
|`schmitt_supported`|readonly\_field|`value = schmittStatus.schmitt_supported`|

PinMux 32 methods · 0 properties

无类级描述。

**构造模板：** `pm = visiong.PinMux()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`is_open`|`()`|bool|Returns True if memory mappings are active.|`result = pm.is_open()`|
|`close`|`()`|None / 状态码|Closes /dev/mem mappings.|`pm.close()`|
|`parse_pin`|`(pin_name)`|对象 / 数值|Parses a pin string like 'GPIO1\_C4', 'gpio1-20', or '1:20'.|`result = pm.parse_pin(pin_name='GPIO0_C3')`|
|`get_mux` ×2|`(bank, pin)`|对象 / 数值|Reads current mux value from register field.|`result = pm.get_mux(bank=0, pin=0)`|
|`set_mux` ×2|`(bank, pin, mux)`|None / 状态码|Writes mux value using Rockchip write-mask semantics (no reboot required).|`pm.set_mux(bank=0, pin=0, mux=0)`|
|`get_register_info` ×2|`(bank, pin)`|对象 / 数值|Returns register address/bitfield info used for this pin.|`result = pm.get_register_info(bank=0, pin=0)`|
|`list_functions` ×2|`(bank, pin)`|对象 / 数值|Lists available alternate functions by parsing /proc/device-tree/pinctrl.|`result = pm.list_functions(bank=0, pin=0)`|
|`get_runtime_status` ×2|`(bank, pin)`|对象 / 数值|Reads mux/gpio owner and current function/group from debugfs pinctrl.|`result = pm.get_runtime_status(bank=0, pin=0)`|
|`check_conflict` ×2|`(bank, pin, target_function_or_group=...)`|对象 / 数值|Checks whether switching this pin may conflict with current mux/gpio owners.|`result = pm.check_conflict(bank=0, pin=0)`|
|`release_conflict` ×2|`(bank, pin)`|None / 状态码|Attempts to unbind current mux owner device. Returns False if release is incomplete.|`pm.release_conflict(bank=0, pin=0)`|
|`get_interface_status`|`(function_or_group)`|对象 / 数值|Reports whether Linux has exposed usable interfaces (/dev/\* or /sys/class/\*) for the function.|`result = pm.get_interface_status(function_or_group='uart4')`|
|`ensure_interface`|`(function_or_group)`|对象 / 数值|Attempts to bind the inferred owner device and re-check userspace interface visibility.|`result = pm.ensure_interface(function_or_group='uart4')`|
|`list_overlays`|`()`|对象 / 数值|Lists currently active device-tree overlays from configfs.|`result = pm.list_overlays()`|
|`apply_overlay`|`(dtbo_path, overlay_name=...)`|对象 / 数值|Applies a DT overlay (.dtbo) through configfs and returns created overlay entry name.|`result = pm.apply_overlay(dtbo_path='/path/to/overlay.dtbo')`|
|`remove_overlay`|`(overlay_name)`|对象 / 数值|Removes an applied configfs overlay by name.|`result = pm.remove_overlay(overlay_name='demo')`|
|`list_adc_channels`|`()`|对象 / 数值|Lists available SARADC channels from IIO sysfs and reads current values.|`result = pm.list_adc_channels()`|
|`read_adc` ×2|`(channel)`|对象 / 数值|Reads one ADC channel by numeric index.|`result = pm.read_adc(channel=0)`|
|`gpio_request_line` ×2|`(bank, pin, config=...)`|对象 / 数值|Requests one GPIO line with direction/bias/drive options.|`result = pm.gpio_request_line(bank=0, pin=0)`|
|`gpio_release_line` ×2|`(bank, pin)`|对象 / 数值|Releases a previously requested GPIO line.|`result = pm.gpio_release_line(bank=0, pin=0)`|
|`gpio_set_value` ×2|`(bank, pin, value)`|对象 / 数值|Sets value on a requested GPIO output line.|`result = pm.gpio_set_value(bank=0, pin=0, value=0)`|
|`gpio_get_value` ×2|`(bank, pin)`|对象 / 数值|Reads value from a requested GPIO line.|`result = pm.gpio_get_value(bank=0, pin=0)`|
|`gpio_get_status` ×2|`(bank, pin)`|对象 / 数值|Returns runtime status of requested GPIO line.|`result = pm.gpio_get_status(bank=0, pin=0)`|
|`set_drive_strength` ×2|`(bank, pin, level)`|None / 状态码|Sets RV1106 IOC drive strength level (0..7) for a pin.|`pm.set_drive_strength(bank=0, pin=0, level=3)`|
|`get_drive_strength` ×2|`(bank, pin)`|对象 / 数值|Reads RV1106 IOC drive strength level/raw register for a pin.|`result = pm.get_drive_strength(bank=0, pin=0)`|
|`set_pull` ×2|`(bank, pin, mode)`|None / 状态码|Sets pull mode (disable/pull\_up/pull\_down/bus\_hold or 0..3).|`pm.set_pull(bank=0, pin=0, mode='up')`|
|`get_pull` ×2|`(bank, pin)`|对象 / 数值|Reads pull mode/raw register for a pin.|`result = pm.get_pull(bank=0, pin=0)`|
|`set_input_schmitt` ×2|`(bank, pin, enable)`|None / 状态码|Enables/disables input schmitt for a pin.|`pm.set_input_schmitt(bank=0, pin=0, enable=True)`|
|`get_input_schmitt` ×2|`(bank, pin)`|对象 / 数值|Reads input schmitt state/raw register for a pin.|`result = pm.get_input_schmitt(bank=0, pin=0)`|
|`probe_electrical_capability` ×2|`(bank, pin, active_test=...)`|对象 / 数值|Probes drive/pull/schmitt capability for one pin. active\_test=True performs write-restore checks.|`result = pm.probe_electrical_capability(bank=0, pin=0)`|
|`probe_electrical_capabilities`|`(active_test=...)`|对象 / 数值|Probes drive/pull/schmitt capability for all pins.|`result = pm.probe_electrical_capabilities()`|
|`get_function_name` ×2|`(bank, pin)`|对象 / 数值|Returns best-effort function name matching current mux.|`result = pm.get_function_name(bank=0, pin=0)`|
|`set_function` ×2|`(bank, pin, function_or_group)`|None / 状态码|Sets mux by function name (e.g. 'uart4', 'pwm1') or group name (e.g. 'uart4m1-xfer').|`pm.set_function(bank=0, pin=0, function_or_group='uart4')`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

NpuClockStatus 0 methods · 8 properties

NPU clock probe status.

**构造模板：** `npuClockStatus = visiong.NpuClockStatus()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|\-|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`npu_node_present`|readonly\_field|`value = npuClockStatus.npu_node_present`|
|`debugfs_available`|readonly\_field|`value = npuClockStatus.debugfs_available`|
|`overlay_configfs_available`|readonly\_field|`value = npuClockStatus.overlay_configfs_available`|
|`assigned_rate_hz`|readonly\_field|`value = npuClockStatus.assigned_rate_hz`|
|`current_rate_hz`|readonly\_field|`value = npuClockStatus.current_rate_hz`|
|`npu_root_rate_hz`|readonly\_field|`value = npuClockStatus.npu_root_rate_hz`|
|`clk500m_src_rate_hz`|readonly\_field|`value = npuClockStatus.clk500m_src_rate_hz`|
|`note`|readonly\_field|`value = npuClockStatus.note`|

NpuClockApplyResult 0 methods · 11 properties

NPU clock apply result.

**构造模板：** `npuClockApplyResult = visiong.NpuClockApplyResult()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|\-|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|`ok`|readonly\_field|`value = npuClockApplyResult.ok`|
|`rebind_attempted`|readonly\_field|`value = npuClockApplyResult.rebind_attempted`|
|`rebind_ok`|readonly\_field|`value = npuClockApplyResult.rebind_ok`|
|`reboot_required`|readonly\_field|`value = npuClockApplyResult.reboot_required`|
|`requested_rate_hz`|readonly\_field|`value = npuClockApplyResult.requested_rate_hz`|
|`assigned_rate_hz`|readonly\_field|`value = npuClockApplyResult.assigned_rate_hz`|
|`current_rate_hz`|readonly\_field|`value = npuClockApplyResult.current_rate_hz`|
|`npu_root_rate_hz`|readonly\_field|`value = npuClockApplyResult.npu_root_rate_hz`|
|`clk500m_src_rate_hz`|readonly\_field|`value = npuClockApplyResult.clk500m_src_rate_hz`|
|`overlay_name`|readonly\_field|`value = npuClockApplyResult.overlay_name`|
|`message`|readonly\_field|`value = npuClockApplyResult.message`|

NpuClock 8 methods · 0 properties

RV1106 NPU clock helper via DT overlay and clock readback.

**构造模板：** `npu_clock = visiong.NpuClock()`

**构造签名：** `()`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`status`|`()`|对象 / 数值|Reads assigned/runtime NPU clock status.|`result = npu_clock.status()`|
|`supported_rates_hz`|`()`|对象 / 数值|Returns conservative validated NPU rates in Hz.|`result = npu_clock.supported_rates_hz()`|
|`supported_rates_mhz`|`()`|对象 / 数值|Returns conservative validated NPU rates in MHz.|`result = npu_clock.supported_rates_mhz()`|
|`list_overlays`|`(prefix=...)`|对象 / 数值|Lists active DT overlays with the given prefix.|`result = npu_clock.list_overlays()`|
|`remove_overlay`|`(overlay_name)`|对象 / 数值|Removes one DT overlay by name.|`result = npu_clock.remove_overlay(overlay_name='demo')`|
|`set_rate_hz`|`(rate_hz, update_cru_clk500m_src=..., unbind_rebind_npu=..., allow_unsafe_rate=...)`|None / 状态码|Applies NPU assigned-clock-rates in Hz. Can optionally update CRU CLK\_500M\_SRC and rebind NPU driver.|`npu_clock.set_rate_hz(rate_hz=500000000)`|
|`set_rate_mhz`|`(rate_mhz, update_cru_clk500m_src=..., unbind_rebind_npu=..., allow_unsafe_rate=...)`|None / 状态码|Applies NPU assigned-clock-rates in MHz.|`npu_clock.set_rate_mhz(rate_mhz=500)`|
|`request_reboot`|`()`|对象 / 数值|Requests immediate system reboot (sync + reboot).|`result = npu_clock.request_reboot()`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

DisplayHTTP 7 methods · 0 properties

无类级描述。

**构造模板：** `http = visiong.DisplayHTTP()`

**构造签名：** `(port=..., quality=..., mode=..., flv_path=..., flv_codec=..., flv_fps=..., flv_rc_mode=...)`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`stop`|`()`|None / 状态码|Stops the HTTP server and disconnects all clients. This is automatically called when the object is garbage collected.|`http.stop()`|
|`display`|`(img)`|None / 状态码|Encodes the ImageBuffer to JPEG (VENC) and pushes the frame to all connected MJPEG clients. In mode='jpg', DisplayHTTP keeps a local JPEG lock and may convert color format or black-pad smaller frames before encoding.|`http.display(img=img)`|
|`is_running`|`()`|bool|Returns True if the server is currently running.|`result = http.is_running()`|
|`set_fps`|`(fps)`|None / 状态码|Sets max FPS. 0 disables limiting.|`http.set_fps(fps=30)`|
|`get_fps`|`()`|对象 / 数值|Returns current max FPS.|`result = http.get_fps()`|
|`set_quality`|`(quality)`|None / 状态码|Sets JPEG quality (1-100).|`http.set_quality(quality=75)`|
|`get_quality`|`()`|对象 / 数值|Returns current JPEG quality (1-100).|`result = http.get_quality()`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

DisplayRTSP 11 methods · 0 properties

An RTSP streamer using hardware VENC (H264/H265). Supports multiple concurrent clients, TCP interleaved and UDP unicast transport.

**构造模板：** `rtsp = visiong.DisplayRTSP()`

**构造签名：** `(port=..., path=..., quality=..., codec=..., fps=..., logs=..., rc_mode=...)`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`stop`|`()`|None / 状态码|Stops the RTSP server. This is automatically called when the object is garbage collected.|`rtsp.stop()`|
|`set_fps`|`(fps)`|None / 状态码|Sets max frames per second. 0 disables limiting.|`rtsp.set_fps(fps=30)`|
|`get_fps`|`()`|对象 / 数值|Returns current max frames per second.|`result = rtsp.get_fps()`|
|`set_quality`|`(quality)`|None / 状态码|Sets encoding quality (1-100). Takes effect on the next display() call.|`rtsp.set_quality(quality=75)`|
|`get_quality`|`()`|对象 / 数值|Returns current encoding quality (1-100).|`result = rtsp.get_quality()`|
|`set_rc_mode`|`(rc_mode)`|None / 状态码|Sets rate control mode: 'cbr' or 'vbr'. Takes effect on next display() call.|`rtsp.set_rc_mode(rc_mode='cbr')`|
|`get_rc_mode`|`()`|对象 / 数值|Returns current rate control mode as string ('cbr' or 'vbr').|`result = rtsp.get_rc_mode()`|
|`set_logs`|`(logs)`|None / 状态码|Sets logs: 1 enable, 0 suppress.|`rtsp.set_logs(logs=1)`|
|`get_logs`|`()`|对象 / 数值|Returns 1 if logs are enabled, otherwise 0.|`result = rtsp.get_logs()`|
|`display`|`(img)`|None / 状态码|Encodes the ImageBuffer and pushes the frame to all connected RTSP clients.|`rtsp.display(img=img)`|
|`is_running`|`()`|bool|Returns True if the server is currently running.|`result = rtsp.is_running()`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

VencRecorder 4 methods · 0 properties

Hardware VENC recorder (Annex-B raw stream or MP4 mux).

**构造模板：** `rec = visiong.VencRecorder(filepath='/path/to/demo.jpg')`

**构造签名：** `(filepath, codec=..., container=..., quality=..., rc_mode=..., fps=..., mp4_faststart=...)`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`write`|`(img)`|None / 状态码|Encodes and writes one frame.|`rec.write(img=img)`|
|`close`|`()`|None / 状态码|Closes and finalizes the output file (required for MP4).|`rec.close()`|
|`is_open`|`()`|bool|Returns True if recorder is open.|`result = rec.is_open()`|
|`path`|`()`|对象 / 数值|Returns output filepath.|`result = rec.path()`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

DisplayHTTPFLV 9 methods · 0 properties

无类级描述。

**构造模板：** `flv = visiong.DisplayHTTPFLV()`

**构造签名：** `(port=..., path=..., quality=..., codec=..., fps=..., rc_mode=...)`

##### Methods（逐函数调用模板）

|方法|参数签名|返回值|说明|调用模板|
|---|---|---|---|---|
|`stop`|`()`|None / 状态码|Stops the server.|`flv.stop()`|
|`display`|`(img)`|None / 状态码|Encodes and pushes one frame to all connected viewers.|`flv.display(img=img)`|
|`is_running`|`()`|bool|Returns True if server is running.|`result = flv.is_running()`|
|`set_fps`|`(fps)`|None / 状态码|Sets max FPS. 0 disables limiting.|`flv.set_fps(fps=30)`|
|`get_fps`|`()`|对象 / 数值|Returns current max FPS.|`result = flv.get_fps()`|
|`set_quality`|`(quality)`|None / 状态码|Sets encoding quality 1-100.|`flv.set_quality(quality=75)`|
|`get_quality`|`()`|对象 / 数值|Returns current encoding quality.|`result = flv.get_quality()`|
|`set_rc_mode`|`(rc_mode)`|None / 状态码|Sets rate control mode: 'cbr' or 'vbr'. Takes effect on next display() call.|`flv.set_rc_mode(rc_mode='cbr')`|
|`get_rc_mode`|`()`|对象 / 数值|Returns current rate control mode as string ('cbr' or 'vbr').|`result = flv.get_rc_mode()`|

##### Properties / Fields（字段访问模板）

|字段|类型|访问模板|
|---|---|---|
|\-|

### 8.3 枚举

ImageTypeIVE 7 values

-   `U8C1`
-   `S16C1`
-   `U16C1`
-   `U64C1`
-   `YUV420SP`
-   `YUV422SP`
-   `U8C3_PACKAGE`

SobelOutCtrl 3 values

-   `HOR`
-   `VER`
-   `BOTH`

OrdStatFilterMode 3 values

-   `MEDIAN`
-   `MAX`
-   `MIN`

SubMode 2 values

-   `ABS`
-   `SHIFT`

LogicOp 3 values

-   `AND`
-   `OR`
-   `XOR`

ThreshMode 3 values

-   `BINARY`
-   `TRUNC`
-   `TO_MINVAL`

IntegOutCtrl 3 values

-   `COMBINE`
-   `SUM`
-   `SQSUM`

CscMode 16 values

-   `YUV2RGB_BT601_LIMITED`
-   `YUV2RGB_BT709_LIMITED`
-   `YUV2RGB_BT601_FULL`
-   `YUV2RGB_BT709_FULL`
-   `YUV2HSV_BT601_LIMITED`
-   `YUV2HSV_BT709_LIMITED`
-   `YUV2HSV_BT601_FULL`
-   `YUV2HSV_BT709_FULL`
-   `RGB2YUV_BT601_LIMITED`
-   `RGB2YUV_BT709_LIMITED`
-   `RGB2YUV_BT601_FULL`
-   `RGB2YUV_BT709_FULL`
-   `RGB2HSV_BT601_LIMITED`
-   `RGB2HSV_BT709_LIMITED`
-   `RGB2HSV_BT601_FULL`
-   `RGB2HSV_BT709_FULL`

Cast16to8Mode 4 values

-   `S16_TO_S8`
-   `S16_TO_U8_ABS`
-   `S16_TO_U8_BIAS`
-   `U16_TO_U8`

DmaMode 4 values

-   `DIRECT_COPY`
-   `INTERVAL_COPY`
-   `SET_3BYTE`
-   `SET_8BYTE`

LbpCmpMode 2 values

-   `NORMAL`
-   `ABS`

SadMode 3 values

-   `MB_4X4`
-   `MB_8X8`
-   `MB_16X16`

ModelType 7 values

-   `YOLOV5`
-   `RETINAFACE`
-   `FACENET`
-   `YOLO11`
-   `YOLO11_SEG`
-   `YOLO11_POSE`
-   `LPRNET`

## 9) 常见问题

import visiong 报错找不到共享库怎么办？

优先检查 visiong.py 与 \_visiong.so 是否在同一 Python 搜索路径下。其次确认设备运行时依赖库完整。

为什么某些类不存在，比如 IVE/NPU/GUI？

该能力通常由编译选项控制。请确认构建时是否启用了对应模块。

录制 MP4 结果损坏或无法播放？

通常是没有正确关闭写入器。请确保 VencRecorder.close() 被调用，或调用 close\_venc\_recorder/close\_all\_venc\_recorders。

NPU 推理结果为空或质量差？

先核对 model\_type、模型输入尺寸、color order（rgb/bgr）和预处理流程是否与训练/导出一致。

NumPy 零拷贝导入失败？

from\_numpy\_zero\_copy 要求 uint8、C 连续、且宽高为偶数。任一条件不满足都会抛异常。
