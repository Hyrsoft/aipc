local model
function init(config)
  model = aipc.load_model(config.model, config)
  aipc.log("info", "LPRNet initialized; input is the complete AI frame")
end
function process(frame)
  return aipc.infer(frame, model, {})
end
function shutdown() model = nil end
