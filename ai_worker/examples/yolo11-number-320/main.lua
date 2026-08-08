local model

function init(config)
  model = aipc.load_model(config.model, config)
  aipc.log("info", "YOLO11 number 320 initialized")
end

function process(frame)
  return aipc.infer(frame, model, {})
end

function shutdown()
  model = nil
end
