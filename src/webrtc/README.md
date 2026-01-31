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

推荐方案 1：HTTP 信令 WebRTC，因为：

延迟最低
浏览器原生支持
已有 libdatachannel 基础代码

方案	延迟	复杂度	说明
HTTP 信令 WebRTC	< 200ms	中	修改现有 WebRTC 模块，用 HTTP 替代 WebSocket 信令
WebSocket + MSE	200-500ms	低	直接推送 H.264 到浏览器，用 MSE 解码
RTSP	1-3s	已实现	需要外部播放器


已实现 HTTP 信令 WebRTC
工作原理
新增 API 端点
端点	方法	说明
/api/webrtc/offer	POST	获取设备的 SDP Offer
/api/webrtc/answer	POST	发送浏览器的 SDP Answer
/api/webrtc/ice	POST	发送 ICE 候选
/api/webrtc/ice	GET	获取设备的本地 ICE 候选
文件修改
webrtc.h - 添加 HTTP 信令 API 方法
webrtc.cpp - 实现 HTTP 信令模式
thread_webrtc.h - 暴露 HTTP 信令接口
thread_webrtc.cpp - 转发调用
http.cpp - 添加 HTTP 信令 API 端点
index.html - 恢复 WebRTC 播放器