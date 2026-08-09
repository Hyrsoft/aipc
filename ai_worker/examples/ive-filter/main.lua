local effects = {
  {name = "Gaussian Blur", kernel = {1,4,6,4,1,4,16,24,16,4,6,24,36,24,6,4,16,24,16,4,1,4,6,4,1}},
  {name = "Sharpen", kernel = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,25,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}},
  {name = "Edge Detect", kernel = {0,0,-1,0,0,0,-1,-2,-1,0,-1,-2,16,-2,-1,0,-1,-2,-1,0,0,0,-1,0,0}},
  {name = "Emboss", kernel = {-2,-1,0,0,0,-1,-1,0,0,0,0,0,1,0,0,0,0,0,1,1,0,0,0,1,2}}
}

function init(config)
  aipc.log("info", "IVE filter initialized; cycling four VisionG kernels")
end

function process(frame)
  -- The Python sample changes the effect every five seconds.  The input is
  -- configured at 5 FPS, so using the frame sequence keeps Lua deterministic
  -- without opening a timer or other host resource.
  local index = (math.floor(frame.sequence / 25) % #effects) + 1
  aipc.infer(frame, {kernel = effects[index].kernel})
  return {}
end
