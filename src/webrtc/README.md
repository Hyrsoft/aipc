核心模块文件
文件	描述
signaling.h	信令客户端头文件 - WebSocket 信令协议
signaling.cpp	信令实现 - 房间加入、SDP/ICE 交换
webrtc.h	WebRTC 系统头文件 - PeerConnection 管理
webrtc.cpp	WebRTC 实现 - 视频轨道、H.264 RTP 封装
thread_webrtc.h	线程封装头文件 - StreamDispatcher 集成
thread_webrtc.cpp	线程封装实现
关键特性
信令协议 - 基于 WebSocket 的 JSON 消息格式：

join/leave - 房间管理
offer/answer - SDP 交换
ice - ICE 候选交换
视频传输 - 使用 libdatachannel：

H.264 RTP 打包 (rtc::H264RtpPacketizer)
与 RKMPI VENC 编码流集成
项目集成 - 遵循现有架构：

StreamConsumer 回调模式
与 RTSP 和 File 模块并行工作