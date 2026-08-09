local logged = false
function init(config)
  aipc.log("info", "RTSP/preview/GUI/SPI remain media sinks; AI worker only observes frames")
end
function process(frame)
  if not logged then
    local info = aipc.frame_info(frame)
    aipc.log("info", string.format("media frame %dx%d seq=%d", info.width, info.height, info.sequence))
    logged = true
  end
  return {}
end
