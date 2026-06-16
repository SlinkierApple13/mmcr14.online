import type {
  ActiveSessionSnapshot,
  CompactSeatStatus,
  GameEventPayload,
  GameEventSnapshot,
  GameState,
  MeldSnapshot,
  ReplayRecordEvent,
  ReplayRoundRecord,
  SeatSnapshot,
  ViewerSnapshot,
} from './types'

const TOTAL_WALL_TILE_COUNT = 136

const WATCHING_SEATS = ['east', 'south', 'west', 'north'] as const

type ReplayCategory = 'claim' | 'transition'

type ReplaySeatState = {
  seat_index: number
  score: number
  afk: boolean
  disconnected: boolean
  player_id: number | null
  username: string | null
  hand_tiles: number[]
  drawn_tile: number | null
  discard_pile: number[]
  discard_drawn_flags: boolean[]
  melds: MeldSnapshot[]
}

type ReplayBaseSnapshot = {
  session_id: number
  seats: ReplaySeatState[]
  state: GameState
  result_event: GameEventSnapshot | null
  last_discarder_seat: number | null
}

export interface ReplayTimelineEntry {
  index: number
  category: 'initial' | ReplayCategory
  event: ReplayRecordEvent | null
  timestampMs: number
  label: string
  snapshotBase: ReplayBaseSnapshot
}

type ReplayQueuedEvent = {
  category: ReplayCategory
  event: ReplayRecordEvent
  order: number
}

function watchingSeatName(seatIndex: number): (typeof WATCHING_SEATS)[number] {
  return WATCHING_SEATS[((seatIndex % 4) + 4) % 4] ?? 'east'
}

export function parseWatchingSeat(rawValue: string | null | undefined): number {
  const normalized = rawValue?.trim().toLowerCase()
  const index = WATCHING_SEATS.indexOf(
    normalized as (typeof WATCHING_SEATS)[number],
  )
  return index >= 0 ? index : 0
}

export function encodeWatchingSeat(seatIndex: number): string {
  return watchingSeatName(seatIndex)
}

function parseSessionId(sessionIdentifier: string): number {
  const rawPrefix = sessionIdentifier.split('_', 1)[0] ?? ''
  const parsed = Number(rawPrefix)
  return Number.isFinite(parsed) && parsed > 0 ? parsed : 0
}

function cloneMeld(meld: MeldSnapshot): MeldSnapshot {
  return {
    tile: meld.tile,
    type: meld.type,
    concealed: meld.concealed,
    chow_mode: meld.chow_mode,
    meld_from_rel: meld.meld_from_rel,
    claimed_from_drawn_discard: meld.claimed_from_drawn_discard,
    added_from_drawn_tile: meld.added_from_drawn_tile,
    concealed_from_drawn_tile: meld.concealed_from_drawn_tile,
  }
}

function cloneSeatState(seat: ReplaySeatState): ReplaySeatState {
  return {
    seat_index: seat.seat_index,
    score: seat.score,
    afk: seat.afk,
    disconnected: seat.disconnected,
    player_id: seat.player_id,
    username: seat.username,
    hand_tiles: [...seat.hand_tiles],
    drawn_tile: seat.drawn_tile,
    discard_pile: [...seat.discard_pile],
    discard_drawn_flags: [...seat.discard_drawn_flags],
    melds: seat.melds.map(cloneMeld),
  }
}

function cloneState(state: GameState): GameState {
  return {
    round_counter: state.round_counter,
    stage_counter: state.stage_counter,
    remaining_tile_count: state.remaining_tile_count,
    current_player: state.current_player ?? null,
    last_actor: state.last_actor ?? null,
    last_event_kind: state.last_event_kind ?? null,
    result_source_actor: state.result_source_actor ?? null,
    ended: state.ended ?? false,
    final_scores: Array.isArray(state.final_scores) ? [...state.final_scores] : null,
  }
}

function cloneResultEvent(event: GameEventSnapshot | null): GameEventSnapshot | null {
  if (!event) {
    return null
  }
  return {
    ...event,
    drawn_tiles: Array.isArray(event.drawn_tiles) ? [...event.drawn_tiles] : undefined,
    revealed_hand_tiles: Array.isArray(event.revealed_hand_tiles)
      ? [...event.revealed_hand_tiles]
      : undefined,
    scores: Array.isArray(event.scores) ? [...event.scores] : undefined,
    win: event.win
      ? {
          win_fan: event.win.win_fan,
          win_base_point: event.win.win_base_point,
          win_fan_codes: [...event.win.win_fan_codes],
          win_fans: event.win.win_fans ? [...event.win.win_fans] : undefined,
        }
      : undefined,
  }
}

function cloneBaseSnapshot(snapshot: ReplayBaseSnapshot): ReplayBaseSnapshot {
  return {
    session_id: snapshot.session_id,
    seats: snapshot.seats.map(cloneSeatState),
    state: cloneState(snapshot.state),
    result_event: cloneResultEvent(snapshot.result_event),
    last_discarder_seat: snapshot.last_discarder_seat,
  }
}

function toGameEventSnapshot(event: ReplayRecordEvent): GameEventSnapshot {
  return {
    kind: event.kind,
    stage_counter: event.stage_counter,
    actor_seat: event.actor_seat,
    timestamp_ms: event.timestamp_ms,
    drawn_tiles: event.drawn_tiles ? [...event.drawn_tiles] : undefined,
    tile: event.tile,
    use_drawn_tile: event.use_drawn_tile,
    forced: event.forced,
    draw_from_back: event.draw_from_back,
    ui64_value: event.ui64_value,
    revealed_hand_tiles: event.revealed_hand_tiles ? [...event.revealed_hand_tiles] : undefined,
    scores: event.final_scores ? [...event.final_scores] : undefined,
    win: event.win_data
      ? {
          win_fan: event.win_data.win_fan,
          win_base_point: event.win_data.win_base_point,
          win_fan_codes: [...event.win_data.win_fan_codes],
          win_fans: event.win_data.win_fans ? [...event.win_data.win_fans] : undefined,
        }
      : undefined,
  }
}

function buildViewerSnapshot(watchingSeat: number): ViewerSnapshot {
  return {
    seat_index: watchingSeat,
    pending: 'none',
    decision_timer_ms: null,
    available_actions: [],
  }
}

function buildSeatSnapshot(seat: ReplaySeatState): SeatSnapshot {
  return {
    seat_index: seat.seat_index,
    score: seat.score,
    afk: seat.afk,
    disconnected: seat.disconnected,
    hand_tile_count: seat.hand_tiles.length,
    has_drawn_tile: seat.drawn_tile != null,
    player_id: seat.player_id,
    username: seat.username,
    discard_pile: [...seat.discard_pile],
    discard_drawn_flags: [...seat.discard_drawn_flags],
    melds: seat.melds.map(cloneMeld),
    hand_tiles: [...seat.hand_tiles],
    drawn_tile: seat.drawn_tile,
  }
}

function buildSeatStatus(seat: ReplaySeatState): CompactSeatStatus {
  return {
    seat_index: seat.seat_index,
    score: seat.score,
    afk: seat.afk,
    disconnected: seat.disconnected,
    hand_tile_count: seat.hand_tiles.length,
    has_drawn_tile: seat.drawn_tile != null,
    username: seat.username,
  }
}

function getChowTilesFromCentral(centralTid: number, chowMode: number): [number, number, number] | null {
  switch (chowMode) {
    case 1:
      return [centralTid - 1, centralTid, centralTid + 1]
    case 2:
      return [centralTid, centralTid - 1, centralTid + 1]
    case 3:
      return [centralTid + 1, centralTid - 1, centralTid]
    default:
      return null
  }
}

function meldFromRel(actorSeat: number, sourceSeat: number): number {
  const relative = (actorSeat - sourceSeat + 4) % 4
  return relative === 0 ? 1 : relative
}

function removeFirstTile(handTiles: number[], tile: number): boolean {
  const index = handTiles.indexOf(tile)
  if (index < 0) {
    return false
  }
  handTiles.splice(index, 1)
  return true
}

function removeMultipleTiles(handTiles: number[], tile: number, count: number): void {
  for (let index = 0; index < count; index += 1) {
    removeFirstTile(handTiles, tile)
  }
}

function popDiscard(seat: ReplaySeatState): void {
  seat.discard_pile.pop()
  seat.discard_drawn_flags.pop()
}

function discardTile(seat: ReplaySeatState, tile: number, useDrawnTile: boolean): void {
  if (useDrawnTile) {
    seat.drawn_tile = null
  } else {
    removeFirstTile(seat.hand_tiles, tile)
    if (seat.drawn_tile != null) {
      seat.hand_tiles.push(seat.drawn_tile)
      seat.drawn_tile = null
    }
  }
  seat.discard_pile.push(tile)
  seat.discard_drawn_flags.push(useDrawnTile)
}

function moveDrawnTileToHand(seat: ReplaySeatState): void {
  if (seat.drawn_tile == null) {
    return
  }
  seat.hand_tiles.push(seat.drawn_tile)
  seat.drawn_tile = null
}

function applyPlayerAvailability(base: ReplayBaseSnapshot, event: ReplayRecordEvent): void {
  const seat = base.seats[event.actor_seat]
  if (!seat) {
    return
  }
  if (event.kind === 'player_left') {
    seat.disconnected = true
  }
  if (event.kind === 'player_resumed') {
    seat.afk = false
    seat.disconnected = false
  }
}

function mergeRecordEvents(roundRecord: ReplayRoundRecord): ReplayQueuedEvent[] {
  const merged: ReplayQueuedEvent[] = [
    ...roundRecord.event_queue
      .filter((event) => event.kind !== 'discard_tile' && event.kind !== 'pass' && event.kind !== 'final_pass')
      .map((event, order) => ({ category: 'claim' as const, event, order })),
    ...roundRecord.transition_queue.map((event, order) => ({ category: 'transition' as const, event, order })),
  ]

  merged.sort((left, right) => {
    const leftIsTerminalZeroTimestamp = left.category === 'transition'
      && left.event.timestamp_ms === 0
      && (left.event.kind === 'drawn_game' || left.event.kind === 'end')
    const rightIsTerminalZeroTimestamp = right.category === 'transition'
      && right.event.timestamp_ms === 0
      && (right.event.kind === 'drawn_game' || right.event.kind === 'end')
    if (leftIsTerminalZeroTimestamp !== rightIsTerminalZeroTimestamp) {
      return leftIsTerminalZeroTimestamp ? 1 : -1
    }
    if (left.event.timestamp_ms !== right.event.timestamp_ms) {
      return left.event.timestamp_ms - right.event.timestamp_ms
    }
    if (left.event.stage_counter !== right.event.stage_counter) {
      return left.event.stage_counter - right.event.stage_counter
    }
    if (left.category !== right.category) {
      return left.category === 'claim' ? -1 : 1
    }
    if (left.event.kind !== right.event.kind && left.event.kind === 'player_resumed') {
      return -1
    }
    if (left.event.kind !== right.event.kind && right.event.kind === 'player_resumed') {
      return 1
    }
    return left.order - right.order
  })

  return merged
}

function formatEntryLabel(category: ReplayCategory, event: ReplayRecordEvent): string {
  const kindLabels: Record<string, string> = {
    start: '开始',
    predraw: '配牌',
    draw_tile: '摸牌',
    discard_tile: '打牌',
    chow: '吃',
    pung: '碰',
    melded_kong: '明杠',
    added_kong: '加杠',
    concealed_kong: '暗杠',
    discard_win: '荣和',
    rob_added_kong_win: '抢杠和',
    self_drawn_win: '自摸',
    drawn_game: '流局',
    end: '结算',
    pass: '过',
    final_pass: '终过',
    player_left: '离线',
    player_resumed: '恢复',
  }
  const prefix = category === 'claim' ? '宣言' : '转移'
  return `${prefix} · ${kindLabels[event.kind] ?? event.kind}`
}

function createInitialBaseSnapshot(roundRecord: ReplayRoundRecord): ReplayBaseSnapshot {
  return {
    session_id: parseSessionId(roundRecord.header.session_identifier),
    seats: roundRecord.initial_seats.map((seat, seatIndex) => ({
      seat_index: seatIndex,
      score: seat.score,
      afk: seat.afk_counter > 0,
      disconnected: Boolean(seat.disconnected),
      player_id: seat.player_id,
      username: seat.player_name,
      hand_tiles: [],
      drawn_tile: null,
      discard_pile: [],
      discard_drawn_flags: [],
      melds: [],
    })),
    state: {
      round_counter: roundRecord.header.round_number,
      stage_counter: 0,
      remaining_tile_count: TOTAL_WALL_TILE_COUNT,
      current_player: null,
      last_actor: null,
      last_event_kind: null,
      result_source_actor: null,
      ended: false,
      final_scores: null,
    },
    result_event: null,
    last_discarder_seat: null,
  }
}

function applyTransition(
  base: ReplayBaseSnapshot,
  event: ReplayRecordEvent,
  roundNumber: number,
): void {
  const previousLastActor = base.state.last_actor ?? null
  const actorSeat = event.actor_seat
  const actor = base.seats[actorSeat]
  if (!actor) {
    return
  }

  base.state.stage_counter = event.stage_counter
  base.state.last_event_kind = event.kind
  base.state.last_actor = actorSeat
  applyPlayerAvailability(base, event)

  switch (event.kind) {
    case 'start': {
      base.result_event = null
      base.last_discarder_seat = null
      base.state.round_counter = roundNumber
      base.state.remaining_tile_count = TOTAL_WALL_TILE_COUNT
      base.state.current_player = actorSeat
      base.state.result_source_actor = null
      base.state.ended = false
      base.state.final_scores = null
      for (const seat of base.seats) {
        seat.hand_tiles = []
        seat.drawn_tile = null
        seat.discard_pile = []
        seat.discard_drawn_flags = []
        seat.melds = []
      }
      return
    }
    case 'predraw': {
      const drawnTiles = event.drawn_tiles ?? []
      actor.hand_tiles.push(...drawnTiles)
      base.state.remaining_tile_count = Math.max(
        0,
        base.state.remaining_tile_count - drawnTiles.length,
      )
      base.state.current_player = actorSeat
      return
    }
    case 'draw_tile': {
      if (typeof event.tile === 'number') {
        actor.drawn_tile = event.tile
      }
      base.state.remaining_tile_count = Math.max(0, base.state.remaining_tile_count - 1)
      base.state.current_player = actorSeat
      return
    }
    case 'discard_tile': {
      if (typeof event.tile === 'number') {
        discardTile(actor, event.tile, Boolean(event.use_drawn_tile))
        base.last_discarder_seat = actorSeat
        base.state.result_source_actor = actorSeat
      }
      base.state.current_player = actorSeat
      return
    }
    case 'chow': {
      const sourceSeat = base.last_discarder_seat ?? previousLastActor ?? actorSeat
      const source = base.seats[sourceSeat]
      const claimedFromDrawnDiscard = Boolean(source?.discard_drawn_flags[source.discard_drawn_flags.length - 1])
      if (source) {
        popDiscard(source)
      }
      if (typeof event.tile === 'number') {
        const chowMode = event.ui64_value ?? 0
        const chowTiles = getChowTilesFromCentral(event.tile, chowMode)
        if (chowTiles) {
          removeFirstTile(actor.hand_tiles, chowTiles[1])
          removeFirstTile(actor.hand_tiles, chowTiles[2])
          actor.melds.push({
            tile: event.tile,
            type: 'sequence',
            chow_mode: chowMode,
            meld_from_rel: meldFromRel(actorSeat, sourceSeat),
            claimed_from_drawn_discard: claimedFromDrawnDiscard,
          })
        }
      }
      actor.drawn_tile = null
      base.state.current_player = actorSeat
      return
    }
    case 'pung': {
      const sourceSeat = base.last_discarder_seat ?? previousLastActor ?? actorSeat
      const source = base.seats[sourceSeat]
      const claimedFromDrawnDiscard = Boolean(source?.discard_drawn_flags[source.discard_drawn_flags.length - 1])
      if (source) {
        popDiscard(source)
      }
      if (typeof event.tile === 'number') {
        removeMultipleTiles(actor.hand_tiles, event.tile, 2)
        actor.melds.push({
          tile: event.tile,
          type: 'triplet',
          chow_mode: 0,
          meld_from_rel: meldFromRel(actorSeat, sourceSeat),
          claimed_from_drawn_discard: claimedFromDrawnDiscard,
        })
      }
      actor.drawn_tile = null
      base.state.current_player = actorSeat
      return
    }
    case 'melded_kong': {
      const sourceSeat = base.last_discarder_seat ?? previousLastActor ?? actorSeat
      const source = base.seats[sourceSeat]
      const claimedFromDrawnDiscard = Boolean(source?.discard_drawn_flags[source.discard_drawn_flags.length - 1])
      if (source) {
        popDiscard(source)
      }
      if (typeof event.tile === 'number') {
        removeMultipleTiles(actor.hand_tiles, event.tile, 3)
        actor.melds.push({
          tile: event.tile,
          type: 'kong',
          concealed: false,
          chow_mode: 0,
          meld_from_rel: meldFromRel(actorSeat, sourceSeat),
          claimed_from_drawn_discard: claimedFromDrawnDiscard,
        })
      }
      actor.drawn_tile = null
      base.state.current_player = actorSeat
      return
    }
    case 'added_kong': {
      if (typeof event.tile === 'number') {
        if (event.use_drawn_tile) {
          actor.drawn_tile = null
        } else {
          removeFirstTile(actor.hand_tiles, event.tile)
          moveDrawnTileToHand(actor)
        }
        const meld = actor.melds.find(
          (item) => item.tile === event.tile && item.type === 'triplet',
        )
        if (meld) {
          meld.type = 'kong'
          if (typeof meld.meld_from_rel === 'number' && meld.meld_from_rel >= 1 && meld.meld_from_rel <= 3) {
            meld.meld_from_rel += 4
          }
          if (event.use_drawn_tile) {
            meld.added_from_drawn_tile = true
          }
        }
      }
      base.state.current_player = actorSeat
      return
    }
    case 'concealed_kong': {
      if (typeof event.tile === 'number') {
        if (event.use_drawn_tile) {
          actor.drawn_tile = null
          removeMultipleTiles(actor.hand_tiles, event.tile, 3)
        } else {
          removeMultipleTiles(actor.hand_tiles, event.tile, 4)
          moveDrawnTileToHand(actor)
        }
        actor.melds.push({
          tile: event.tile,
          type: 'kong',
          concealed: true,
          chow_mode: 0,
          meld_from_rel: 0,
          concealed_from_drawn_tile: Boolean(event.use_drawn_tile),
        })
      }
      base.state.current_player = actorSeat
      return
    }
    case 'discard_win': {
      base.state.result_source_actor = base.last_discarder_seat ?? previousLastActor
      if (Array.isArray(event.revealed_hand_tiles)) {
        actor.hand_tiles = [...event.revealed_hand_tiles]
        actor.drawn_tile = null
      }
      base.result_event = toGameEventSnapshot(event)
      base.state.current_player = actorSeat
      return
    }
    case 'rob_added_kong_win': {
      base.state.result_source_actor = previousLastActor
      if (Array.isArray(event.revealed_hand_tiles)) {
        actor.hand_tiles = [...event.revealed_hand_tiles]
        actor.drawn_tile = null
      }
      base.result_event = toGameEventSnapshot(event)
      base.state.current_player = actorSeat
      return
    }
    case 'self_drawn_win': {
      if (Array.isArray(event.revealed_hand_tiles)) {
        actor.hand_tiles = [...event.revealed_hand_tiles]
      }
      if (typeof event.tile === 'number') {
        actor.drawn_tile = event.tile
      }
      base.result_event = toGameEventSnapshot(event)
      base.state.current_player = actorSeat
      return
    }
    case 'drawn_game': {
      base.result_event = toGameEventSnapshot(event)
      return
    }
    case 'end': {
      base.state.ended = true
      if (Array.isArray(event.final_scores)) {
        base.state.final_scores = [...event.final_scores]
        event.final_scores.forEach((score, seatIndex) => {
          const seat = base.seats[seatIndex]
          if (seat) {
            seat.score = score
          }
        })
      }
      return
    }
    case 'player_left':
    case 'player_resumed': {
      return
    }
    default:
      return
  }
}

function buildReplaySnapshot(
  snapshotBase: ReplayBaseSnapshot,
  watchingSeat: number,
  revealAllHands: boolean,
): ActiveSessionSnapshot {
  return {
    phase: 'active',
    session_id: snapshotBase.session_id,
    state: cloneState(snapshotBase.state),
    seats: snapshotBase.seats.map(buildSeatSnapshot),
    viewer: buildViewerSnapshot(watchingSeat),
    result_event: cloneResultEvent(snapshotBase.result_event),
    reveal_all_hands: revealAllHands,
  }
}

function buildReplayPayload(
  category: ReplayCategory,
  event: ReplayRecordEvent,
  snapshotBase: ReplayBaseSnapshot,
  watchingSeat: number,
  revealAllHands: boolean,
): GameEventPayload {
  return {
    category,
    event: toGameEventSnapshot(event),
    state: cloneState(snapshotBase.state),
    viewer: buildViewerSnapshot(watchingSeat),
    seat_status: snapshotBase.seats.map(buildSeatStatus),
    reveal_all_hands: revealAllHands,
  }
}

export function buildReplayTimeline(roundRecord: ReplayRoundRecord): ReplayTimelineEntry[] {
  const base = createInitialBaseSnapshot(roundRecord)
  const timeline: ReplayTimelineEntry[] = [
    {
      index: 0,
      category: 'initial',
      event: null,
      timestampMs: 0,
      label: '初始状态',
      snapshotBase: cloneBaseSnapshot(base),
    },
  ]

  const merged = mergeRecordEvents(roundRecord)
  merged.forEach((item) => {
    if (item.category === 'transition') {
      applyTransition(base, item.event, roundRecord.header.round_number)
    } else {
      applyPlayerAvailability(base, item.event)
    }

    timeline.push({
      index: timeline.length,
      category: item.category,
      event: item.event,
      timestampMs: item.event.timestamp_ms,
      label: formatEntryLabel(item.category, item.event),
      snapshotBase: cloneBaseSnapshot(base),
    })
  })

  return timeline
}

export function materializeReplaySnapshot(
  entry: ReplayTimelineEntry,
  watchingSeat: number,
  revealAllHands: boolean,
): ActiveSessionSnapshot {
  return buildReplaySnapshot(entry.snapshotBase, watchingSeat, revealAllHands)
}

export function materializeReplayPayload(
  entry: ReplayTimelineEntry,
  watchingSeat: number,
  revealAllHands: boolean,
): GameEventPayload | null {
  if (entry.category === 'initial' || !entry.event) {
    return null
  }
  return buildReplayPayload(entry.category, entry.event, entry.snapshotBase, watchingSeat, revealAllHands)
}

export function findFinalTransitionIndex(timeline: ReplayTimelineEntry[]): number {
  for (let index = timeline.length - 1; index >= 0; index -= 1) {
    const entry = timeline[index]
    if (entry.category !== 'transition' || !entry.event) {
      continue
    }
    if (entry.event.kind === 'end') {
      continue
    }
    return index
  }
  return 0
}

export function findSelfTransitionIndex(
  timeline: ReplayTimelineEntry[],
  currentIndex: number,
  watchingSeat: number,
  direction: 'backward' | 'forward',
): number | null {
  const step = direction === 'forward' ? 1 : -1
  for (
    let index = currentIndex + step;
    index >= 0 && index < timeline.length;
    index += step
  ) {
    const entry = timeline[index]
    if (entry.category !== 'transition' || !entry.event) {
      continue
    }
    if (entry.event.actor_seat === watchingSeat) {
      return index
    }
  }
  return null
}

export function clampReplayDelay(delayMs: number): number {
  if (!Number.isFinite(delayMs) || delayMs <= 0) {
    return 150
  }
  return Math.min(3000, Math.max(50, delayMs))
}
