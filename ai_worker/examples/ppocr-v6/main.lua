local model

function init(config)
  model = aipc.load_model(config.model, config)
  aipc.log("info", "PPOCR v6 initialized; quad/text fields are retained")
end

function process(frame)
  return aipc.infer(frame, model, {})
end

function shutdown()
  model = nil
end
