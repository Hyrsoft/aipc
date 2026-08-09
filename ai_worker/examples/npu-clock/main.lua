function init(config)
  aipc.log("info", "NPU clock example requests 420 MHz with CRU update and NPU rebind")
end
function process(frame)
  return aipc.infer(frame, {})
end
