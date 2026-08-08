# AIPC 标准 AI 结果接口 v1

Rust daemon 将完成 AIPR 校验、模型输入坐标反变换和短生命周期目标跟踪后的结果，
通过 HTTP JSON 与 SSE 提供给报警、记录和其他独立服务。公开事件采用 CloudEvents
1.0 structured JSON；浏览器 OSD 使用的旧 `/api/v1/ai/events` 保持兼容，但新服务
应使用本文的 `/api/v1/ai/results/*` 接口。

## 接口

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| GET | `/api/v1/ai/results/latest` | 最新一条 `frame.v1`；尚无结果时返回 204 |
| GET | `/api/v1/ai/results/stream` | CloudEvents SSE 实时流和有界补发 |
| GET | `/api/v1/ai/results/schema` | `application/schema+json` 数据契约 |

```bash
curl -fsS http://BOARD_IP:8080/api/v1/ai/results/latest | jq
curl -N http://BOARD_IP:8080/api/v1/ai/results/stream
curl -fsS http://BOARD_IP:8080/api/v1/ai/results/schema | jq
```

daemon 当前沿用 trusted-LAN/no-auth 策略，不能直接暴露到不可信网络。

## CloudEvents 与事件类型

每个 SSE item 的 `id` 等于 CloudEvent `id`，`event` 等于 CloudEvent `type`，
`data` 是完整 CloudEvent JSON：

```text
id: 1d63955e-5dbc-4454-88bb-9f5349b23535:42
event: io.aipc.ai.frame.v1
data: {"specversion":"1.0", ...}
```

支持的类型：

- `io.aipc.ai.frame.v1`：每个通过校验的推理结果。
- `io.aipc.ai.track.entered.v1`：同一 track 连续达到确认帧数。
- `io.aipc.ai.track.updated.v1`：目标显著移动、置信度变化或达到更新心跳。
- `io.aipc.ai.track.exited.v1`：目标超过丢失 TTL，或 AI generation 被停止/替换。
- `io.aipc.ai.generation.v1`：media/AI generation 开始或停止。
- `io.aipc.ai.stream.gap.v1`：请求的 cursor 已超出内存 replay 窗口。

`id` 的冒号前是 daemon 实例 stream ID，冒号后是该实例内递增序号。daemon 重启后
stream ID 会改变；消费者必须把新的 generation/gap 视为重新同步边界。track 的唯一
身份是 `(ai_generation, track_id)`，不能只持久化 `track_id`。

## frame.v1 数据

```json
{
  "specversion": "1.0",
  "id": "1d63955e-5dbc-4454-88bb-9f5349b23535:42",
  "source": "urn:aipc:camera:camera0",
  "type": "io.aipc.ai.frame.v1",
  "subject": "frame/media-generation/1234",
  "time": "2026-08-08T01:30:00Z",
  "datacontenttype": "application/json",
  "dataschema": "/api/v1/ai/results/schema",
  "data": {
    "schema_version": 1,
    "source_id": "camera0",
    "media_generation": "media-generation",
    "ai_generation": "ai-generation",
    "sequence": 1234,
    "pts_us": 123456789,
    "published_at_ms": 1786150000000,
    "frame": {
      "width": 1920,
      "height": 1080,
      "coordinate_space": "main_normalized_top_left"
    },
    "inference": {
      "project": "yolov5-coco80",
      "algorithm": "yolov5",
      "model": "yolov5n_coco80_640.rknn",
      "duration_us": 89000
    },
    "objects": [{
      "track_id": 1,
      "class_id": 62,
      "label": "tv",
      "confidence": 0.58,
      "bbox": {"x": 0.42, "y": 0.31, "width": 0.16, "height": 0.28}
    }],
    "annotations": [{
      "kind": "text",
      "label": "沪A12345",
      "confidence": 0.93,
      "bbox": {"x": 0.42, "y": 0.31, "width": 0.16, "height": 0.08},
      "data": {"text": "沪A12345", "text_score": 0.93}
    }]
  }
}
```

`bbox` 原点在主路图像左上角，全部值均为 `[0, 1]` 主路归一化坐标；消费者可用
`frame.width/height` 换算像素。`pts_us` 是媒体链路单调微秒时间戳，用于和视频帧、
录像 generation 对齐；CloudEvent `time` 和 `published_at_ms` 是 daemon 发布墙上时间，
适合日志排序，但不能替代精确媒体 PTS。

`objects` 是经过 Rust tracker 关联、用于生命周期事件的目标；`annotations` 是 Lua/
VisionG 后端返回的附加结果，保留 `kind` 和算法专属 `data`。例如 PPOCR 使用
`kind=text` 并保留 `quad/text`，MLSD 使用 `kind=line` 并保留 `length`，NCC 使用
`kind=similarity` 并保留 `similarity`。附加结果不会改变主路视频或 OSD 的消费者隔离。

## SSE 补发和 gap

daemon 默认在内存中保留最近 256 个事件。SSE 客户端应保存最后成功处理的 `id`，
重连时发送标准 `Last-Event-ID`：

```bash
curl -N -H 'Last-Event-ID: STREAM_UUID:42' \
  http://BOARD_IP:8080/api/v1/ai/results/stream
```

- cursor 仍在窗口内：先顺序补发 cursor 之后的事件，再转入实时流。
- cursor 的 stream ID 不匹配或已过期：先收到 `stream.gap.v1`，其中包含当前
  `earliest_event_id` 和 `latest_event_id`。
- 收到 gap 后：调用 `results/latest` 建立最新状态，并以随后 SSE 事件继续处理。

replay 只存在内存中，不保证 daemon 重启后的审计级重放。慢消费者不会反压 AI、
OSD 或 VENC；如果内部 broadcast 落后，daemon 优先从 replay ring 补齐，超出窗口后
明确发送 gap。

## Python 消费者示例

```python
import json
import requests

url = "http://BOARD_IP:8080/api/v1/ai/results/stream"
last_event_id = None

while True:
    headers = {"Accept": "text/event-stream"}
    if last_event_id:
        headers["Last-Event-ID"] = last_event_id
    with requests.get(url, headers=headers, stream=True, timeout=(5, None)) as response:
        response.raise_for_status()
        event_id = None
        for raw in response.iter_lines(decode_unicode=True):
            if raw.startswith("id:"):
                event_id = raw[3:].strip()
            elif raw.startswith("data:"):
                event = json.loads(raw[5:].strip())
                if event["type"] == "io.aipc.ai.track.entered.v1":
                    obj = event["data"]["object"]
                    print("alarm candidate", obj["label"], obj["confidence"])
                elif event["type"] == "io.aipc.ai.stream.gap.v1":
                    latest = requests.get(
                        "http://BOARD_IP:8080/api/v1/ai/results/latest", timeout=5
                    )
                    if latest.status_code == 200:
                        print("resync", latest.json())
                if event_id:
                    last_event_id = event_id
```

生产消费者应在业务处理成功后再持久化 `last_event_id`。报警规则通常订阅
`track.entered/exited` 并自行增加区域、时间段和去抖规则；记录服务可保存
`frame.v1` 的 `media_generation + pts_us`，或在满足条件时调用现有录像 API。

## Rust 消费者示例

下面示例使用 `reqwest-eventsource`；生产代码同样应在业务处理成功后保存 event ID：

```rust
use futures_util::StreamExt;
use reqwest_eventsource::{Event, EventSource};

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let request = reqwest::Client::new()
        .get("http://BOARD_IP:8080/api/v1/ai/results/stream");
    let mut stream = EventSource::new(request)?;
    while let Some(item) = stream.next().await {
        if let Event::Message(message) = item? {
            let event: serde_json::Value = serde_json::from_str(&message.data)?;
            if event["type"] == "io.aipc.ai.track.entered.v1" {
                println!("entered: {}", event["data"]["object"]);
            }
        }
    }
    Ok(())
}
```

## 配置与状态

`ai` 配置新增：

```json
{
  "source_id": "camera0",
  "result_replay_capacity": 256,
  "track_confirmations": 2,
  "track_lost_timeout_ms": 500,
  "track_update_interval_ms": 500
}
```

`GET /api/v1/ai/status` 的 `result_bus` 提供 stream ID、最新/最早事件 ID、累计发布数、
replay 深度/容量和消费者 lag 计数。以上配置缺失时使用默认值，旧配置文件无需迁移。
