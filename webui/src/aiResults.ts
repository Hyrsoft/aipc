import type { AiCloudEvent, AiFrameResultData, AiResultEventType, AiTrackResultData } from './types'

export type AiResultFilter = 'all' | 'frame' | 'tracks' | 'generation' | 'gap'
export type AiResultCategory = Exclude<AiResultFilter, 'all'>

export const aiResultEventTypes: AiResultEventType[] = [
  'io.aipc.ai.frame.v1',
  'io.aipc.ai.track.entered.v1',
  'io.aipc.ai.track.updated.v1',
  'io.aipc.ai.track.exited.v1',
  'io.aipc.ai.stream.gap.v1',
  'io.aipc.ai.generation.v1',
]

export function aiResultCategory(event: AiCloudEvent): AiResultCategory {
  if (event.type === 'io.aipc.ai.frame.v1') return 'frame'
  if (event.type.startsWith('io.aipc.ai.track.')) return 'tracks'
  if (event.type === 'io.aipc.ai.generation.v1') return 'generation'
  return 'gap'
}

export function aiResultTypeLabel(type: AiResultEventType): string {
  return type.replace('io.aipc.ai.', '').replace('.v1', '')
}

export function aiResultMatches(event: AiCloudEvent, filter: AiResultFilter): boolean {
  return filter === 'all' || aiResultCategory(event) === filter
}

export function appendAiResultEvents(
  current: AiCloudEvent[],
  incoming: AiCloudEvent[],
  limit = 240,
): AiCloudEvent[] {
  const known = new Set(current.map((event) => event.id))
  const unique = incoming.filter((event) => {
    if (known.has(event.id)) return false
    known.add(event.id)
    return true
  })
  if (!unique.length) return current
  return [...unique, ...current].sort(compareNewestFirst).slice(0, limit)
}

function compareNewestFirst(left: AiCloudEvent, right: AiCloudEvent): number {
  const leftSeparator = left.id.lastIndexOf(':')
  const rightSeparator = right.id.lastIndexOf(':')
  const leftStream = left.id.slice(0, leftSeparator)
  const rightStream = right.id.slice(0, rightSeparator)
  if (leftSeparator >= 0 && rightSeparator >= 0 && leftStream === rightStream) {
    const leftSequence = Number(left.id.slice(leftSeparator + 1))
    const rightSequence = Number(right.id.slice(rightSeparator + 1))
    if (Number.isFinite(leftSequence) && Number.isFinite(rightSequence)) return rightSequence - leftSequence
  }
  return Date.parse(right.time) - Date.parse(left.time)
}

export function aiResultSummary(event: AiCloudEvent): string {
  if (event.type === 'io.aipc.ai.frame.v1') {
    const data = event.data as AiFrameResultData
    const labels = data.objects.slice(0, 4).map((object) => `${object.label} ${(object.confidence * 100).toFixed(0)}%`)
    const annotationKinds = [...new Set(data.annotations.map((annotation) => annotation.kind))]
    const annotationText = data.annotations.length ? ` · ${data.annotations.length} annotations${annotationKinds.length ? ` (${annotationKinds.join(', ')})` : ''}` : ''
    return `frame ${data.sequence} · ${data.objects.length} objects${annotationText}${labels.length ? ` · ${labels.join(', ')}` : ''}`
  }
  if (event.type.startsWith('io.aipc.ai.track.')) {
    const data = event.data as AiTrackResultData
    return `track ${data.object.track_id} · ${data.object.label} ${(data.object.confidence * 100).toFixed(1)}% · ${data.reason}`
  }
  if (event.type === 'io.aipc.ai.generation.v1') {
    const data = event.data as { state: string; reason: string; ai_generation: string | null }
    return `${data.state} · ${data.ai_generation?.slice(0, 8) || 'none'} · ${data.reason}`
  }
  const data = event.data as { reason: string; earliest_event_id: string | null; latest_event_id: string | null }
  return `${data.reason} · replay ${shortEventId(data.earliest_event_id)} → ${shortEventId(data.latest_event_id)}`
}

export function shortEventId(id: string | null | undefined): string {
  if (!id) return '—'
  const separator = id.lastIndexOf(':')
  return separator < 0 ? id : `${id.slice(0, 8)}:${id.slice(separator + 1)}`
}
