aipc 的media producer分为两个模式，simple ipc 和 visiong，其中simple ipc是纯粹的IP Camera模式，追求高清画质高帧率和低延迟（视频流管线），visiong负责AI处理与渲染（AI 推理管线），这两个模式区分开。

其中，visiong支持多种模型，比如 yolov5、yolov11物体识别、retainface人脸识别、PPOCR文字识别等，visiong库提供这些库的api（但需要自己准备模型文件和运行时环境）。apic 程序调用visiong 库提供的api，进行具体的AI视觉逻辑，比如识别什么物体，如何画线等等。

src/media_producer/visiong中分文件夹区分各个模型

simple ipc 和 visiong 模式的切换，是冷切换，需要重新配置rkmpi媒体管线，重新配置硬件资源，而 visiong 模式内部不同模型之间的切换，则由visiong库内部提供的api负责，区别于simple ipc 和 visiong 模式之间的冷切换。

如何集成visiong库？
visiong库的GitHub仓库提供了release，包括python版本和c++版本，包括动态库和静态库。对于aipc，不从源码开始编译，而是直接下载[预编译的库](https://github.com/yiex/visiong/releases/download/v1.0.1/visiong_cpp.zip), 解压到build目录中，预编译库提供so和头文件，供开发使用和链接，在cmake配置的安装脚本中，需要把visiong预编译库提供的动态库也复制到install文件夹中，方便进行部署。

把下载解压visiong_cpp写到cmakelists里，每次configure cmake时自动下载（检测如果已存在则忽略）

