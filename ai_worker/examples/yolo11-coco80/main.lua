local model

function init(config)
  model = aipc.load_model(config.model, config)
  aipc.log("info", "YOLO11 COCO80 initialized")
end

function process(frame)
  return aipc.infer(frame, model, {})
end

function shutdown()
  model = nil
end
