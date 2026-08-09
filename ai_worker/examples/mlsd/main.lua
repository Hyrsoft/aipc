local model
function init(config)
  model = aipc.load_model(config.model, config)
  aipc.log("info", "MLSD initialized; line results use kind=line")
end
function process(frame)
  return aipc.infer(frame, model, {})
end
function shutdown() model = nil end
