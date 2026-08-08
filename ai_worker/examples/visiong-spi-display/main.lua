function init(config)
  aipc.log("info", "VisionG DisplaySPI remains an external media sink; AI Lua does not own SPI")
end

function process(frame)
  return {{x1=0, y1=0, x2=frame.width, y2=frame.height, confidence=1.0,
    class_id=0, label="spi-display", kind="capability",
    owner="external-media-sink", chip="ST7789", rotation_degrees=90}}
end
