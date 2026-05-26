import type { WsEnvelope } from './types'

function resolveHttpOrigin(): string {
  const configured = import.meta.env.VITE_API_ORIGIN
  if (typeof configured === 'string' && configured.length > 0) {
    return configured
  }

  return window.location.origin
}

const HTTP_ORIGIN = resolveHttpOrigin()
const WS_ORIGIN = HTTP_ORIGIN.replace(/^http/, 'ws')
const API_PREFIX = '/api/v1'

export class ApiError extends Error {
  status: number
  code: string | null
  payload: unknown

  constructor(status: number, message: string, code: string | null, payload: unknown) {
    super(message)
    this.status = status
    this.code = code
    this.payload = payload
  }
}

interface ApiRequestOptions extends Omit<RequestInit, 'body'> {
  body?: unknown
  token?: string | null
}

function buildApiUrl(path: string): string {
  return new URL(`${API_PREFIX}${path}`, HTTP_ORIGIN).toString()
}

function readErrorMessage(payload: unknown, fallback: string): { message: string; code: string | null } {
  if (payload && typeof payload === 'object' && 'error' in payload) {
    const errorValue = (payload as { error?: unknown }).error
    if (errorValue && typeof errorValue === 'object') {
      const code = 'code' in errorValue && typeof errorValue.code === 'string' ? errorValue.code : null
      const message =
        'message' in errorValue && typeof errorValue.message === 'string'
          ? errorValue.message
          : fallback
      return { message, code }
    }
  }

  if (payload && typeof payload === 'object' && 'message' in payload && typeof payload.message === 'string') {
    return { message: payload.message, code: null }
  }

  if (typeof payload === 'string' && payload.trim().length > 0) {
    return { message: payload, code: null }
  }

  return { message: fallback, code: null }
}

export async function apiRequest<T>(path: string, options: ApiRequestOptions = {}): Promise<T> {
  const headers = new Headers(options.headers)
  headers.set('Accept', 'application/json')

  if (options.body !== undefined) {
    headers.set('Content-Type', 'application/json')
  }
  if (options.token) {
    headers.set('Authorization', `Bearer ${options.token}`)
  }

  const response = await fetch(buildApiUrl(path), {
    ...options,
    headers,
    credentials: 'same-origin',
    body: options.body === undefined ? undefined : JSON.stringify(options.body),
  })

  if (response.status === 204) {
    return undefined as T
  }

  const contentType = response.headers.get('content-type') ?? ''
  const payload = contentType.includes('application/json') ? await response.json() : await response.text()

  if (!response.ok) {
    const { message, code } = readErrorMessage(payload, `request failed with status ${response.status}`)
    throw new ApiError(response.status, message, code, payload)
  }

  return payload as T
}

export function buildWebSocketUrl(
  path: string,
  token?: string | null,
  query: Record<string, string | number | null | undefined> = {},
): string {
  const url = new URL(path, WS_ORIGIN)
  if (token) {
    url.searchParams.set('access_token', token)
  }
  for (const [key, value] of Object.entries(query)) {
    if (value === undefined || value === null || value === '') {
      continue
    }
    url.searchParams.set(key, String(value))
  }
  return url.toString()
}

export function sendEnvelope<TPayload>(
  socket: WebSocket,
  type: string,
  payload: TPayload,
  requestId: string = crypto.randomUUID(),
): string {
  const envelope: WsEnvelope<TPayload> = {
    type,
    requestId,
    payload,
  }
  socket.send(JSON.stringify(envelope))
  return requestId
}