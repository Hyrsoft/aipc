local model = nil

function init(config)
  model = aipc.load_model(config.model)
  aipc.log("info", "YOLOv5 project initialized")
end

function process(frame)
  return aipc.infer(frame, model, {})
end

function shutdown()
  model = nil
end
