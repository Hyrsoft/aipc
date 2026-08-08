import { describe, expect, it } from 'vitest'
import { aiResultCategory, aiResultMatches, aiResultSummary, appendAiResultEvents, shortEventId } from './aiResults'
import type { AiCloudEvent, AiResultEventType } from './types'

function event(id: string, type: AiResultEventType = 'io.aipc.ai.frame.v1'): AiCloudEvent {
  return {
    specversion: '1.0',
    id: `stream-uuid:${id}`,
    source: 'urn:aipc:camera:camera0',
    type,
    subject: type.includes('.track.') ? 'track/ai-1/7' : 'frame/media-1/42',
    time: '2026-08-08T01:30:00Z',
    datacontenttype: 'application/json',
    dataschema: '/api/v1/ai/results/schema',
    data: type === 'io.aipc.ai.frame.v1' ? {
      schema_version: 1,
      source_id: 'camera0',
      media_generation: 'media-1',
      ai_generation: 'ai-1',
      sequence: 42,
      pts_us: 123456,
      published_at_ms: 1,
      frame: { width: 1920, height: 1080, coordinate_space: 'main_normalized_top_left' },
      inference: { project: 'yolo', algorithm: 'yolov5', model: 'model.rknn', duration_us: 89000 },
      objects: [{
        track_id: 7,
        class_id: 0,
        label: 'person',
        confidence: 0.91,
        bbox: { x: 0.1, y: 0.2, width: 0.3, height: 0.4 },
      }],
    } : {
      schema_version: 1,
      source_id: 'camera0',
      media_generation: 'media-1',
      ai_generation: 'ai-1',
      sequence: 42,
      pts_us: 123456,
      published_at_ms: 1,
      frame: { width: 1920, height: 1080, coordinate_space: 'main_normalized_top_left' },
      inference: { project: 'yolo', algorithm: 'yolov5', model: 'model.rknn', duration_us: 89000 },
      object: {
        track_id: 7,
        class_id: 0,
        label: 'person',
        confidence: 0.91,
        bbox: { x: 0.1, y: 0.2, width: 0.3, height: 0.4 },
      },
      reason: 'confirmed',
    },
  }
}

describe('AI standard result viewer helpers', () => {
  it('deduplicates events and keeps newest events first within the bound', () => {
    const result = appendAiResultEvents([event('1'), event('4')], [event('3'), event('1'), event('2'), event('2')], 3)
    expect(result.map((item) => item.id)).toEqual(['stream-uuid:4', 'stream-uuid:3', 'stream-uuid:2'])
  })

  it('classifies and filters target lifecycle events', () => {
    const entered = event('9', 'io.aipc.ai.track.entered.v1')
    expect(aiResultCategory(entered)).toBe('tracks')
    expect(aiResultMatches(entered, 'tracks')).toBe(true)
    expect(aiResultMatches(entered, 'frame')).toBe(false)
    expect(aiResultSummary(entered)).toContain('track 7 · person 91.0%')
  })

  it('summarizes frame objects and shortens stream event IDs', () => {
    const frame = event('42')
    expect(aiResultSummary(frame)).toContain('frame 42 · 1 objects · person 91%')
    expect(shortEventId(frame.id)).toBe('stream-u:42')
  })
})
