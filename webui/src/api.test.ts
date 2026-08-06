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
    expect(logged.events[1].kind).toBe('log')
  })

  it('keeps the newest 200 events in chronological order', () => {
    let state: LiveState = { status: null, events: [], logs: [] }
    for (let timestamp_ms = 1; timestamp_ms <= 205; timestamp_ms += 1) {
      state = reduceServerEvent(state, { kind: 'worker_event', timestamp_ms, payload: { event: 'Metrics' } })
    }
    expect(state.events).toHaveLength(200)
    expect(state.events[0].timestamp_ms).toBe(6)
    expect(state.events[199].timestamp_ms).toBe(205)
  })
})
