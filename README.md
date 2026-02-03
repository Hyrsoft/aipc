

vlc打开：rtsp://192.168.8.235/live/0


使用ffmpeg打开rtsp流：ffplay rtsp://192.168.8.235/live/0


# 构建并安装
cmake --build build/Debug --target install

# 在设备上启动
./start_app.sh                    # 前台运行
./start_app.sh --daemon           # 后台运行
./start_app.sh --record           # 启用录制
SIGNALING_HOST=192.168.1.100 ./start_app.sh  # 指定信令服务器

# 停止服务
./stop_app.sh                     # 正常停止
./stop_app.sh --force             # 强制停止