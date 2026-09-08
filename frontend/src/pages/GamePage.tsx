import { useEffect, useRef, useState } from 'react'
import { useLocation, useNavigate, useParams } from 'react-router-dom'

import SceneAppearancePanel from '../components/SceneAppearancePanel'
import { MahjongScene } from '../game/scene/MahjongScene'
import { buildWebSocketUrl, sendEnvelope } from '../lib/backend'
import { useStoredSceneAppearance } from '../lib/useStoredSceneAppearance'
import {
  clearStoredAuth,
  clearStoredSessionId,
  loadStoredAuth,
  loadStoredSessionId,
  loadStoredVolume,
  saveStoredSessionId,
  saveStoredVolume,
} from '../lib/storage'
import type {
  GameEventPayload,
  PendingSnapshot,
  PassAckPayload,
  SessionSnapshot,
  WsEnvelope,
} from '../lib/types'
import { rankToChinese } from '../lib/chinese'
import './GamePage.css'

const END_RESULTS_DELAY_MS = 3000
const PRIVATE_DRAW_EVENTS = new Set(['predraw', 'draw_tile'])

type RatingCard = {
  player_id: number
  username?: string
  mu?: number
  tau?: number
  sigma?: number
  points?: number
  level?: number
}

type SpectatorSeat = {
  seat_index: number
  player_id: number | null
  username: string | null
  hand_tile_count: number
  has_drawn_tile: boolean
}

type SpectatorManagementEntry = {
  status: 'pending' | 'approved'
  spectator_player_id: number
  spectator_username: string
  request_id?: string
  expires_at_ms?: number
}

type SpectatorHandAccess = {
  target_player_id: number
  seat_index: number
}

function resolveSessionId(routeId: string | undefined, search: string): number | null {
  const params = new URLSearchParams(search)
  const candidates = [routeId, params.get('gameId'), params.get('sessionId')]
  for (const c of candidates) {
    if (!c) continue
    const n = Number(c)
    if (Number.isFinite(n) && n > 0) return n
  }
  return null
}

export default function GamePage() {
  const navigate = useNavigate()
  const location = useLocation()
  const params = useParams()

  const auth = loadStoredAuth()
  const token = auth?.session.token ?? null
  const isSpectator = new URLSearchParams(location.search).get('spectate') === '1'

  const [sessionIdHint, setSessionIdHint] = useState<number | null>(
    () => resolveSessionId(params.sessionId, location.search) ?? (isSpectator ? null : loadStoredSessionId()),
  )
  const [phase, setPhase] = useState<'loading' | 'pending' | 'active'>('loading')
  const [pendingSnapshot, setPendingSnapshot] = useState<PendingSnapshot | null>(null)
  const [notification, setNotification] = useState('')
  const [showNotif, setShowNotif] = useState(false)
  const [sceneReady, setSceneReady] = useState(false)
  const [appearancePanelOpen, setAppearancePanelOpen] = useState(false)
  const [spectatorManagementOpen, setSpectatorManagementOpen] = useState(false)
  const [ratingsExpanded, setRatingsExpanded] = useState(
    () => window.matchMedia('(min-width: 901px)').matches,
  )
  const [volume, setVolumeState] = useState(() => loadStoredVolume())
  const {
    appearance: sceneAppearance,
    backgroundImage,
    backgroundImageLoading,
    setBackgroundColorTable,
    setBackgroundColorOutside,
    setBackgroundImageEnabled,
    setBackgroundImageAlpha,
    setTileCoverColor,
    addTileCoverColor,
    removeTileCoverColor,
    uploadBackgroundImage,
    clearBackgroundImage,
    resetAppearance,
  } = useStoredSceneAppearance()

  const stageRef = useRef<HTMLDivElement | null>(null)
  const sceneRef = useRef<MahjongScene | null>(null)
  const socketRef = useRef<WebSocket | null>(null)
  const lastScRef = useRef(-1)
  const disposedRef = useRef(false)
  const authoritativeActiveRef = useRef(false)
  const gameEndedRef = useRef(false)
  const endTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const [gameRatings, setGameRatings] = useState<RatingCard[]>([])
  const [pendingRatings, setPendingRatings] = useState<RatingCard[]>([])
  const [spectatorSeats, setSpectatorSeats] = useState<SpectatorSeat[]>([])
  const spectatorSeatsRef = useRef<SpectatorSeat[]>([])
  const [requestedHandSeat, setRequestedHandSeat] = useState<number | null>(null)
  const [approvedHandAccess, setApprovedHandAccess] = useState<SpectatorHandAccess | null>(null)
  const approvedHandAccessRef = useRef<SpectatorHandAccess | null>(null)
  const [spectatorPerspectiveSeat, setSpectatorPerspectiveSeat] = useState(0)
  const spectatorPerspectiveSeatRef = useRef(0)
  const spectatorPerspectivePlayerIdRef = useRef<number | null>(null)
  const pendingAuthorizedDrawEventRef = useRef<GameEventPayload | null>(null)
  const spectatorPredrawInProgressRef = useRef(false)
  const [spectatorManagement, setSpectatorManagement] = useState<SpectatorManagementEntry[]>([])
  const spectatorManagementPendingIdsRef = useRef<Set<string>>(new Set())
  const [abandonEnabled, setAbandonEnabled] = useState(false)
  const [duplicateMode, setDuplicateMode] = useState(false)
  const [myAbandoned, setMyAbandoned] = useState(false)
  const [abandonConfirmOpen, setAbandonConfirmOpen] = useState(false)

  // ── Resolve session id from URL changes ───────────────────────
  useEffect(() => {
    setSessionIdHint(
      resolveSessionId(params.sessionId, location.search) ?? (isSpectator ? null : loadStoredSessionId()),
    )
  }, [isSpectator, location.search, params.sessionId])

  // ── Mount Pixi scene ──────────────────────────────────────────
  useEffect(() => {
    if (!stageRef.current) return
    let cancelled = false
    const scene = new MahjongScene((type, payload) => {
      const s = socketRef.current
      if (s && s.readyState === WebSocket.OPEN) {
        sendEnvelope(s, type, payload)
      }
    })
    scene.setPresentationMode(isSpectator ? 'spectator' : 'game')
    sceneRef.current = scene
    setSceneReady(false)
    scene.mount(stageRef.current).then((mounted) => {
      if (cancelled || !mounted) return
      scene.setVolume(loadStoredVolume())
      scene.setOnConnectionLost(() => {
        const s = socketRef.current
        if (s) s.close()
      })
      // Preload sounds
      const soundFiles = [
        '01-start', '03-cd', '05-draw', '06-discard',
        '08-inquire', '09-cpk', '14-chow-m', '16-pung-m',
        '18-kong-m', '20-win-m', '25-xchg',
      ]
      for (const alias of soundFiles) {
        const audio = new Audio(`/sounds/${alias}.wav`)
        audio.volume = 0.5
        audio.preload = 'auto'
        audio.load()
        scene.loadSound(alias, audio)
      }
      setSceneReady(true)
    })
    return () => {
      cancelled = true
      setSceneReady(false)
      scene.destroy()
      if (sceneRef.current === scene) {
        sceneRef.current = null
      }
    }
  }, [isSpectator])

  useEffect(() => {
    sceneRef.current?.setAppearance(sceneAppearance)
  }, [sceneAppearance])

  useEffect(() => {
    sceneRef.current?.setBackgroundImage(backgroundImage?.dataUrl ?? null)
  }, [backgroundImage?.dataUrl])

  // ── Force PixiJS resize when ratings expand/collapse ──────────
  useEffect(() => {
    const timer = setTimeout(() => sceneRef.current?.forceResize(), 100)
    return () => clearTimeout(timer)
  }, [ratingsExpanded])

  // ── WebSocket ─────────────────────────────────────────────────
  useEffect(() => {
    if (!token || (isSpectator && !sessionIdHint) || !sceneReady) {
      setPhase('loading')
      authoritativeActiveRef.current = false
      return
    }

    let intentionalClose = false
    disposedRef.current = false

    const connect = () => {
      if (disposedRef.current) return

      const url = buildWebSocketUrl(
        isSpectator ? '/ws/spectate' : '/ws/game',
        token,
        sessionIdHint ? { session_id: sessionIdHint } : {},
      )
      const socket = new WebSocket(url)
      socketRef.current = socket

      const scheduleLobbyReturn = () => {
        if (endTimeoutRef.current !== null) return
        intentionalClose = true
        if (!isSpectator) clearStoredSessionId()
        endTimeoutRef.current = setTimeout(() => {
          endTimeoutRef.current = null
          const current = socketRef.current
          if (current && current.readyState === WebSocket.OPEN) {
            current.close()
          }
        }, END_RESULTS_DELAY_MS)
      }

      socket.onopen = () => {
        if (disposedRef.current || socketRef.current !== socket) return
        if (isSpectator) {
          setRequestedHandSeat(null)
        }
      }

      socket.onmessage = (evt) => {
        if (disposedRef.current || socketRef.current !== socket) return

        let env: WsEnvelope<unknown>
        try { env = JSON.parse(evt.data) as WsEnvelope<unknown> }
        catch { notify('收到无法解析的消息'); return }

        if (env.type === 'pong') {
          const payload = env.payload as { identifier?: number } | null
          sceneRef.current?.handleLatencyPong(payload?.identifier)
          return
        }

        if (env.type === 'spectator.hand.pending' && isSpectator) {
          const payload = env.payload as { seat_index?: number }
          if (typeof payload.seat_index === 'number') setRequestedHandSeat(payload.seat_index)
          return
        }

        if (env.type === 'spectator.hand.result' && isSpectator) {
          const payload = env.payload as {
            seat_index?: number
            target_player_id?: number
            approved?: boolean
          }
          setRequestedHandSeat(null)
          if (payload.approved && typeof payload.seat_index === 'number' &&
              typeof payload.target_player_id === 'number') {
            const previous = approvedHandAccessRef.current
            if (previous && previous.target_player_id !== payload.target_player_id) {
              const previousSeat = spectatorSeatsRef.current.find(
                (seat) => seat.player_id === previous.target_player_id,
              )
              if (previousSeat) {
                sceneRef.current?.concealSpectatorHand(
                  previousSeat.seat_index,
                  previousSeat.hand_tile_count,
                  previousSeat.has_drawn_tile,
                )
              }
            }
            syncSpectatorHandAccess({
              target_player_id: payload.target_player_id,
              seat_index: payload.seat_index,
            })
            notify('玩家已同意，现在可以查看他的手牌')
          } else {
            notify('玩家拒绝了看牌申请')
          }
          return
        }

        if (env.type === 'spectator.hand.update' && isSpectator) {
          const payload = env.payload as {
            seat_index?: number
            target_player_id?: number
            stage_counter?: number
            hand_tiles?: number[]
            drawn_tile?: number | null
          }
          if (typeof payload.seat_index === 'number' &&
              typeof payload.target_player_id === 'number' &&
              Array.isArray(payload.hand_tiles)) {
            syncSpectatorHandAccess({
              target_player_id: payload.target_player_id,
              seat_index: payload.seat_index,
            })
            const pendingDraw = pendingAuthorizedDrawEventRef.current
            const sameStage = pendingDraw !== null &&
              payload.stage_counter === pendingDraw.event.stage_counter &&
              payload.seat_index === pendingDraw.event.actor_seat
            const hasPrivateDrawData = sameStage && (
              (pendingDraw.event.kind === 'draw_tile' && typeof payload.drawn_tile === 'number') ||
              (pendingDraw.event.kind === 'predraw' && payload.hand_tiles.length >=
                ((pendingDraw.event.ui64_value ?? 0) < 12 ? 4 : 1))
            )
            if (hasPrivateDrawData) {
              const kind = pendingDraw.event.kind
              const count = kind === 'predraw'
                ? ((pendingDraw.event.ui64_value ?? 0) < 12 ? 4 : 1)
                : 0
              const privateEvent = kind === 'draw_tile'
                ? { ...pendingDraw.event, tile: payload.drawn_tile ?? undefined }
                : {
                    ...pendingDraw.event,
                    drawn_tiles: payload.hand_tiles.slice(-count),
                  }
              pendingAuthorizedDrawEventRef.current = null
              sceneRef.current?.handleEvent({
                ...pendingDraw,
                event: privateEvent,
                reveal_all_hands: true,
              })
              if (kind === 'predraw' && pendingDraw.event.ui64_value === 15) {
                spectatorPredrawInProgressRef.current = false
              }
            } else {
              if (pendingDraw !== null && typeof payload.stage_counter === 'number' &&
                  payload.stage_counter < pendingDraw.event.stage_counter) {
                return
              }
              if (pendingDraw !== null && typeof payload.stage_counter === 'number' &&
                  payload.stage_counter >= pendingDraw.event.stage_counter) {
                pendingAuthorizedDrawEventRef.current = null
              }
              sceneRef.current?.revealSpectatorHand(
                payload.seat_index,
                payload.hand_tiles,
                typeof payload.drawn_tile === 'number' ? payload.drawn_tile : null,
                !spectatorPredrawInProgressRef.current,
              )
            }
          }
          return
        }

        if (env.type === 'spectator.hand.revoked' && isSpectator) {
          const payload = env.payload as { target_player_id?: number }
          const previous = approvedHandAccessRef.current
          if (previous && payload.target_player_id === previous.target_player_id) {
            const previousSeat = spectatorSeatsRef.current.find(
              (seat) => seat.player_id === previous.target_player_id,
            )
            if (previousSeat) {
              sceneRef.current?.concealSpectatorHand(
                previousSeat.seat_index,
                previousSeat.hand_tile_count,
                previousSeat.has_drawn_tile,
              )
            }
            syncSpectatorHandAccess(null)
            notify('玩家已取消看牌许可')
          }
          return
        }

        if (env.type === 'spectator.hand.management' && !isSpectator) {
          const payload = env.payload as { entries?: unknown[] }
          const receivedAt = Date.now()
          const entries = Array.isArray(payload.entries)
            ? payload.entries.flatMap((value): SpectatorManagementEntry[] => {
              const entry = value as Record<string, unknown>
              if ((entry.status !== 'pending' && entry.status !== 'approved') ||
                typeof entry.spectator_player_id !== 'number' ||
                typeof entry.spectator_username !== 'string') {
                return []
              }
              if (entry.status === 'pending') {
                if (typeof entry.request_id !== 'string' ||
                  typeof entry.expires_in_ms !== 'number') {
                  return []
                }
                return [{
                  status: 'pending',
                  spectator_player_id: entry.spectator_player_id,
                  spectator_username: entry.spectator_username,
                  request_id: entry.request_id,
                  expires_at_ms: receivedAt + Math.max(0, entry.expires_in_ms),
                }]
              }
              return [{
                status: 'approved',
                spectator_player_id: entry.spectator_player_id,
                spectator_username: entry.spectator_username,
              }]
            })
            : []
          const pendingRequestIds = new Set<string>(
            entries.filter((entry) => entry.status === 'pending' && entry.request_id !== undefined)
              .map((entry) => entry.request_id as string),
          )
          for (const entry of entries) {
            if (entry.status === 'pending' && entry.request_id !== undefined &&
                !spectatorManagementPendingIdsRef.current.has(entry.request_id)) {
              notify(`${entry.spectator_username} 申请查看你的手牌`)
            }
          }
          spectatorManagementPendingIdsRef.current = pendingRequestIds
          setSpectatorManagement(entries)
          return
        }

        if (env.type === 'game.abandon.notify') {
          const payload = env.payload as { player_id?: number; username?: string; abandoned?: boolean }
          if (typeof payload.username === 'string' && typeof payload.abandoned === 'boolean') {
            notify(`${payload.username} ${payload.abandoned ? '已放弃游戏' : '已返回游戏'}`)
          }
          if (typeof payload.player_id === 'number' && payload.player_id === auth?.player.player_id) {
            setMyAbandoned(payload.abandoned === true)
          }
          return
        }

        // ── session.snapshot ──────────────────────────────────
        if (env.type === 'session.snapshot') {
          pendingAuthorizedDrawEventRef.current = null
          const snap = env.payload as SessionSnapshot
          if (snap.phase === 'pending') {
            authoritativeActiveRef.current = false
            setPendingSnapshot(snap)
            setPhase('pending')
            lastScRef.current = -1
            setAbandonEnabled(snap.summary.abandon_game !== false)
            setDuplicateMode(snap.summary.duplicate_mode === true)
            setMyAbandoned(false)
            // Capture ratings for pending phase sidebar
            const pRatings = (snap as unknown as Record<string, unknown>)?.ratings
            if (Array.isArray(pRatings)) {
              setPendingRatings(pRatings as RatingCard[])
            }
          } else {
            authoritativeActiveRef.current = true
            setPendingSnapshot(null)
            setPhase('active')
            lastScRef.current = snap.state.stage_counter
            setAbandonEnabled(snap.abandon_game !== false)
            setDuplicateMode(snap.duplicate_mode === true)
            const ownSeat = snap.seats.find(
              (seat) => seat.seat_index === snap.viewer.seat_index,
            )
            setMyAbandoned(ownSeat?.abandoned === true)
            spectatorPredrawInProgressRef.current = snap.state.last_event_kind === 'start' ||
              (snap.state.last_event_kind === 'predraw' &&
                (snap.result_event?.ui64_value ?? 15) < 15)
            const sid = snap.session_id
            if (sid) {
              if (!isSpectator) saveStoredSessionId(sid)
              setSessionIdHint(sid)
            }
            sceneRef.current?.flushFromSnapshot(snap)
            if (isSpectator) {
              const seats = snap.seats.map((seat) => ({
                seat_index: seat.seat_index,
                player_id: seat.player_id,
                username: seat.username,
                hand_tile_count: seat.hand_tile_count,
                has_drawn_tile: seat.has_drawn_tile,
              }))
              spectatorSeatsRef.current = seats
              setSpectatorSeats(seats)
              setSpectatorPerspectiveSeat(snap.viewer.seat_index)
              spectatorPerspectiveSeatRef.current = snap.viewer.seat_index
              spectatorPerspectivePlayerIdRef.current =
                seats.find((seat) => seat.seat_index === snap.viewer.seat_index)?.player_id ?? null
              const access = snap.hand_access
              syncSpectatorHandAccess(
                access?.granted && typeof access.target_player_id === 'number' &&
                    typeof access.seat_index === 'number'
                  ? {
                      target_player_id: access.target_player_id,
                      seat_index: access.seat_index,
                    }
                  : null,
              )
            }
            // Restart periodic ping after a successful reconnect
            sceneRef.current?.restartPeriodicPing()
            // Capture ratings from snapshot (resume path)
            const snapRatings = (env.payload as Record<string, unknown>)?.ratings
            if (Array.isArray(snapRatings)) {
              setGameRatings(snapRatings as Array<{ player_id: number; mu?: number; tau?: number; sigma?: number; points?: number; level?: number }>)
            }
            if (snap.state.ended) {
              gameEndedRef.current = true
              scheduleLobbyReturn()
            }
          }
          return
        }

        // ── game.event ────────────────────────────────────────
        if (env.type === 'game.event') {
          const payload = env.payload as GameEventPayload
          const isAuthoritativeStart = payload.category === 'transition' && payload.event.kind === 'start'
          const isAuthoritativeEnd = payload.category === 'transition' && payload.event.kind === 'end'
          if (!authoritativeActiveRef.current && !isAuthoritativeStart && !isAuthoritativeEnd) {
            return
          }
          if (isAuthoritativeStart || isAuthoritativeEnd) {
            authoritativeActiveRef.current = true
          }
          if (isSpectator && isAuthoritativeStart) {
            const perspectivePlayerId = spectatorPerspectivePlayerIdRef.current
            const perspectiveSeat = payload.seat_status.find(
              (seat) => seat.player_id === perspectivePlayerId,
            )
            if (perspectivePlayerId !== null &&
                perspectiveSeat !== undefined &&
                perspectiveSeat.seat_index !== spectatorPerspectiveSeatRef.current) {
              sendEnvelope(socket, 'spectator.perspective', {
                seat_index: perspectiveSeat.seat_index,
              })
            }
          }
          const sc = payload.event.stage_counter

          if (sc < lastScRef.current) return // stale
          lastScRef.current = sc
          setPendingSnapshot(null)
          setPhase('active')

          if (payload.category === 'transition' && payload.event.kind === 'player_left') {
            sceneRef.current?.handlePlayerLeft(payload.event.actor_seat)
          }
          if (payload.category === 'transition' && payload.event.kind === 'player_resumed') {
            sceneRef.current?.handlePlayerResumed(payload.event.actor_seat)
          }
          const accessBeforeEvent = approvedHandAccessRef.current
          const actorBeforeEvent = payload.seat_status.find(
            (seat) => seat.seat_index === payload.event.actor_seat,
          )
          const authorizedPrivateDraw = isSpectator &&
            payload.category === 'transition' &&
            actorBeforeEvent?.player_id === accessBeforeEvent?.target_player_id &&
            PRIVATE_DRAW_EVENTS.has(payload.event.kind)
          if (isSpectator && isAuthoritativeStart) {
            spectatorPredrawInProgressRef.current = true
          }
          if (authorizedPrivateDraw) {
            pendingAuthorizedDrawEventRef.current = payload
          } else {
            sceneRef.current?.handleEvent(payload)
            if (isSpectator && payload.event.kind === 'predraw' &&
                payload.event.ui64_value === 15) {
              spectatorPredrawInProgressRef.current = false
            }
          }
          if (isSpectator) {
            const seats = spectatorSeatsRef.current.map((seat) => {
              const latest = payload.seat_status.find((item) => item.seat_index === seat.seat_index)
              return latest
                ? {
                    ...seat,
                    player_id: latest.player_id ?? seat.player_id,
                    username: latest.username ?? seat.username,
                    hand_tile_count: latest.hand_tile_count,
                    has_drawn_tile: latest.has_drawn_tile,
                  }
                : seat
            })
            spectatorSeatsRef.current = seats
            setSpectatorSeats(seats)
            const access = approvedHandAccessRef.current
            if (access) {
              const targetSeat = seats.find(
                (seat) => seat.player_id === access.target_player_id,
              )
              if (targetSeat) {
                syncSpectatorHandAccess({
                  target_player_id: access.target_player_id,
                  seat_index: targetSeat.seat_index,
                })
              }
              if (authorizedPrivateDraw) {
                sendEnvelope(socket, 'spectator.hand.refresh', {})
              }
            }
          }
          // Capture ratings from start/end events
          const ratingsPayload = (env.payload as Record<string, unknown>)?.ratings
          if (Array.isArray(ratingsPayload)) {
            setGameRatings(ratingsPayload as Array<{ player_id: number; mu?: number; tau?: number; sigma?: number; points?: number; level?: number }>)
          }
          const finalRatingsPayload = (env.payload as Record<string, unknown>)?.final_ratings
          if (isAuthoritativeEnd && Array.isArray(finalRatingsPayload) && Array.isArray(ratingsPayload)) {
            const rArr = ratingsPayload as Array<{ player_id: number; mu?: number; sigma?: number; points?: number; level?: number }>
            const fArr = finalRatingsPayload as Array<{ player_id: number; mu?: number; sigma?: number; points?: number; level?: number }>
            const myId = auth?.player.player_id
            if (!isSpectator && myId) {
              const initMe = rArr.find(r => r.player_id === myId)
              const finalMe = fArr.find(r => r.player_id === myId)
              if (initMe && finalMe) {
                const rankChanged = (initMe.level ?? 0) !== (finalMe.level ?? 0)
                const deltaMu = (finalMe.mu ?? 0) - (initMe.mu ?? 0)
                const deltaPoints = (finalMe.points ?? 0) - (initMe.points ?? 0)
                sceneRef.current?.showRatingResult(
                  rankChanged,
                  rankToChinese(initMe.level ?? 0),
                  rankToChinese(finalMe.level ?? 0),
                  deltaMu,
                  deltaPoints,
                )
              }
            }
          }
          if (isAuthoritativeEnd) {
            gameEndedRef.current = true
            scheduleLobbyReturn()
          }
          return
        }

        if (env.type === 'game.pass.ack' || env.type === 'pass.ack') {
          sceneRef.current?.handlePassAck(env.payload as PassAckPayload)
          return
        }

        if (env.type === 'rating.update') {
          const payload = env.payload as {
            initial: Array<{ player_id: number; level?: number }>
            final: Array<{ player_id: number; level?: number }>
            deltas: Array<{ player_id: number; delta_mu: number; delta_points: number }>
          }
          const myId = auth?.player.player_id
          if (myId && payload.initial && payload.final && payload.deltas) {
            const initMe = payload.initial.find(r => r.player_id === myId)
            const finalMe = payload.final.find(r => r.player_id === myId)
            const deltaMe = payload.deltas.find(r => r.player_id === myId)
            if (initMe && finalMe && deltaMe) {
              const rankChanged = (initMe.level ?? 0) !== (finalMe.level ?? 0)
              sceneRef.current?.showRatingResult(
                rankChanged,
                rankToChinese(initMe.level ?? 0),
                rankToChinese(finalMe.level ?? 0),
                deltaMe.delta_mu,
                deltaMe.delta_points,
              )
            }
          }
          return
        }

        if (env.type === 'error') {
          const errPayload = env.payload as { code?: string; message?: string }
          const msg = errPayload.message ?? '牌桌错误'
          notify(msg)
          if (isSpectator) setRequestedHandSeat(null)
          if (errPayload.code === 'not_found') {
            clearStoredSessionId()
            setSessionIdHint(null)
            authoritativeActiveRef.current = false
          }
          if (errPayload.code === 'unauthorized' || errPayload.code === 'kicked') {
            clearStoredAuth()
            clearStoredSessionId()
          }
        }

        // ── resume.required ────────────────────────────────────
        if (env.type === 'resume.required') {
          const s = socketRef.current
          if (s && s.readyState === WebSocket.OPEN) {
            sendEnvelope(s, 'resume.ack', {})
          }
          return
        }

        // ── rating.update ──────────────────────────────────────
        if (env.type === 'rating.update') {
          const rp = env.payload as { deltas?: { player_id: number; delta_mu: number; delta_points: number }[] }
          const me = auth?.player.player_id
          if (me && rp.deltas && Array.isArray(rp.deltas)) {
            const myDelta = rp.deltas.find(d => d.player_id === me)
            if (myDelta) {
              const muSign = myDelta.delta_mu >= 0 ? '+' : ''
              const ptSign = myDelta.delta_points >= 0 ? '+' : ''
              sceneRef.current?.showRatingUpdate(
                `等级分 ${muSign}${myDelta.delta_mu.toFixed(2)}  积分 ${ptSign}${myDelta.delta_points.toFixed(2)}`
              )
            }
          }
          return
        }
      }

      socket.onerror = () => { /* handled by onclose */ }

      socket.onclose = () => {
        if (socketRef.current === socket) socketRef.current = null
        if (disposedRef.current || intentionalClose || gameEndedRef.current) return
        if (isSpectator) {
          const access = approvedHandAccessRef.current
          const previous = spectatorSeatsRef.current.find(
            (seat) => seat.player_id === access?.target_player_id,
          )
          if (access && previous) {
            sceneRef.current?.concealSpectatorHand(
              previous.seat_index, previous.hand_tile_count, previous.has_drawn_tile,
            )
          }
          syncSpectatorHandAccess(null)
          setRequestedHandSeat(null)
        }
        setPhase('loading')
        notify('连接已断开，正在重连……')
        let retries = 0
        const tryReconnect = () => {
          if (disposedRef.current || gameEndedRef.current) return
          retries++
          const delay = Math.min(1000 * retries, 8000)
          setTimeout(connect, delay)
        }
        tryReconnect()
      }
    }

    connect()

    return () => {
      disposedRef.current = true
      intentionalClose = true
      if (endTimeoutRef.current !== null) {
        clearTimeout(endTimeoutRef.current)
        endTimeoutRef.current = null
      }
      socketRef.current?.close()
      socketRef.current = null
    }
  }, [sessionIdHint, token, sceneReady, isSpectator])

  useEffect(() => {
    if (!abandonConfirmOpen) return
    const timeout = window.setTimeout(() => setAbandonConfirmOpen(false), 5000)
    return () => window.clearTimeout(timeout)
  }, [abandonConfirmOpen])

  useEffect(() => {
    sceneRef.current?.setInteractionPaused(abandonConfirmOpen)
    return () => {
      sceneRef.current?.setInteractionPaused(false)
    }
  }, [abandonConfirmOpen])

  useEffect(() => {
    if (isSpectator) return
    const nextExpiry = spectatorManagement.reduce<number | null>((nearest, entry) => {
      if (entry.status !== 'pending' || entry.expires_at_ms === undefined) return nearest
      return nearest === null ? entry.expires_at_ms : Math.min(nearest, entry.expires_at_ms)
    }, null)
    if (nextExpiry === null) return
    const timeout = window.setTimeout(() => {
      const now = Date.now()
      setSpectatorManagement((current) => current.filter(
        (entry) => entry.status === 'approved' ||
          entry.expires_at_ms === undefined || entry.expires_at_ms > now,
      ))
    }, Math.max(0, nextExpiry - Date.now()))
    return () => window.clearTimeout(timeout)
  }, [isSpectator, spectatorManagement])

  // ── Helpers ──────────────────────────────────────────────────
  function notify(msg: string) { setNotification(msg); setShowNotif(true); setTimeout(() => setShowNotif(false), 3000) }

  function syncSpectatorHandAccess(access: SpectatorHandAccess | null) {
    if (access === null) pendingAuthorizedDrawEventRef.current = null
    approvedHandAccessRef.current = access
    setApprovedHandAccess(access)
  }

  function requestSpectatorHand(seatIndex: number) {
    const socket = socketRef.current
    if (!isSpectator || !socket || socket.readyState !== WebSocket.OPEN) return
    sendEnvelope(socket, 'spectator.hand.request', { seat_index: seatIndex })
    setRequestedHandSeat(seatIndex)
  }

  function changeSpectatorPerspective(seatIndex: number) {
    const socket = socketRef.current
    if (!isSpectator || !socket || socket.readyState !== WebSocket.OPEN) return
    spectatorPerspectivePlayerIdRef.current =
      spectatorSeatsRef.current.find((seat) => seat.seat_index === seatIndex)?.player_id ?? null
    sendEnvelope(socket, 'spectator.perspective', { seat_index: seatIndex })
  }

  function respondToSpectatorHand(request: SpectatorManagementEntry, approved: boolean) {
    const socket = socketRef.current
    if (!socket || socket.readyState !== WebSocket.OPEN || !request.request_id) return
    sendEnvelope(socket, 'spectator.hand.respond', {
      request_id: request.request_id,
      approved,
    })
    setSpectatorManagement((current) => (
      current.filter((item) => item.request_id !== request.request_id)
    ))
  }

  function revokeSpectatorHand(entry: SpectatorManagementEntry) {
    const socket = socketRef.current
    if (!socket || socket.readyState !== WebSocket.OPEN || sessionIdHint == null) return
    sendEnvelope(socket, 'spectator.hand.revoke', {
      session_id: sessionIdHint,
      spectator_player_id: entry.spectator_player_id,
    })
    setSpectatorManagement((current) => current.filter(
      (item) => item.spectator_player_id !== entry.spectator_player_id ||
        item.status !== 'approved',
    ))
  }

  function openAbandonConfirm() {
    if (!abandonEnabled || myAbandoned) return
    setAbandonConfirmOpen(true)
  }

  function confirmAbandon() {
    setAbandonConfirmOpen(false)
    const socket = socketRef.current
    if (!socket || socket.readyState !== WebSocket.OPEN) return
    sendEnvelope(socket, 'game.abandon', { abandon: true })
    setMyAbandoned(true)
  }

  function revertAbandon() {
    const socket = socketRef.current
    if (!socket || socket.readyState !== WebSocket.OPEN) return
    sendEnvelope(socket, 'game.abandon', { abandon: false })
    setMyAbandoned(false)
  }

  const isVirtualPlayer = (playerId: number) => playerId <= 0

  const sortRatingCards = (cards: RatingCard[]) => {
    return [...cards].sort((left, right) => {
      const leftVirtual = isVirtualPlayer(left.player_id)
      const rightVirtual = isVirtualPlayer(right.player_id)
      if (leftVirtual !== rightVirtual) {
        return leftVirtual ? 1 : -1
      }
      return left.player_id - right.player_id
    })
  }

  const sidebarCards: RatingCard[] = (() => {
    if (phase === 'pending' && pendingSnapshot) {
      const pendingCards = pendingSnapshot.seats
        .filter((seat) => seat.player_id !== null)
        .map((seat) => {
          const playerId = seat.player_id as number
          const fromRatings = pendingRatings.find((rating) => rating.player_id === playerId)
          return {
            player_id: playerId,
            username: seat.username ?? fromRatings?.username,
            mu: fromRatings?.mu,
            tau: fromRatings?.tau,
            sigma: fromRatings?.sigma,
            points: fromRatings?.points,
            level: fromRatings?.level,
          }
        })
      return sortRatingCards(pendingCards)
    }
    if (isSpectator && spectatorSeats.length > 0) {
      const spectatorCards = spectatorSeats
        .filter((seat) => seat.player_id !== null)
        .map((seat) => {
          const playerId = seat.player_id as number
          const fromRatings = gameRatings.find((rating) => rating.player_id === playerId)
          return {
            player_id: playerId,
            username: seat.username ?? fromRatings?.username,
            mu: fromRatings?.mu,
            tau: fromRatings?.tau,
            sigma: fromRatings?.sigma,
            points: fromRatings?.points,
            level: fromRatings?.level,
          }
        })
      return sortRatingCards(spectatorCards)
    }
    return sortRatingCards(gameRatings)
  })()

  function sendReady(ready: boolean) {
    const s = socketRef.current
    if (!s || s.readyState !== WebSocket.OPEN || !sessionIdHint) return
    sendEnvelope(s, 'queue.ready', { session_id: sessionIdHint, ready })
  }

  function chooseSeat(seatIndex: number) {
    const s = socketRef.current
    if (!s || s.readyState !== WebSocket.OPEN || !sessionIdHint) return
    sendEnvelope(s, 'queue.choose_seat', { session_id: sessionIdHint, seat_index: seatIndex })
  }

  // ── Pending phase: show waiting room in scene ────────────────
  useEffect(() => {
    if (phase !== 'pending' || !pendingSnapshot) return
    const own = pendingSnapshot.seats.find(
      (s: { player_id: number | null }) => s.player_id === auth?.player.player_id,
    )
    const isDuplicatePending = Boolean(pendingSnapshot.summary.duplicate_mode)
    const members = pendingSnapshot.members ?? []
    const ownSeat = isDuplicatePending
      ? (members.find((m) => m.player_id === auth?.player.player_id)?.seat_index ?? null)
      : null
    sceneRef.current?.showPending(
      pendingSnapshot.seats,
      own?.ready ?? false,
      () => {
        if (isDuplicatePending && ownSeat === null) {
          notify('请选择座位')
          return
        }
        sendReady(!(own?.ready ?? false))
      },
      isDuplicatePending
        ? {
            members,
            ownPlayerId: auth?.player.player_id ?? null,
            ownSeat,
            onChooseSeat: chooseSeat,
          }
        : null,
    )
  }, [phase, pendingSnapshot, auth?.player.player_id])

  // ── No token ─────────────────────────────────────────────────
  if (!token) {
    return (
      <div className="game-blocked">
        <div className="game-blocked-card">
          <h1>需要先登录</h1>
          <p>当前没有可用的会话，无法进入牌局。</p>
          <div className="game-blocked-actions">
            <button onClick={() => navigate('/')}>返回大厅</button>
            <button onClick={() => window.location.reload()}>重新检查</button>
          </div>
        </div>
      </div>
    )
  }

  if (isSpectator && !sessionIdHint) {
    return (
      <div className="game-blocked">
        <div className="game-blocked-card">
          <h1>找不到牌桌</h1>
          <p>请从大厅选择一场进行中的牌局。</p>
          <div className="game-blocked-actions">
            <button onClick={() => navigate('/')}>返回大厅</button>
          </div>
        </div>
      </div>
    )
  }

  // ── Render ───────────────────────────────────────────────────
  return (
    <div className="mahjongGame" style={{ background: sceneAppearance.backgroundColorOutside }}>
      {abandonConfirmOpen && (
        <div
          className="game-abandon-confirm"
          onPointerDown={(e) => e.stopPropagation()}
          onPointerUp={(e) => e.stopPropagation()}
          onClick={(e) => e.stopPropagation()}
          onContextMenu={(e) => { e.preventDefault(); e.stopPropagation() }}
          onWheel={(e) => { e.preventDefault(); e.stopPropagation() }}
        >
          <div className="game-abandon-confirm-card">
            <p>确定要放弃本局游戏吗？</p>
            <div>
              <button type="button" onClick={() => setAbandonConfirmOpen(false)}>取消</button>
              <button type="button" className="is-danger" onClick={confirmAbandon}>放弃游戏</button>
            </div>
          </div>
        </div>
      )}
      {/* {phase === 'loading' && <div className="game-loading">连接牌桌中…</div>} */}
      <div className="game-page__layout" style={{ background: sceneAppearance.backgroundColorOutside }}>
        <section className="game-page__board-panel">
          <div className="game-page__stage-shell" style={{ background: sceneAppearance.backgroundColorTable }}>
            <div ref={stageRef} className="game-stage" />
            {showNotif && <div className="game-notification">{notification}</div>}
            {phase === 'loading' && (
              <div className="replay-stage-overlay">
                {sceneReady ? '连接牌桌中…' : '正在加载中…'}
              </div>
            )}
          </div>
        </section>
        <aside className="game-page__sidebar">
          <div className={`game-page__ratings-area${ratingsExpanded ? ' is-expanded' : ''}`}>
            {sidebarCards.length > 0 && sidebarCards.map((r) => {
              const seat = isSpectator
                ? spectatorSeats.find((item) => item.player_id === r.player_id)
                : undefined
              const isApproved = approvedHandAccess?.target_player_id === r.player_id
              const isPending = seat !== undefined && requestedHandSeat === seat.seat_index
              const isPerspective = seat !== undefined && spectatorPerspectiveSeat === seat.seat_index
              return (
                <div className="game-page__sidebar-card" key={r.player_id}>
                  <div className="player-name">
                    {r.username || (r.player_id > 0 ? `#${r.player_id}` : 'N/A')}
                  </div>
                  {r.points !== undefined && (
                    <div className="player-rating">
                      {rankToChinese(r.level ?? 0)} · {r.points.toFixed(2)}pts
                    </div>
                  )}
                  {r.mu !== undefined && (
                    <div className="player-rating">
                      R {r.mu.toFixed(2)}±{r.tau?.toFixed(2)} · σ {r.sigma?.toFixed(2)}
                    </div>
                  )}
                  {isSpectator && seat !== undefined && (
                    <div className="spectator-player-actions">
                      {r.player_id > 0 && (
                        <button
                          type="button"
                          className={`spectator-hand-request${isApproved ? ' is-approved' : ''}`}
                          disabled={isApproved || requestedHandSeat !== null}
                          onClick={() => requestSpectatorHand(seat.seat_index)}
                        >
                          {isApproved ? '已允许看牌' : isPending ? '等待同意…' : '申请看牌'}
                        </button>
                      )}
                      <button
                        type="button"
                        className={`spectator-perspective-button${isPerspective ? ' is-active' : ''}`}
                        disabled={isPerspective}
                        onClick={() => changeSpectatorPerspective(seat.seat_index)}
                      >
                        {isPerspective ? '当前视角' : '切换视角'}
                      </button>
                    </div>
                  )}
                </div>
              )
            })}
          </div>
          <div className="game-page__sidebar-bottom-stack">
            {phase === 'active' && !isSpectator && !duplicateMode && (
              <div className="spectator-management-row">
                {abandonEnabled && (
                  <button
                    type="button"
                    className={`game-abandon-button${myAbandoned ? ' is-abandoned' : ''}`}
                    onClick={myAbandoned ? revertAbandon : openAbandonConfirm}
                  >
                    {myAbandoned ? '取消放弃' : '放弃游戏'}
                  </button>
                )}
                <button
                  type="button"
                  className="scene-appearance-toggle__button"
                  aria-expanded={spectatorManagementOpen}
                  onClick={() => {
                    if (appearancePanelOpen) setAppearancePanelOpen(false)
                    setSpectatorManagementOpen((value) => !value)
                  }}
                >
                  观战管理{spectatorManagement.length > 0 ? ` (${spectatorManagement.length})` : ''}
                </button>
              </div>
            )}
            <div className="game-page__sidebar-bottom-row">
              {sidebarCards.length > 0 && (
                <button
                  type="button"
                  className="scene-appearance-toggle__button"
                  aria-expanded={ratingsExpanded}
                  onClick={() => setRatingsExpanded((v) => !v)}
                >
                  {'玩家信息'}
                </button>
              )}
              <button
                type="button"
                className="scene-appearance-toggle__button"
                aria-expanded={appearancePanelOpen}
                onClick={() => {
                  if (spectatorManagementOpen) setSpectatorManagementOpen(false)
                  setAppearancePanelOpen((value) => !value)
                }}
              >
                设置
              </button>
            </div>
            {spectatorManagementOpen && phase === 'active' && (
            <div className="scene-appearance-toggle__panel spectator-management-panel">
              <div className="scene-appearance-toggle__card spectator-management-card">
                <strong>观战管理</strong>
                {spectatorManagement.length === 0 ? (
                  <p className="spectator-management-empty">暂无看牌申请或许可</p>
                ) : (
                  <div className="spectator-management-list">
                    {spectatorManagement.map((entry) => (
                      <div
                        className="spectator-management-entry"
                        key={`${entry.status}-${entry.spectator_player_id}-${entry.request_id ?? ''}`}
                      >
                        <div>
                          <strong>{entry.spectator_username}</strong>
                          <span>{entry.status === 'pending' ? '申请看牌' : '已允许看牌'}</span>
                        </div>
                        <div className="spectator-management-actions">
                          {entry.status === 'pending' ? (
                            <>
                              <button type="button" onClick={() => respondToSpectatorHand(entry, true)}>允许</button>
                              <button type="button" onClick={() => respondToSpectatorHand(entry, false)}>拒绝</button>
                            </>
                          ) : (
                            <button type="button" onClick={() => revokeSpectatorHand(entry)}>取消许可</button>
                          )}
                        </div>
                      </div>
                    ))}
                  </div>
                )}
              </div>
            </div>
          )}
          {appearancePanelOpen && (
            <div className="scene-appearance-toggle__panel">
              <div className="scene-appearance-toggle__card">
                <SceneAppearancePanel
                  title="设置"
                  appearance={sceneAppearance}
                  backgroundImageName={backgroundImage?.name ?? null}
                  backgroundImageLoading={backgroundImageLoading}
                  volume={volume}
                  onVolumeChange={(v) => { setVolumeState(v); saveStoredVolume(v); sceneRef.current?.setVolume(v) }}
                  onBackgroundColorTableChange={setBackgroundColorTable}
                  onBackgroundColorOutsideChange={setBackgroundColorOutside}
                  onBackgroundImageEnabledChange={setBackgroundImageEnabled}
                  onBackgroundImageAlphaChange={setBackgroundImageAlpha}
                  onBackgroundImageSelected={uploadBackgroundImage}
                  onBackgroundImageCleared={clearBackgroundImage}
                  onTileCoverColorChange={setTileCoverColor}
                  onAddTileCoverColor={addTileCoverColor}
                  onRemoveTileCoverColor={removeTileCoverColor}
                  onReset={resetAppearance}
                />
              </div>
            </div>
          )}
          </div>
        </aside>
      </div>
    </div>
  )
}
