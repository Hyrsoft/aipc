## 问题1:

visiong库的目标是：在visiong模式下，推理模型、分辨率，都由python代码负责，但是在i_media_producer.h中，还保留了预设分辨率配置的结构体，预设了AI模式的分辨率是640x360；
在media_manager单例中，还保留了设置分辨率（需要重新初始化）的方法，然而目前simple ipc是可能要从预设分辨率/帧率中选择，但是visiong是完全由代码驱动的。因此，分辨率设置这一块，不能作为media_manager和media_prducer子类的共有接口。

## 问题2:

simple ipc完全不负责AI推理模式的相关设置，但是在src/media_producer/simple_ipc/mpi_config.h中还存在遗留的ai配置相关函数。
注意区分simple ipc和visiong的初始化流程，simpe ipc使用rkmpi提供的api，而visiong在python代码中使用它自带的api（底层也是基于rkmpi），但是目前mpi_config.h中，过多mpi配置相关的自由函数，还是用media这个名称空间，从语义上与visiong有混淆，应该在名称空间层面加以区分。

## 问题3:

在src/media_producer/visiong中，visiong模式，是python代码负责摄像头初始化、帧循环、模型选择、分辨率配置、讲处理后的视频帧传递给c++代码进行编码。c++代码负责的是python代码管理、python解释器生命周期管理、编码和分发。
但是visiong_producer.h的注释中的内容还是上一版的架构，包括src/http.cpp中内嵌的python代码，也是上一版的。
