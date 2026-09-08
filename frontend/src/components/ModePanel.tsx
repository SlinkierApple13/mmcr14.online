import { useEffect, useRef, useState } from 'react'
import type { FiveGatesModeState, FiveGatesModeUpdate } from '../lib/types'

const TEAM_NAMES = ['虎队', '龙队']
const TEAM_COLORS = ['#c0392b', '#2471a3'] as const

function isFiveGates(state: unknown): state is FiveGatesModeState {
  return (
    !!state &&
    typeof state === 'object' &&
    (state as { mode?: string }).mode === 'pass_five_gates'
  )
}

function completedCount(bitmask: number | undefined): number {
  if (typeof bitmask !== 'number') return 0
  let count = 0
  let bits = bitmask
  while (bits !== 0) {
    bits &= bits - 1
    count += 1
  }
  return count
}

/**
 * 过五关 group panel shown in the waiting room: 虎/龙 team cards with join
 * buttons, plus a ready gate (team required before readying up).
 */
export function TeamPanel({
  seats,
  myPlayerId,
  onSelectTeam,
}: {
  seats: Array<{ seat_index: number; ready: boolean; team: number | null; player_id: number | null; username: string | null }>
  myPlayerId: number | null
  onSelectTeam: (team: 0 | 1) => void
}) {
  const myTeam = (() => {
    const own = seats.find((seat) => seat.player_id === myPlayerId)
    const team = own?.team
    return team === 0 || team === 1 ? team : null
  })()

  const membersOf = (team: 0 | 1) =>
    seats.filter((seat) => seat.team === team && seat.player_id !== null)

  return (
    <div className="mode-team-panel">
      <div className="mode-team-panel__title">过五关 · 组队</div>
      <div className="mode-team-panel__teams">
        {([0, 1] as const).map((team) => {
          const members = membersOf(team)
          const full = members.length >= 2
          const mine = myTeam === team
          return (
            <div key={team} className={`mode-team-panel__card is-${team === 0 ? 'hu' : 'long'}${mine ? ' is-mine' : ''}`}>
              <div className="mode-team-panel__team-name" style={{ color: TEAM_COLORS[team] }}>
                {TEAM_NAMES[team]}
                <span className="mode-team-panel__count"> {members.length}/2</span>
              </div>
              <div className="mode-team-panel__members">
                {members.length === 0 && <div className="mode-team-panel__empty">暂无成员</div>}
                {members.map((member) => (
                  <div key={member.player_id} className="mode-team-panel__member">
                    {member.username ?? `#${member.player_id}`}
                    {member.player_id === myPlayerId && <span className="mode-team-panel__me">（我）</span>}
                    {member.ready && <span className="mode-team-panel__ready-dot" title="已准备" />}
                  </div>
                ))}
              </div>
              {mine ? (
                <div className="mode-team-panel__joined">已加入</div>
              ) : (
                <button
                  type="button"
                  className="mode-team-panel__join"
                  disabled={full}
                  onClick={() => onSelectTeam(team)}
                >
                  {full ? '已满' : `加入${TEAM_NAMES[team]}`}
                </button>
              )}
            </div>
          )
        })}
      </div>
      {myTeam === null && (
        <div className="mode-team-panel__hint">请先选择队伍，再点击「准备」</div>
      )}
    </div>
  )
}

/**
 * 过五关 HUD shown during the active game: team progress + 5 target cards.
 */
export function ModePanel({
  modeState,
  modeUpdate,
  ended,
  seats,
}: {
  modeState: unknown
  modeUpdate: FiveGatesModeUpdate | null
  ended: boolean
  seats?: Array<{ seat_index: number; player_id: number | null; username: string | null }>
}) {
  const state = isFiveGates(modeState) ? modeState : null
  const [completedNow, setCompletedNow] = useState<string[]>([])
  const [flash, setFlash] = useState(false)
  const flashTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)

  useEffect(() => {
    if (!modeUpdate || !state) return
    const completedNowList = Array.isArray(modeUpdate.completed_now) ? modeUpdate.completed_now : []
    if (completedNowList.length > 0) {
      setCompletedNow(completedNowList)
      setFlash(true)
      if (flashTimerRef.current !== null) clearTimeout(flashTimerRef.current)
      flashTimerRef.current = setTimeout(() => {
        setFlash(false)
        setCompletedNow([])
      }, 1200)
    }
    return () => {
      if (flashTimerRef.current !== null) clearTimeout(flashTimerRef.current)
    }
  }, [modeUpdate, state])

  if (!state) return null

  const winnerTeam = typeof state.winner_team === 'number' ? state.winner_team : null
  const teamCompleted = (team: 0 | 1) => completedCount(Number(state.completed?.[team] ?? 0))
  const teams = Array.isArray(state.teams) ? state.teams : []

  const seatName = (playerId: number): string => {
    const seat = (seats ?? []).find((s) => s.player_id === playerId)
    return seat?.username?.trim() ? seat.username : `#${playerId}`
  }

  return (
    <div className={`mode-panel${flash ? ' is-flash' : ''}`}>
      <div className="mode-panel__teams">
        {([0, 1] as const).map((team) => {
          const members = teams.filter((t) => t.team === team)
          const done = teamCompleted(team)
          return (
            <div key={team} className={`mode-panel__team is-${team === 0 ? 'hu' : 'long'}`}>
              <span className="mode-panel__team-name" style={{ color: TEAM_COLORS[team] }}>
                {TEAM_NAMES[team]}
              </span>
              <span className="mode-panel__team-progress">
                {done}/5
              </span>
              {ended && winnerTeam !== null && (
                <span className="mode-panel__winner-badge">
                  {winnerTeam === team ? '🏆 胜' : winnerTeam === -2 ? '平局' : ''}
                </span>
              )}
              <div className="mode-panel__team-members">
                {members.map((m) => seatName(m.player_id)).join(', ')}
              </div>
            </div>
          )
        })}
      </div>
      <div className="mode-panel__targets-row">
        <span className="mode-panel__row-label" style={{ color: TEAM_COLORS[0] }}>虎队</span>
        <div className="mode-panel__targets mode-panel__targets--team0">
          {state.targets.map((name, index) => {
            const doneByTeam0 = ((Number(state.completed?.[0] ?? 0) >> index) & 1) === 1
            const justDone = completedNow.includes(name)
            return (
              <div
                key={`t0-${name}-${index}`}
                className={`mode-panel__target${doneByTeam0 ? ' is-done' : ''}${justDone && doneByTeam0 ? ' is-just-done' : ''}`}
                style={doneByTeam0 ? { borderColor: TEAM_COLORS[0] } : undefined}
                title={doneByTeam0 ? `${TEAM_NAMES[0]} 完成` : '未完成'}
              >
                <span className="mode-panel__target-index">{index + 1}</span>
                <span className="mode-panel__target-name">{name}</span>
                {doneByTeam0 && <span className="mode-panel__target-check">✓</span>}
              </div>
            )
          })}
        </div>
      </div>
      <div className="mode-panel__targets-row">
        <span className="mode-panel__row-label" style={{ color: TEAM_COLORS[1] }}>龙队</span>
        <div className="mode-panel__targets mode-panel__targets--team1">
          {state.targets.map((name, index) => {
            const doneByTeam1 = ((Number(state.completed?.[1] ?? 0) >> index) & 1) === 1
            const justDone = completedNow.includes(name)
            return (
              <div
                key={`t1-${name}-${index}`}
                className={`mode-panel__target${doneByTeam1 ? ' is-done' : ''}${justDone && doneByTeam1 ? ' is-just-done' : ''}`}
                style={doneByTeam1 ? { borderColor: TEAM_COLORS[1] } : undefined}
                title={doneByTeam1 ? `${TEAM_NAMES[1]} 完成` : '未完成'}
              >
                <span className="mode-panel__target-index">{index + 1}</span>
                <span className="mode-panel__target-name">{name}</span>
                {doneByTeam1 && <span className="mode-panel__target-check">✓</span>}
              </div>
            )
          })}
        </div>
      </div>
      {ended && winnerTeam !== null && (
        <div className="mode-panel__result">
          {winnerTeam === -2
            ? '本局平局'
            : `${TEAM_NAMES[winnerTeam]} 获胜`}
        </div>
      )}
    </div>
  )
}
