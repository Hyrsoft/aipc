import type { AiModelInfo, AiOsdMode, AiProjectDocument, AiProjectSummary, AiStatus, DaemonStatus, LogEntry, PersistentState, RecordingList, RecordingSettings, RecordingStatus, RtspStatus, ServerEvent, WorkerConfig } from './types'

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
  recordingSettings: () => request<RecordingSettings>('/api/v1/recording/settings'),
  updateRecordingSettings: (directory: string) => request<RecordingSettings>('/api/v1/recording/settings', {
    method: 'PUT', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ directory }),
  }),
  recordingStatus: () => request<RecordingStatus>('/api/v1/recording/status'),
  recordingControl: (action: 'start' | 'stop') => request<RecordingStatus>(`/api/v1/recording/${action}`, { method: 'POST' }),
  recordings: (offset = 0, limit = 25) => request<RecordingList>(`/api/v1/recordings?offset=${offset}&limit=${limit}`),
  deleteRecordings: (ids: string[]) => request<{ deleted: number }>('/api/v1/recordings/delete', {
    method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ ids }),
  }),
  exportRecordings: async (ids: string[]) => {
    const response = await fetch('/api/v1/recordings/export', {
      method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ ids }),
    })
    if (!response.ok) throw new Error((await response.json().catch(() => ({})))?.error?.message || `HTTP ${response.status}`)
    return response.blob()
  },
  rtspStatus: () => request<RtspStatus>('/api/v1/rtsp/status'),
  aiStatus: () => request<AiStatus>('/api/v1/ai/status'),
  aiProjects: () => request<AiProjectSummary[]>('/api/v1/ai/projects'),
  aiProject: (id: string) => request<AiProjectDocument>(`/api/v1/ai/projects/${encodeURIComponent(id)}`),
  saveAiProject: (document: AiProjectDocument) => request<AiProjectDocument>(
    `/api/v1/ai/projects/${encodeURIComponent(document.manifest.id)}`,
    { method: 'PUT', headers: { 'content-type': 'application/json' }, body: JSON.stringify(document) },
  ),
  createAiProject: (document: AiProjectDocument) => request<AiProjectDocument>(
    '/api/v1/ai/projects',
    { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(document) },
  ),
  deleteAiProject: (id: string) => request<void>(`/api/v1/ai/projects/${encodeURIComponent(id)}`, { method: 'DELETE' }),
  validateAiProject: (id: string) => request<{ valid: boolean; project: string }>(
    `/api/v1/ai/projects/${encodeURIComponent(id)}/validate`, { method: 'POST' },
  ),
  deployAiProject: (id: string) => request<AiStatus>(
    `/api/v1/ai/projects/${encodeURIComponent(id)}/deploy`, { method: 'POST' },
  ),
  aiModels: () => request<AiModelInfo[]>('/api/v1/ai/models'),
  uploadAiModel: (file: File) => {
    const body = new FormData()
    body.append('file', file)
    return request<AiModelInfo>('/api/v1/ai/models', { method: 'POST', body })
  },
  deleteAiModel: (name: string) => request<void>(`/api/v1/ai/models/${encodeURIComponent(name)}`, { method: 'DELETE' }),
  setAiOsd: (mode: AiOsdMode) => request<{ mode: AiOsdMode }>('/api/v1/ai/osd', {
    method: 'PUT', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ mode }),
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
