import { useEffect, useMemo, useRef, useState } from 'react'
import { useLocation, useNavigate } from 'react-router-dom'

// import SceneAppearancePanel from '../components/SceneAppearancePanel'
import { MahjongScene } from '../game/scene/MahjongScene'
import { apiRequest, buildWebSocketUrl } from '../lib/backend'
import { loadStoredVolume } from '../lib/storage'
import { useStoredSceneAppearance } from '../lib/useStoredSceneAppearance'
import {
  buildReplayTimeline,
  clampReplayDelay,
  findFinalTransitionIndex,
  materializeReplayPayload,
  materializeReplaySnapshot,
  parseWatchingSeat,
  type ReplayTimelineEntry,
} from '../lib/replay'
import type { ReplaySessionPayload, WsEnvelope } from '../lib/types'
import { rankToChinese } from '../lib/chinese'
import './GamePage.css'
import './ReplayPage.css'

function readRound(search: string): number {
  const value = new URLSearchParams(search).get('round')
  const parsed = Number(value)
  return Number.isFinite(parsed) && parsed > 0 ? parsed : 1
}

function normalizeReplaySeedFragment(seed: string | number | null | undefined): string {
  if (seed == null) {
    return ''
  }
  if (typeof seed === 'string') {
    return seed.replace(/^0x/i, '')
  }
  return seed.toString(16)
}

function findFirstReplayActionIndex(timeline: ReplayTimelineEntry[]): number {
  if (timeline.length === 0) {
    return 0
  }

  const drawIndex = timeline.findIndex((entry, entryIndex) => {
    if (entryIndex === 0 || !entry.event) {
      return false
    }
    return entry.event.kind === 'draw_tile'
  })

  if (drawIndex >= 0) { 
    return drawIndex
  }

  return Math.min(timeline.length - 1, 1)
}

function findDefaultReplayEntryIndex(timeline: ReplayTimelineEntry[]): number {
  return findFirstReplayActionIndex(timeline)
}

export default function ReplayPage() {
  const location = useLocation()
  const navigate = useNavigate()

  const searchParams = useMemo(() => new URLSearchParams(location.search), [location.search])
  const sessionIdentifier = searchParams.get('session')?.trim() ?? ''
  const initialRoundNumber = readRound(location.search)
  const initialWatchingSeat = parseWatchingSeat(searchParams.get('perspective') ?? searchParams.get('watching'))

  const [sceneReady, setSceneReady] = useState(false)
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState('')
  const [sessionPayload, setSessionPayload] = useState<ReplaySessionPayload | null>(null)
  const [timeline, setTimeline] = useState<ReplayTimelineEntry[]>([])
  const [currentIndex, setCurrentIndex] = useState(0)
  const [playing, setPlaying] = useState(false)
  const [hideOtherHands, setHideOtherHands] = useState(false)
  // const [appearancePanelOpen, setAppearancePanelOpen] = useState(false)
  const [requestedRoundNumber, setRequestedRoundNumber] = useState(initialRoundNumber)
  const [watchingSeat, setWatchingSeat] = useState(initialWatchingSeat)
  const {
    appearance: sceneAppearance,
    backgroundImage,
    // backgroundImageLoading,
    // setBackgroundColorTable,
    // setBackgroundColorOutside,
    // setBackgroundImageEnabled,
    // setBackgroundImageAlpha,
    // setTileCoverColor,
    // addTileCoverColor,
    // removeTileCoverColor,
    // uploadBackgroundImage,
    // clearBackgroundImage,
    // resetAppearance,
  } = useStoredSceneAppearance()

  const stageRef = useRef<HTMLDivElement | null>(null)
  const sceneRef = useRef<MahjongScene | null>(null)
  const playbackTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const pollTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const currentIndexRef = useRef(0)
  const watchingSeatRef = useRef(watchingSeat)
  const revealAllHandsRef = useRef(!hideOtherHands)

  const roundRecords = sessionPayload?.round_records ?? []
  const roundCount = sessionPayload?.round_count ?? Math.max(roundRecords.length, 1)
  const selectedRoundNumber = Math.min(Math.max(requestedRoundNumber, 1), Math.max(roundCount, 1))
  const activeRoundRecord = roundRecords[selectedRoundNumber - 1] ?? null
  const canMoveBackward = currentIndex > 0
  const canMoveForward = currentIndex < timeline.length - 1
  const revealAllHands = !hideOtherHands
  const finalTransitionIndex = findFinalTransitionIndex(timeline)
  const firstPlayableIndex = findFirstReplayActionIndex(timeline)
  const currentTimelineEntry = timeline[currentIndex] ?? null
  const currentRoundTurn = currentTimelineEntry?.event?.round_turn ?? (activeRoundRecord ? 0 : null)
  const totalRoundTurn =
    activeRoundRecord?.round_result?.total_turn ??
    activeRoundRecord?.transition_queue[activeRoundRecord.transition_queue.length - 1]?.round_total_turn ??
    null

  const wallSeeds = activeRoundRecord?.round_start_snapshot.wall_seeds ?? []
  const wallSeedCopyText = wallSeeds.map((seed) => normalizeReplaySeedFragment(seed)).join('')
  const recordVersion = activeRoundRecord?.version ?? 0
  const replayRatings = (activeRoundRecord as unknown as Record<string, unknown> | null)?.ratings as Array<{ player_id: number; username?: string; mu?: number; tau?: number; sigma?: number; points?: number; level?: number }> | undefined
  const finalReplayRatings = (activeRoundRecord as unknown as Record<string, unknown> | null)?.final_ratings as Array<{ player_id: number; username?: string; mu?: number; tau?: number; sigma?: number; points?: number; level?: number }> | undefined

  // Show rating result when kEnd is reached in replay
  useEffect(() => {
    if (!sceneRef.current || !replayRatings || !finalReplayRatings) return
    const entry = timeline[currentIndex]
    if (!entry || entry.event?.kind !== 'end') return

    const initSeats = activeRoundRecord?.initial_seats ?? []
    const watchingPlayerId = initSeats[watchingSeat]?.player_id ?? null
    if (watchingPlayerId == null) return

    const initMe = replayRatings.find(r => r.player_id === watchingPlayerId)
    const finalMe = finalReplayRatings.find(r => r.player_id === watchingPlayerId)
    if (!initMe || !finalMe) return

    const rankChanged = (initMe.level ?? 0) !== (finalMe.level ?? 0)
    const deltaMu = (finalMe.mu ?? 0) - (initMe.mu ?? 0)
    const deltaPoints = (finalMe.points ?? 0) - (initMe.points ?? 0)
    sceneRef.current.showRatingResult(
      rankChanged,
      rankToChinese(initMe.level ?? 0),
      rankToChinese(finalMe.level ?? 0),
      deltaMu,
      deltaPoints,
    )
  }, [currentIndex, timeline, watchingSeat, replayRatings, finalReplayRatings, activeRoundRecord])

  function stopPlayback() {
    if (playbackTimeoutRef.current !== null) {
      clearTimeout(playbackTimeoutRef.current)
      playbackTimeoutRef.current = null
    }
    setPlaying(false)
  }

  useEffect(() => {
    stopPlayback()
    setRequestedRoundNumber(initialRoundNumber)
    setWatchingSeat(initialWatchingSeat)
  }, [sessionIdentifier])

  useEffect(() => {
    watchingSeatRef.current = watchingSeat
    revealAllHandsRef.current = revealAllHands
  }, [watchingSeat, revealAllHands])

  function flushIndex(index: number) {
    const entry = timeline[index]
    if (!entry) {
      return
    }
    currentIndexRef.current = index
    setCurrentIndex(index)
    sceneRef.current?.flushFromSnapshot(
      materializeReplaySnapshot(entry, watchingSeatRef.current, revealAllHandsRef.current),
    )
    sceneRef.current?.applyReplayCue(entry.category, entry.event as Record<string, any> | null)
  }

  function applyIndex(index: number) {
    const boundedIndex = Math.max(0, Math.min(index, timeline.length - 1))
    const entry = timeline[boundedIndex]
    if (!entry) {
      return
    }

    currentIndexRef.current = boundedIndex
    setCurrentIndex(boundedIndex)

    if (!sceneRef.current) {
      return
    }

    sceneRef.current.flushFromSnapshot(
      materializeReplaySnapshot(entry, watchingSeatRef.current, revealAllHandsRef.current),
    )
    sceneRef.current.applyReplayCue(entry.category, entry.event as Record<string, any> | null)
  }

  function animateForward(index: number) {
    const boundedIndex = Math.max(0, Math.min(index, timeline.length - 1))
    const entry = timeline[boundedIndex]
    if (!entry) {
      return
    }

    currentIndexRef.current = boundedIndex
    setCurrentIndex(boundedIndex)

    const payload = materializeReplayPayload(entry, watchingSeatRef.current, revealAllHandsRef.current)
    if (!payload || !sceneRef.current) {
      flushIndex(boundedIndex)
      return
    }
    sceneRef.current.handleEvent(payload as unknown as Record<string, unknown>)
  }

  useEffect(() => {
    if (!stageRef.current) {
      return
    }
    let cancelled = false

    const scene = new MahjongScene(() => {})
    scene.setPresentationMode('replay')
    sceneRef.current = scene
    setSceneReady(false)

    scene.mount(stageRef.current).then((mounted) => {
      if (cancelled || !mounted) {
        return
      }
      setSceneReady(true)
      scene.setVolume(loadStoredVolume())
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
    })

    return () => {
      cancelled = true
      stopPlayback()
      setSceneReady(false)
      scene.destroy()
      if (sceneRef.current === scene) {
        sceneRef.current = null
      }
    }
  }, [])

  useEffect(() => {
    sceneRef.current?.setAppearance(sceneAppearance)
  }, [sceneAppearance])

  useEffect(() => {
    sceneRef.current?.setBackgroundImage(backgroundImage?.dataUrl ?? null)
  }, [backgroundImage?.dataUrl])

  useEffect(() => {
    if (!sessionIdentifier) {
      setLoading(false)
      setError('缺少 session 参数，无法加载回放。')
      setSessionPayload(null)
      setTimeline([])
      stopPlayback()
      return
    }

    let cancelled = false
    setLoading(true)
    setError('')
    setSessionPayload(null)
    setTimeline([])
    stopPlayback()

    let receivedPayload = false
    const socket = new WebSocket(buildWebSocketUrl('/ws/replay', null, { session: sessionIdentifier }))

    socket.onmessage = (evt) => {
      if (cancelled) {
        return
      }
      let envelope: WsEnvelope<unknown>
      try {
        envelope = JSON.parse(evt.data) as WsEnvelope<unknown>
      } catch {
        setError('收到无法解析的回放消息。')
        setLoading(false)
        socket.close()
        return
      }

      if (envelope.type === 'replay.session') {
        receivedPayload = true
        const payload = envelope.payload as ReplaySessionPayload
        setSessionPayload(payload)
        setLoading(false)
        return
      }

      if (envelope.type === 'error') {
        const payload = envelope.payload as { message?: string }
        setError(payload.message ?? '加载回放失败。')
        setLoading(false)
      }
    }

    socket.onerror = () => {
      if (cancelled) {
        return
      }
      setError('回放连接失败。')
      setLoading(false)
    }

    socket.onclose = () => {
      if (cancelled || receivedPayload) {
        return
      }
      setLoading(false)
    }

    return () => {
      cancelled = true
      socket.close()
    }
  }, [sessionIdentifier])

  useEffect(() => {
    if (!sessionPayload) {
      return
    }

    stopPlayback()
    if (!activeRoundRecord) {
      setTimeline([])
      setCurrentIndex(0)
      currentIndexRef.current = 0
      setError('所选轮次不存在。')
      return
    }

    setError('')
    const nextTimeline = buildReplayTimeline(activeRoundRecord)
    const defaultIndex = findDefaultReplayEntryIndex(nextTimeline)
    setTimeline(nextTimeline)
    setCurrentIndex(defaultIndex)
    currentIndexRef.current = defaultIndex
  }, [activeRoundRecord, sessionPayload])

  useEffect(() => {
    if (!sceneReady || timeline.length === 0) {
      return
    }
    const boundedIndex = Math.max(0, Math.min(currentIndexRef.current, timeline.length - 1))
    const entry = timeline[boundedIndex]
    if (!entry) {
      return
    }
    currentIndexRef.current = boundedIndex
    setCurrentIndex(boundedIndex)
    sceneRef.current?.flushFromSnapshot(
      materializeReplaySnapshot(entry, watchingSeat, revealAllHands),
    )
  }, [sceneReady, timeline, watchingSeat, revealAllHands])

  useEffect(() => {
    if (!playing || timeline.length === 0) {
      return
    }
    if (currentIndexRef.current >= timeline.length - 1) {
      setPlaying(false)
      return
    }

    const current = timeline[currentIndexRef.current]
    const next = timeline[currentIndexRef.current + 1]
    const delayMs = clampReplayDelay(next.timestampMs - current.timestampMs)
    playbackTimeoutRef.current = setTimeout(() => {
      playbackTimeoutRef.current = null
      animateForward(currentIndexRef.current + 1)
    }, delayMs)

    return () => {
      if (playbackTimeoutRef.current !== null) {
        clearTimeout(playbackTimeoutRef.current)
        playbackTimeoutRef.current = null
      }
    }
  }, [playing, currentIndex, timeline, watchingSeat])

  useEffect(() => {
    sceneRef.current?.setVolume(loadStoredVolume())
  }, [])

  // ── Keybinds ────────────────────────────────────────────────────
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement) return
      switch (e.key) {
        case 'ArrowLeft':
          if (!canMoveBackward) break
          stopPlayback(); applyIndex(currentIndexRef.current - 1); break
        case 'ArrowRight':
          if (!canMoveForward) break
          stopPlayback(); applyIndex(currentIndexRef.current + 1); break
        case 'Home': case '[':
          if (timeline.length === 0) break
          stopPlayback(); applyIndex(firstPlayableIndex); break
        case 'End': case ']':
          if (timeline.length === 0) break
          stopPlayback(); applyIndex(finalTransitionIndex); break
        case 'Enter': stopPlayback(); setHideOtherHands(v => !v); break
        case ' ':
          if (!canMoveForward && !playing) break
          e.preventDefault(); setPlaying(v => !v); break
        case '-': stopPlayback(); setWatchingSeat(s => (s + 3) % 4); break
        case '+': case '=': stopPlayback(); setWatchingSeat(s => (s + 1) % 4); break
        case 'p': case 'ArrowUp':
          if (selectedRoundNumber <= 1) break
          stopPlayback(); setRequestedRoundNumber(selectedRoundNumber - 1); break
         case '\\': case 'ArrowDown':
          if (selectedRoundNumber >= roundCount) break
          stopPlayback(); setRequestedRoundNumber(selectedRoundNumber + 1); break
      }
    }
    document.addEventListener('keydown', handler)
    return () => document.removeEventListener('keydown', handler)
  }, [canMoveBackward, canMoveForward, selectedRoundNumber, roundCount, firstPlayableIndex, finalTransitionIndex, timeline, hideOtherHands, watchingSeat, playing])

  // ── Wheel → advance/retreat 1 event in replay ────────────────────
  useEffect(() => {
    const handler = (e: WheelEvent) => {
      if (!sceneReady || timeline.length === 0) return
      const steppingForward = e.deltaY > 0
      const steppingBackward = e.deltaY < 0
      if (!steppingForward && !steppingBackward) {
        return
      }
      e.preventDefault()
      if (steppingForward && currentIndexRef.current >= timeline.length - 1) {
        return
      }
      if (steppingBackward && currentIndexRef.current <= 0) {
        return
      }
      stopPlayback()
      if (steppingForward) applyIndex(currentIndexRef.current + 1)
      else applyIndex(currentIndexRef.current - 1)
    }
    document.addEventListener('wheel', handler, { passive: false })
    return () => document.removeEventListener('wheel', handler)
  }, [sceneReady, timeline])

  // ── Poll for new rounds if watching latest round live ─────────────
  const POLL_WINDOW_MS = 20 * 60 * 1000
  const lastTransition = activeRoundRecord?.transition_queue?.[activeRoundRecord.transition_queue.length - 1]
  const hasEnd = lastTransition?.kind === 'end'
  const isLatestRound = selectedRoundNumber === roundCount
  const lastTs = lastTransition?.timestamp_ms ?? 0
  const withinWindow = lastTs > 0 && (Date.now() - lastTs < POLL_WINDOW_MS)
  const shouldPoll = isLatestRound && !hasEnd && withinWindow

  useEffect(() => {
    if (!shouldPoll || !sessionIdentifier) {
      if (pollTimeoutRef.current) { clearTimeout(pollTimeoutRef.current); pollTimeoutRef.current = null }
      return
    }
    const poll = () => {
      pollTimeoutRef.current = null
      apiRequest<ReplaySessionPayload>(`/replay/${encodeURIComponent(sessionIdentifier)}/rounds-after?round=${selectedRoundNumber}`)
        .then(payload => {
          if (payload.round_records && payload.round_records.length > 0) {
            setSessionPayload(prev => prev ? { ...prev, round_records: [...(prev.round_records ?? []), ...payload.round_records!], round_count: payload.round_count ?? (prev.round_count ?? 0) } : payload)
          }
        })
        .catch(() => {})
        .finally(() => {
          pollTimeoutRef.current = setTimeout(poll, 10000)
        })
    }
    pollTimeoutRef.current = setTimeout(poll, 10000)
    return () => { if (pollTimeoutRef.current) { clearTimeout(pollTimeoutRef.current); pollTimeoutRef.current = null } }
  }, [shouldPoll, sessionIdentifier, selectedRoundNumber])

  // ── URL sync for round & perspective ──────────────────────────────
  useEffect(() => {
    const params = new URLSearchParams(location.search)
    params.set('round', String(selectedRoundNumber))
    params.set('perspective', String(watchingSeat))
    const newSearch = params.toString()
    if (location.search !== `?${newSearch}`) {
      navigate({ search: newSearch }, { replace: true })
    }
  }, [selectedRoundNumber, watchingSeat, location.search, navigate])

  // ── Same-player perspective persistence across rounds ─────────────
  const watchingPlayerIdRef = useRef<number | null>(null)
  useEffect(() => {
    if (!activeRoundRecord) return
    const initSeats = activeRoundRecord.initial_seats ?? []
    const prevPid = watchingPlayerIdRef.current

    if (prevPid != null) {
      const newSeat = initSeats.findIndex(s => s.player_id === prevPid)
      if (newSeat >= 0) {
        watchingPlayerIdRef.current = initSeats[newSeat]?.player_id ?? null
        if (newSeat !== watchingSeat) {
          setWatchingSeat(newSeat)
        }
        return
      }
    }

    // Player not found in new round — fall back to same seat index
    watchingPlayerIdRef.current = initSeats[watchingSeat]?.player_id ?? null
  }, [activeRoundRecord])

  if (!sessionIdentifier) {
    return (
      <div className="game-blocked">
        <div className="game-blocked-card">
          <h1>缺少回放参数</h1>
          <div className="game-blocked-actions">
            <button onClick={() => navigate('/')}>返回大厅</button>
          </div>
        </div>
      </div>
    )
  }

  return (
    <div className="replay-page" style={{ background: sceneAppearance.backgroundColorOutside }}>
      <div className="replay-layout" style={{ background: sceneAppearance.backgroundColorOutside }}>
        <section className="replay-board-panel">
          <div className="replay-stage-shell" style={{ background: sceneAppearance.backgroundColorTable }}>
            <div ref={stageRef} className="replay-stage" />
            {loading ? <div className="replay-stage-overlay">回放加载中…</div> : null}
            {!loading && error ? <div className="replay-stage-overlay replay-stage-overlay-error">{error}</div> : null}
          </div>
        </section>

        <aside className="replay-sidebar">
          <div className="replay-sidebar-card">
            <h2 className="replay-title">回放</h2>
            <p className="replay-subtitle">{sessionIdentifier}</p>

            <div className="replay-chip-row">
              <span className="replay-chip">第 {selectedRoundNumber} / {roundCount} 局</span>
              <span className="replay-chip">巡目 {currentRoundTurn ?? '-'} / {totalRoundTurn ?? '-'}</span>
            </div>

            <div className="replay-section">
              {/* <h3>记录信息</h3>
              <p>玩家：{(sessionPayload?.player_names ?? []).filter((name) => name.trim().length > 0).join(' / ') || '无'}</p> */}
              <button onClick={() => { void navigator.clipboard?.writeText(wallSeedCopyText) }} disabled={wallSeedCopyText.length === 0 || recordVersion < 3}>复制牌山种子</button>
            </div>

            <div className="replay-control-grid">
              <button onClick={() => { stopPlayback(); applyIndex(firstPlayableIndex) }} disabled={timeline.length === 0}>&lt;&lt;</button>
              <button onClick={() => { stopPlayback(); applyIndex(finalTransitionIndex) }} disabled={timeline.length === 0}>&gt;&gt;</button>
              <button onClick={() => { stopPlayback(); applyIndex(currentIndexRef.current - 1) }} disabled={!canMoveBackward}>&lt;</button>
              <button onClick={() => { stopPlayback(); applyIndex(currentIndexRef.current + 1) }} disabled={!canMoveForward}>&gt;</button>
              <button onClick={() => setPlaying((value) => !value)} disabled={!canMoveForward && !playing}>{playing ? '暂停' : '自动播放'}</button>
              <button onClick={() => { stopPlayback(); setHideOtherHands((value) => !value) }}>{hideOtherHands ? '显示他家手牌' : '隐藏他家手牌'}</button>
              <button onClick={() => { stopPlayback(); if (selectedRoundNumber > 1) setRequestedRoundNumber(selectedRoundNumber - 1) }} disabled={selectedRoundNumber <= 1}>上一局</button>
              <button onClick={() => { stopPlayback(); if (selectedRoundNumber < roundCount) setRequestedRoundNumber(selectedRoundNumber + 1) }} disabled={selectedRoundNumber >= roundCount}>下一局</button>
              <button onClick={() => { stopPlayback(); setWatchingSeat((watchingSeat + 3) % 4) }}>上家</button>
              <button onClick={() => { stopPlayback(); setWatchingSeat((watchingSeat + 1) % 4) }}>下家</button>
            </div>

            {replayRatings && replayRatings.length > 0 && (
              <div className="replay-section" style={{ marginTop: 16 }}>
                <h3 style={{ fontSize: '13px', marginBottom: 8 }}>初始评级</h3>
                {replayRatings.map((r) => (
                  <div key={r.player_id} style={{ marginBottom: 6, fontSize: '12px' }}>
                    <div style={{ fontWeight: 500 }}>{r.username || `#${r.player_id}`}</div>
                    {r.level !== undefined && (
                      <div style={{ color: '#666' }}>{rankToChinese(r.level)} · {r.points?.toFixed(2)}pts</div>
                    )}
                    {r.mu !== undefined && (
                      <div style={{ color: '#999', fontSize: '10px' }}>R {r.mu.toFixed(2)}±{r.tau?.toFixed(2)} · σ {r.sigma?.toFixed(2)}</div>
                    )}
                  </div>
                ))}
              </div>
            )}
            {finalReplayRatings && finalReplayRatings.length > 0 && (
              <div className="replay-section" style={{ marginTop: 8 }}>
                <h3 style={{ fontSize: '13px', marginBottom: 8 }}>最终评级</h3>
                {replayRatings && replayRatings.length > 0
                  ? replayRatings.map((init) => {
                      const r = finalReplayRatings.find(fr => fr.player_id === init.player_id)
                      const name = init.username || r?.username || `#${init.player_id}`
                      const data = r ?? init
                      return (
                        <div key={data.player_id} style={{ marginBottom: 6, fontSize: '12px' }}>
                          <div style={{ fontWeight: 500 }}>{name}</div>
                          {data.level !== undefined && (
                            <div style={{ color: '#666' }}>{rankToChinese(data.level)} · {data.points?.toFixed(2)}pts</div>
                          )}
                          {data.mu !== undefined && (
                            <div style={{ color: '#999', fontSize: '10px' }}>R {data.mu.toFixed(2)}±{data.tau?.toFixed(2)} · σ {data.sigma?.toFixed(2)}</div>
                          )}
                        </div>
                      )
                    })
                  : finalReplayRatings.map((r) => (
                      <div key={r.player_id} style={{ marginBottom: 6, fontSize: '12px' }}>
                        <div style={{ fontWeight: 500 }}>{r.username || `#${r.player_id}`}</div>
                        {r.level !== undefined && (
                          <div style={{ color: '#666' }}>{rankToChinese(r.level)} · {r.points?.toFixed(2)}pts</div>
                        )}
                        {r.mu !== undefined && (
                          <div style={{ color: '#999', fontSize: '10px' }}>R {r.mu.toFixed(2)}±{r.tau?.toFixed(2)} · σ {r.sigma?.toFixed(2)}</div>
                        )}
                      </div>
                    ))}
              </div>
            )}


          {/* <div className={`scene-appearance-toggle${appearancePanelOpen ? ' is-open' : ''}`}>
              {appearancePanelOpen && (
                <div className="scene-appearance-toggle__panel">
                  <div className="scene-appearance-toggle__card">
                    <SceneAppearancePanel
                      title="场景外观"
                      appearance={sceneAppearance}
                      backgroundImageName={backgroundImage?.name ?? null}
                      backgroundImageLoading={backgroundImageLoading}
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
              <button
                type="button"
                className="scene-appearance-toggle__button"
                aria-expanded={appearancePanelOpen}
                onClick={() => setAppearancePanelOpen((value) => !value)}
              >
                外观设置
              </button>
            </div> */}
          </div>
        </aside>
      </div>
    </div>
  )
}