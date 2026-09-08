import { Container, Graphics } from 'pixi.js'
import {
  TILE_WIDTH, TILE_HEIGHT, TILE_RADIUS, LINE_WIDTH,
  FRONT_COLOR, BORDER_COLOR,
} from './constants'
import { Tile } from './Tile'

const WALL_COLS = 17
const WALL_ROWS = 11
// Rows 2, 5 and 8 are unused, leaving exactly 17 * 8 = 136 slots.
const USED_ROWS = [0, 1, 3, 4, 6, 7, 9, 10]

// Wall-index offset for the seat rendered at the bottom of the display.
// Bottom = East → 34, South → 0, West → 102, North → 68.
const SEAT_OFFSETS = [34, 0, 102, 68]

/**
 * Renders the current wall state in a 17x11 grid above the table.
 * Slots follow the standard wall layout: row pairs cover one player's
 * 34-tile wall with even indices in the upper row (descending left→right)
 * and odd indices in the lower row.
 */
export class WallDisplay extends Container {
  private readonly slotTiles: Array<Tile | null> = new Array(136).fill(null)
  private readonly baseWallIndices: number[] = new Array(136).fill(0)
  private readonly slotWallIndices: number[] = new Array(136).fill(0)
  private seatOffset = 0
  private lastState: Array<number | null> | null = null

  constructor(parent: Container) {
    super()

    const bg = new Graphics()
    const padX = TILE_WIDTH / 2
    const padY = TILE_HEIGHT / 2
    bg.roundRect(
      -(WALL_COLS * TILE_WIDTH) / 2 - padX,
      -(WALL_ROWS * TILE_HEIGHT) / 2 - padY,
      WALL_COLS * TILE_WIDTH + padX * 2,
      WALL_ROWS * TILE_HEIGHT + padY * 2,
      TILE_RADIUS * 2,
    )
    bg.fill({ color: FRONT_COLOR, alpha: 0.8 })
    bg.stroke({ color: BORDER_COLOR, width: LINE_WIDTH })
    this.addChild(bg)

    let slot = 0
    for (const row of USED_ROWS) {
      const segment = row < 2 ? 0 : row < 5 ? 1 : row < 8 ? 2 : 3
      const bottom = (row % 3) % 2 === 1
      for (let col = 0; col < WALL_COLS; col += 1) {
        const wallIndex = segment * 34 + (WALL_COLS - 1 - col) * 2 + (bottom ? 1 : 0)
        const tile = Tile.newInvisible(0)
        tile.x = (col - (WALL_COLS - 1) / 2) * TILE_WIDTH
        tile.y = (row - (WALL_ROWS - 1) / 2) * TILE_HEIGHT
        this.addChild(tile)
        this.baseWallIndices[slot] = wallIndex
        this.slotWallIndices[slot] = wallIndex
        this.slotTiles[slot] = tile
        slot += 1
      }
    }

    this.zIndex = 100
    this.eventMode = 'static'
    this.visible = false
    parent.addChild(this)
  }

  /** Rotate the wall rows so the given seat (0=east,1=south,2=west,3=north) is at the bottom. */
  setPerspectiveSeat(seatIndex: number): void {
    const normalized = ((seatIndex % 4) + 4) % 4
    this.seatOffset = SEAT_OFFSETS[normalized] ?? 0
    for (let index = 0; index < this.slotWallIndices.length; index += 1) {
      this.slotWallIndices[index] = (this.baseWallIndices[index] + this.seatOffset) % 136
    }
    if (this.lastState !== null) {
      this.setSlots(this.lastState)
    }
  }

  /** Update every slot from the current wall state (null = consumed). */
  setSlots(state: Array<number | null> | null): void {
    this.lastState = state
    for (let index = 0; index < this.slotTiles.length; index += 1) {
      const tile = this.slotTiles[index]
      if (!tile) continue
      const value = state?.[this.slotWallIndices[index]] ?? null
      if (value !== null && value !== undefined && value > 0) {
        tile.updateTid(value)
        tile.show()
        tile.visible = true
      } else {
        tile.updateTid(0)
        tile.visible = false
      }
    }
  }

  setVisible(visible: boolean): void {
    this.visible = visible
  }
}
