function init(config)
  aipc.log("info", "IVE NCC template initialized")
end
function process(frame)
  return aipc.infer(frame, {})
end
