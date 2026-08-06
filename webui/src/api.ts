import type { DaemonStatus, LogEntry, PersistentState, ServerEvent, WorkerConfig } from './types'

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(path, init)
  const body = await response.json().catch(() => ({}))
  if (!response.ok) throw new Error(body?.error?.message || `HTTP ${response.status}`)
  return body as T
}

export const api = {
  status: () => request<DaemonStatus>('/api/v1/status'),
  config: () => request<PersistentState>('/api/v1/config'),
  logs: (limit = 100) => request<LogEntry[]>(`/api/v1/logs?limit=${limit}`),
  control: (action: 'start' | 'stop' | 'restart') =>
    request<{ generation: string | null; action: string }>(`/api/v1/worker/${action}`, { method: 'POST' }),
  apply: (config: WorkerConfig) =>
    request<{ generation: string; action: string }>('/api/v1/config', {
      method: 'PUT',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(config),
    }),
}

export interface LiveState {
  status: DaemonStatus | null
  events: ServerEvent[]
  logs: LogEntry[]
}

export function reduceServerEvent(state: LiveState, event: ServerEvent): LiveState {
  const events = [...state.events, event].slice(-200)
  if (event.kind === 'status') return { ...state, status: event.payload, events }
  if (event.kind === 'log') return { ...state, logs: [...state.logs, event.payload].slice(-200), events }
  return { ...state, events }
}

export function connectEvents(onEvent: (event: ServerEvent) => void, onConnection: (up: boolean) => void) {
  const source = new EventSource('/api/v1/events')
  const kinds = ['status', 'worker_event', 'supervisor', 'log', 'lagged']
  for (const kind of kinds) {
    source.addEventListener(kind, (message) => {
      try {
        const parsed = JSON.parse((message as MessageEvent).data)
        onEvent(parsed?.kind ? parsed : { kind, timestamp_ms: Date.now(), payload: parsed })
      } catch { /* keep stream alive */ }
    })
  }
  source.onopen = () => onConnection(true)
  source.onerror = () => onConnection(false)
  return () => source.close()
}
