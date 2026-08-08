function init(config)
  aipc.log("info", "IVE filter runs in the worker; output is intentionally metadata-only")
end
function process(frame)
  aipc.infer(frame, {})
  return {}
end
