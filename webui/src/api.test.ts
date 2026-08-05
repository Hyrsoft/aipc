import { describe, expect, it } from 'vitest'
import { reduceServerEvent, type LiveState } from './api'

describe('SSE state reducer', () => {
  it('updates status and appends bounded logs', () => {
    const initial: LiveState = { status: null, events: [], logs: [] }
    const status = reduceServerEvent(initial, {
      kind: 'status', timestamp_ms: 1, payload: { state: 'running', generation: 'g1' },
    })
    expect(status.status?.state).toBe('running')
    const logged = reduceServerEvent(status, {
      kind: 'log', timestamp_ms: 2, payload: { timestamp_ms: 2, stream: 'stderr', line: 'ready' },
    })
    expect(logged.logs[0].line).toBe('ready')
    expect(logged.events).toHaveLength(2)
  })
})
