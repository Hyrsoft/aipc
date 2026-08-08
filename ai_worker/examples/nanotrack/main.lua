local model
local current
local lost = 0
local initialized = false
local config

local function center_box(frame, config)
  local w = math.max(16, math.floor(frame.width * (config.options.center_width_ratio or 0.25)))
  local h = math.max(16, math.floor(frame.height * (config.options.center_height_ratio or 0.25)))
  return {math.floor((frame.width - w) / 2), math.floor((frame.height - h) / 2), w, h}
end

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

local function smooth(raw, frame)
  if not current then return raw end
  local factor = config.options.position_smooth or 0.6
  local old_w = current.x2 - current.x1
  local old_h = current.y2 - current.y1
  local new_w = raw.x2 - raw.x1
  local new_h = raw.y2 - raw.y1
  local max_scale = config.options.max_scale_change or 1.05
  local min_scale = config.options.min_scale_change or 0.9
  new_w = clamp(new_w, old_w * min_scale, old_w * max_scale)
  new_h = clamp(new_h, old_h * min_scale, old_h * max_scale)
  local cx = (raw.x1 + raw.x2) / 2
  local cy = (raw.y1 + raw.y2) / 2
  cx = current.x1 + old_w / 2 + (cx - (current.x1 + old_w / 2)) * factor
  cy = current.y1 + old_h / 2 + (cy - (current.y1 + old_h / 2)) * factor
  raw.x1 = math.floor(clamp(cx - new_w / 2, 0, frame.width - 1))
  raw.y1 = math.floor(clamp(cy - new_h / 2, 0, frame.height - 1))
  raw.x2 = math.floor(clamp(raw.x1 + new_w, raw.x1 + 1, frame.width))
  raw.y2 = math.floor(clamp(raw.y1 + new_h, raw.y1 + 1, frame.height))
  return raw
end

function init(cfg)
  config = cfg
  model = aipc.load_model(config.model, config)
  aipc.log("info", "NanoTrack initialized with center-box recovery")
end

function process(frame)
  config = config or {options = {}}
  if not initialized then
    local result = aipc.infer(frame, model, {action = "init", box = center_box(frame, config)})
    current = result[1]
    initialized = true
    lost = 0
    return result
  end
  local result = aipc.infer(frame, model, {})
  local raw = result[1]
  if not raw then return {} end
  if raw.confidence < (config.threshold or 0.6) then
    lost = lost + 1
    if lost > (config.options.max_lost_frames or 30) then
      initialized = false
      current = nil
      lost = 0
    end
    return current and {current} or {}
  end
  lost = 0
  current = smooth(raw, frame)
  return {current}
end

function shutdown()
  model = nil
  current = nil
end
