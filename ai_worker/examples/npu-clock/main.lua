function init(config)
  aipc.log("info", "NPU clock probe is read-only; set options.apply=true only for maintenance")
end
function process(frame)
  return aipc.infer(frame, {})
end
