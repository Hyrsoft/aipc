function init(config)
  aipc.log("info", "VisionG DisplayRTSP maps to daemon RTSP at :8554/live")
end

function process(frame)
  return {{x1=0, y1=0, x2=frame.width, y2=frame.height, confidence=1.0,
    class_id=0, label="rtsp", kind="capability", owner="aipc-daemon",
    endpoint="rtsp://BOARD_IP:8554/live", codec="h264"}}
end
