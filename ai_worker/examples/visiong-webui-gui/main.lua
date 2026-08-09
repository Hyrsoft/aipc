function init(config)
  aipc.log("info", "VisionG GUI maps to AIPC WebUI, preview and recording APIs")
end

function process(frame)
  return {{x1=0, y1=0, x2=frame.width, y2=frame.height, confidence=1.0,
    class_id=0, label="webui-gui", kind="capability",
    owner="aipc-daemon+webui", preview="/api/v1/preview/ws",
    recordings="/api/v1/recordings"}}
end
