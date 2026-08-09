function init(config)
  aipc.log("info", "VisionG ISP focus maps to media_worker ownership; AI Lua has no raw AIQ access")
end

function process(frame)
  return {{x1=0, y1=0, x2=frame.width, y2=frame.height, confidence=1.0,
    class_id=0, label="isp-af", kind="capability", owner="media_worker",
    control="rkaiq", lua_access=false}}
end
