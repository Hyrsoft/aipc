function init(config)
  aipc.log("info", "HSV blob example initialized")
end
function process(frame)
  return aipc.infer(frame, {})
end
