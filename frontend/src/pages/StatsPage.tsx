import { useCallback, useEffect, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import {
  Button,
  Card,
  Col,
  DatePicker,
  Divider,
  Empty,
  Form,
  InputNumber,
  Layout,
  notification,
  Row,
  Select,
  Space,
  Spin,
  Statistic,
  Switch,
  Table,
  Typography,
} from 'antd'
import {
  ArrowLeftOutlined,
  BarChartOutlined,
  FilterOutlined,
  SearchOutlined,
  UnorderedListOutlined,
} from '@ant-design/icons'
import type { ColumnsType } from 'antd/es/table'
import type { SorterResult } from 'antd/es/table/interface'
import dayjs from 'dayjs'
import { apiRequest, buildWebSocketUrl } from '../lib/backend'
import './StatsPage.css'

const { Header, Content } = Layout
const { Title, Text } = Typography
const { RangePicker } = DatePicker

// ── Types ──────────────────────────────────────────────────────────

interface StatsPlayer {
  player_id: number
  username: string
}

interface RoundEntryRecordRaw {
  game_folder: string
  game_index: number
  drawn_game: boolean
  winner: number
  from: number
  fan: number
  time: number
  turn: number
  all_players: { player_id: number; username: string }[]
  fans_str?: string
}

interface FanStatItem {
  fan_name: string
  occurance_count: number
  occurance_rate: number
}

interface StatsData {
  avg_win_pt: number
  avg_hwin_pt: number
  avg_rkwin_pt: number
  avg_win_turn: number
  avg_hwin_turn: number
  avg_rkwin_turn: number
  avg_turn: number
  avg_meld_count: number
  win_rate: number
  hwin_rate: number
  drawn_game_rate: number
  meld_rate: number
  tot_rounds: number
  tot_wins: number
  fan_stats?: FanStatItem[]
  avg_shoot_pt?: number
  avg_selfdrawned_pt?: number
  shoot_rate?: number
  selfdrawned_rate?: number
  avg_round_pt?: number
  avg_selfdrawned_turn?: number
  avg_shoot_turn?: number
  player_name?: string
  player_id?: number
}

interface RoundEntryRecord {
  key: number
  tableId: string
  roundId: string
  replayFolder: string
  roundIndex: number
  fan: string
  fanValue: number
  fans_str: string
  timestamp: number
  formattedTime: string
  players: string[]
  winner: string
  shooter: string
  drawn: boolean
}

type RecordSortField = 'time' | 'fan'
type RecordSortOrder = 'asc' | 'desc'

// ── Fan name list ──────────────────────────────────────────────────

const FAN_NAMES: string[] = [
  '和牌', '天和', '地和', '岭上开花', '海底捞月', '河底捞鱼', '抢杠',
  '十三幺', '七对', '门前清', '四暗杠', '三暗杠', '双暗杠', '暗杠',
  '四杠', '三杠', '双杠', '四暗刻', '三暗刻', '对对和',
  '十二归', '八归', '三叠对', '二叠对', '叠对',
  '字一色', '大四喜', '小四喜', '四喜对', '风牌三刻', '风牌七对', '风牌六对',
  '大三元', '小三元', '三元六对', '三元对',
  '番牌四刻', '番牌三刻', '番牌二刻', '番牌刻', '番牌七对', '番牌六对', '番牌五对', '番牌四副', '番牌三副', '番牌二副', '番牌',
  '清幺九', '混幺九', '清带幺', '混带幺', '九莲宝灯',
  '清一色', '混一色', '五门齐', '混一数', '二数', '二聚', '三聚', '四聚',
  '连数', '间数', '镜数', '映数', '满庭芳',
  '四同顺', '三同顺', '二般高', '一般高', '四连刻', '三连刻',
  '四步高', '三步高', '四连环', '三连环', '一气贯通',
  '七连对', '六连对', '五连对', '四连对',
  '三色同刻', '三色同顺', '三色二对', '三色同对', '三色连刻', '三色贯通',
  '镜同', '镜同三对', '镜同二对', '双龙会',
]

const fanOptions = FAN_NAMES.map((name, index) => ({ label: name, value: index }))

// ── Component ──────────────────────────────────────────────────────

export default function StatsPage() {
  const navigate = useNavigate()

  const [loading, setLoading] = useState(true)
  const [currentStats, setCurrentStats] = useState<StatsData | null>(null)
  const [fanStats, setFanStats] = useState<FanStatItem[]>([])
  const [playerList, setPlayerList] = useState<StatsPlayer[]>([])
  const [recordsData, setRecordsData] = useState<RoundEntryRecord[]>([])
  const [totalRecords, setTotalRecords] = useState(0)
  const [recordsLoading, setRecordsLoading] = useState(false)
  const [activeColumn, setActiveColumn] = useState<'data' | 'records'>('data')
  const [pageSize, setPageSize] = useState(16)
  const [currentPage, setCurrentPage] = useState(1)
  const [recordSortField, setRecordSortField] = useState<RecordSortField>('time')
  const [recordSortOrder, setRecordSortOrder] = useState<RecordSortOrder>('desc')
  const [showPlayerStats, setShowPlayerStats] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const wsRef = useRef<WebSocket | null>(null)
  const wsReady = useRef(false)
  const pendingMessages = useRef<unknown[]>([])
  const [filterForm] = Form.useForm()
  const watchedPlayerName: string[] | undefined = Form.useWatch('playerName', filterForm)
  const watchedExcludeSuperior = Form.useWatch('exclude_superior_fans', filterForm)
  const watchedIncludeNonstandard = Form.useWatch('include_nonstandard', filterForm)
  const playerHasSelection = Array.isArray(watchedPlayerName) && watchedPlayerName.length >= 1

  useEffect(() => {
    if (!playerHasSelection) setShowPlayerStats(false)
  }, [playerHasSelection])

  // ── Helpers ──────────────────────────────────────────────────────

  const sendWs = useCallback((msg: unknown) => {
    if (wsRef.current && wsReady.current) {
      wsRef.current.send(JSON.stringify(msg))
    } else {
      pendingMessages.current.push(msg)
    }
  }, [])

  const requestRecords = useCallback(
    (
      page: number,
      pgSize: number,
      sortField: RecordSortField = recordSortField,
      sortOrder: RecordSortOrder = recordSortOrder,
    ) => {
      setRecordsLoading(true)
      sendWs({ type: 'get_records', sort_field: sortField, sort_order: sortOrder,
        offset: (page - 1) * pgSize, limit: pgSize })
    },
    [recordSortField, recordSortOrder, sendWs],
  )

  const buildAndSendFilter = useCallback(
    (values: Record<string, unknown>, playerStatsOn?: boolean) => {
      setLoading(true)
      setError(null)
      const filter: Record<string, unknown> = {}
      const usePlayerStats = playerStatsOn !== undefined ? playerStatsOn : showPlayerStats

      if (Array.isArray(values.playerName) && values.playerName.length > 0) {
        filter.player_filter_positive = (values.playerName as number[]).map(Number)
        if (usePlayerStats && (values.playerName as number[]).length >= 1) {
          filter.player_name = String(Number((values.playerName as number[])[0]))
        }
      }
      if (values.winPlayer !== undefined && values.winPlayer !== null && values.winPlayer !== '') {
        filter.win_player_filter_positive = String(Number(values.winPlayer as number))
      }
      if (values.shootPlayer !== undefined && values.shootPlayer !== null && values.shootPlayer !== '') {
        filter.shoot_player_filter_positive = String(Number(values.shootPlayer as number))
      }
      if (Array.isArray(values.fan_filter_positive) && values.fan_filter_positive.length > 0)
        filter.fan_filter_positive = values.fan_filter_positive
      if (Array.isArray(values.fan_filter_negative) && values.fan_filter_negative.length > 0)
        filter.fan_filter_negative = values.fan_filter_negative
      if (values.winType) {
        if (values.winType === 'self_drawn') filter.self_drawn_filter_positive = true
        else if (values.winType === 'ron') filter.self_drawn_filter_negative = true
      }
      if (values.min_fan !== undefined && values.min_fan !== null) filter.min_fan = values.min_fan
      if (values.max_fan !== undefined && values.max_fan !== null) filter.max_fan = values.max_fan
      if (Array.isArray(values.timeRange) && values.timeRange.length === 2) {
        filter.time_start = (values.timeRange[0] as dayjs.Dayjs).valueOf() * 1000
        filter.time_end = (values.timeRange[1] as dayjs.Dayjs).valueOf() * 1000
      }
      filter.exclude_superior_fans = values.exclude_superior_fans !== undefined ? values.exclude_superior_fans : false
      filter.include_nonstandard = values.include_nonstandard !== undefined ? values.include_nonstandard : false

      sendWs({ type: 'filter', filter })
    },
    [sendWs, showPlayerStats],
  )

  const handleFilter = useCallback(
    (values: Record<string, unknown>) => buildAndSendFilter(values),
    [buildAndSendFilter],
  )

  const clearFilters = useCallback(() => {
    filterForm.resetFields()
    setShowPlayerStats(false)
    setLoading(true)
    setError(null)
    sendWs({ type: 'overall_stats' })
  }, [filterForm, sendWs])

  // ── WebSocket ────────────────────────────────────────────────────

  useEffect(() => {
    apiRequest<{ players: StatsPlayer[] }>('/stats/players')
      .then((resp) => setPlayerList(resp.players ?? []))
      .catch(() => {})

    const ws = new WebSocket(buildWebSocketUrl('/ws/stats'))
    wsRef.current = ws
    let pingTimer: ReturnType<typeof setInterval> | null = null

    ws.onopen = () => {
      wsReady.current = true
      while (pendingMessages.current.length > 0) {
        ws.send(JSON.stringify(pendingMessages.current.shift()))
      }
      sendWs({ type: 'overall_stats' })
      pingTimer = setInterval(() => {
        if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ type: 'ping' }))
      }, 15000)
    }

    ws.onmessage = (event) => {
      try {
        const msg = JSON.parse(event.data) as Record<string, unknown>

        if (msg.type === 'pong') return

        let inner = msg
        if (msg.payload && typeof msg.payload === 'object' && msg.payload !== null && 'type' in msg.payload) {
          inner = msg.payload as Record<string, unknown>
        }

        if (inner.type === 'stats') {
          const data = (inner.data ?? {}) as StatsData
          data.tot_rounds = (inner.tot_records as number) ?? data.tot_rounds ?? 0
          setCurrentStats(data)
          setFanStats((data.fan_stats ?? []) as FanStatItem[])
          setTotalRecords((inner.tot_records as number) ?? 0)
          setLoading(false)
          setCurrentPage(1)
          setRecordsData([])
          requestRecords(1, pageSize)
          return
        }

        if (inner.type === 'records') {
          const rawEntries = (inner.round_entries ?? []) as RoundEntryRecordRaw[]
          setRecordsData(processRoundEntries(rawEntries))
          setTotalRecords((inner.total as number) ?? 0)
          setRecordsLoading(false)
          return
        }

        if (inner.type === 'error') {
          const errMsg = ((inner as { message?: string }).message) ?? '服务器错误'
          console.error('Stats WS error:', errMsg)
          setError(errMsg)
          setLoading(false)
          setRecordsLoading(false)
          notification.error({ message: '错误', description: errMsg, placement: 'topRight' })
        }
      } catch { /* ignore */ }
    }

    ws.onclose = () => {
      wsReady.current = false
      if (pingTimer) clearInterval(pingTimer)
    }

    ws.onerror = () => setLoading(false)

    return () => {
      wsReady.current = false
      if (pingTimer) clearInterval(pingTimer)
      if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING) ws.close()
      wsRef.current = null
    }
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  // ── Process round entries like old impl ──────────────────────────

  const extractTableId = (replayFolder: string) => {
    const parts = replayFolder.split('_')
    return parts[0] || replayFolder
  }

  const getRoundDisplay = (roundIndex: number) => {
    const winds = ['東', '南', '西', '北']
    const windIndex = Math.floor((roundIndex - 1) / 4) % 4
    const gameNumber = ((roundIndex + 3) % 4) + 1 + Math.floor((roundIndex - 1) / 16) * 4
    return `${winds[windIndex]}${gameNumber}`
  }

  const formatTimestamp = (ms: number) => {
    return dayjs(ms).format('YYYY-MM-DD HH:mm')
  }

  const processRoundEntries = useCallback(
    (entries: RoundEntryRecordRaw[]) =>
      entries.map((entry, index) => ({
        key: index,
        tableId: extractTableId(entry.game_folder),
        roundId: getRoundDisplay(entry.game_index),
        replayFolder: entry.game_folder,
        roundIndex: entry.game_index,
        fan: (entry.fan > 0 ? entry.fan.toFixed(2) : '流局'),
        fanValue: entry.fan,
        fans_str: entry.fans_str || '',
        timestamp: entry.time,
        formattedTime: formatTimestamp(entry.time),
        players: (entry.all_players ?? []).map(p => p.username || `#${p.player_id}`),
        winner: entry.drawn_game ? ''
          : ((entry.all_players ?? []).find(p => p.player_id === entry.winner)?.username ?? `#${entry.winner}`),
        shooter: (entry.drawn_game || entry.winner === entry.from) ? ''
          : ((entry.all_players ?? []).find(p => p.player_id === entry.from)?.username ?? `#${entry.from}`),
        drawn: entry.drawn_game,
      })),
    [],
  )

  // ── Records table columns ────────────────────────────────────────

  const recordColumns: ColumnsType<RoundEntryRecord> = [
    { title: '桌号', dataIndex: 'tableId', key: 'tableId', width: 70 },
    { title: '小局', dataIndex: 'roundId', key: 'roundId', width: 40 },
    { title: '東', dataIndex: 'players', key: 'player0', width: 80, ellipsis: true,
      render: (players: string[]) => players?.[0] || 'N/A' },
    { title: '南', dataIndex: 'players', key: 'player1', width: 80, ellipsis: true,
      render: (players: string[]) => players?.[1] || 'N/A' },
    { title: '西', dataIndex: 'players', key: 'player2', width: 80, ellipsis: true,
      render: (players: string[]) => players?.[2] || 'N/A' },
    { title: '北', dataIndex: 'players', key: 'player3', width: 80, ellipsis: true,
      render: (players: string[]) => players?.[3] || 'N/A' },
    { title: '和', dataIndex: 'winner', key: 'winner', width: 80, ellipsis: true,
      render: (winner: string) => winner || 'N/A' },
    { title: '铳', dataIndex: 'shooter', key: 'shooter', width: 80, ellipsis: true,
      render: (shooter: string) => shooter || 'N/A' },
    { title: '番种', dataIndex: 'fans_str', key: 'fans_str', width: 140, ellipsis: true,
      render: (str: string) => str || 'N/A' },
    { title: '番数', dataIndex: 'fan', key: 'fan', width: 55,
      sorter: true,
      sortOrder: recordSortField === 'fan' ? (recordSortOrder === 'asc' ? 'ascend' : 'descend') : null,
      render: (fan: string) => fan },
    { title: '时间', dataIndex: 'formattedTime', key: 'time', width: 130,
      sorter: true,
      sortOrder: recordSortField === 'time' ? (recordSortOrder === 'asc' ? 'ascend' : 'descend') : null,
      render: (t: string) => t || 'N/A' },
    { title: '操作', key: 'action', width: 80,
      render: (_: unknown, r: RoundEntryRecord) => (
        <Button type="link" size="small"
          onClick={() => window.open(`/replay?session=${encodeURIComponent(r.replayFolder)}&round=${r.roundIndex}`, '_blank')}>
          回放
        </Button>
      )},
  ]

  // ── Render ────────────────────────────────────────────────────────

  const stats = currentStats
  const playerOpts = playerList.map(p => ({ label: `${p.username} (#${p.player_id})`, value: p.player_id }))

  return (
    <Layout className="stats-page">
      <Header className="stats-page__header">
        <div className="stats-page__header-inner">
          <Space>
            <Button type="text" icon={<ArrowLeftOutlined />} onClick={() => navigate('/')} style={{ color: '#333' }}>
              返回大厅
            </Button>
            <Title level={3} className="stats-page__title">
              {/* <BarChartOutlined style={{ marginRight: 8 }} /> */}
              统计数据
            </Title>
          </Space>

          <Space>
            <Button type={activeColumn === 'data' ? 'primary' : 'default'} icon={<BarChartOutlined />}
              onClick={() => setActiveColumn('data')} size="small">数据</Button>
            <Button type={activeColumn === 'records' ? 'primary' : 'default'} icon={<UnorderedListOutlined />}
              onClick={() => { setActiveColumn('records'); if (recordsData.length === 0 && totalRecords > 0) requestRecords(currentPage, pageSize) }}
              size="small">记录</Button>
          </Space>
        </div>
      </Header>

      <Content className="stats-page__content">
        {/* ── Filter card ─────────────────────────────────────────── */}
        <Card className="stats-page__card">
          <Title level={4}>筛选条件</Title>
          <Form form={filterForm} layout="inline" onFinish={handleFilter} style={{ width: '100%' }}>
            <Row gutter={[16, 16]} style={{ width: '100%' }}>
              <Col xs={24} sm={12} md={6}>
                <Form.Item name="playerName" label="玩家">
                  <Select mode="multiple" placeholder="选择玩家" showSearch allowClear
                    options={playerOpts}
                    filterOption={(input, option) => (option?.label as string)?.toLowerCase().includes(input.toLowerCase())}
                    maxTagCount={2} maxTagTextLength={8} />
                </Form.Item>
              </Col>
              <Col xs={24} sm={12} md={6}>
                <Form.Item name="winPlayer" label="和家">
                  <Select placeholder="和家" showSearch allowClear options={playerOpts}
                    filterOption={(input, option) => (option?.label as string)?.toLowerCase().includes(input.toLowerCase())} />
                </Form.Item>
              </Col>
              <Col xs={24} sm={12} md={6}>
                <Form.Item name="shootPlayer" label="铳家">
                  <Select placeholder="铳家" showSearch allowClear options={playerOpts}
                    filterOption={(input, option) => (option?.label as string)?.toLowerCase().includes(input.toLowerCase())} />
                </Form.Item>
              </Col>
              <Col xs={24} sm={12} md={6}>
                <Form.Item name="winType" label="和牌方式">
                  <Select placeholder="和牌方式" allowClear options={[{ label: '自摸', value: 'self_drawn' }, { label: '铳和', value: 'ron' }]} />
                </Form.Item>
              </Col>
              <Col xs={24} sm={12} md={6}>
                <Form.Item name="fan_filter_positive" label="包含番种">
                  <Select mode="multiple" placeholder="输入番种名称搜索" options={fanOptions} showSearch
                    filterOption={(input, option) => (option?.label as string)?.toLowerCase().includes(input.toLowerCase())}
                    maxTagTextLength={10} />
                </Form.Item>
              </Col>
              <Col xs={24} sm={12} md={6}>
                <Form.Item name="fan_filter_negative" label="排除番种">
                  <Select mode="multiple" placeholder="输入番种名称搜索" options={fanOptions} showSearch
                    filterOption={(input, option) => (option?.label as string)?.toLowerCase().includes(input.toLowerCase())}
                    maxTagTextLength={10} />
                </Form.Item>
              </Col>
              <Col xs={24} sm={12} md={6}>
                <Form.Item name="min_fan" label="最低番数">
                  <InputNumber placeholder="最低番数" min={0} />
                </Form.Item>
              </Col>
              <Col xs={24} sm={12} md={6}>
                <Form.Item name="max_fan" label="最高番数">
                  <InputNumber placeholder="最高番数" min={0} />
                </Form.Item>
              </Col>
              <Col xs={24} sm={12} md={8}>
                <Form.Item name="timeRange" label="时间范围">
                  <RangePicker placeholder={['开始时间', '结束时间']} />
                </Form.Item>
              </Col>
              <Col xs={28} sm={14} md={10}>
                <Form.Item label=" ">
                  <Space size="large">
                    <Space>
                      <Switch checked={!!watchedExcludeSuperior}
                        onChange={(checked) => filterForm.setFieldsValue({ exclude_superior_fans: checked })} />
                      <Text>排除上位番种</Text>
                    </Space>
                    <Space>
                      <Switch checked={!!watchedIncludeNonstandard}
                        onChange={(checked) => filterForm.setFieldsValue({ include_nonstandard: checked })} />
                      <Text>非标准对局</Text>
                    </Space>
                    <Space>
                      <Switch checked={showPlayerStats && playerHasSelection}
                        onChange={(checked) => {
                          setShowPlayerStats(checked)
                          buildAndSendFilter(filterForm.getFieldsValue(), checked)
                        }}
                        disabled={!playerHasSelection} />
                      <Text style={{ color: playerHasSelection ? undefined : '#999' }}>玩家数据</Text>
                    </Space>
                  </Space>
                </Form.Item>
                <Form.Item name="exclude_superior_fans" hidden initialValue={false}><Switch /></Form.Item>
                <Form.Item name="include_nonstandard" hidden initialValue={false}><Switch /></Form.Item>
              </Col>
              <Col xs={16} sm={10} md={7}>
                <Form.Item label=" ">
                  <Space>
                    <Button type="primary" htmlType="submit" icon={<SearchOutlined />}>筛选</Button>
                    <Button onClick={clearFilters} icon={<FilterOutlined />}>清除</Button>
                  </Space>
                </Form.Item>
              </Col>
            </Row>
          </Form>
        </Card>

        {/* ── Loading / Error ─────────────────────────────────────── */}
        {loading ? (
          <Card className="stats-page__loading-card">
            <Spin size="large" />
            <div style={{ marginTop: 16 }}><Text>正在加载统计数据...</Text></div>
          </Card>
        ) : error ? (
          <Card className="stats-page__card">
            <div style={{ textAlign: 'center' }}>
              <Text type="danger">{error}</Text>
              <Button size="small" onClick={() => sendWs({ type: 'overall_stats' })} style={{ marginLeft: 8 }}>重试</Button>
            </div>
          </Card>
        ) : null}

        {/* ── Data column ─────────────────────────────────────────── */}
        {!loading && activeColumn === 'data' && stats ? (
          <Card className="stats-page__card">
            <Title level={4}>统计数据</Title>
            {stats.tot_rounds === 0 ? (
              <Empty description="暂无统计数据" />
            ) : (
              <Row gutter={[16, 16]}>
                {/* Overall stats */}
                {!stats.player_name && !stats.player_id ? (
                  <>
                    <Col xs={12} sm={8} md={6}><Statistic title="总小局数" value={stats.tot_rounds || 0} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="流局率" value={`${((stats.drawn_game_rate ?? 0) * 100).toFixed(2)}%`} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="自摸率" value={`${((stats.hwin_rate ?? 0) * 100).toFixed(2)}%`} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均和牌点数" value={(stats.avg_win_pt ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均自摸点数" value={(stats.avg_hwin_pt ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均铳和点数" value={(stats.avg_rkwin_pt ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均和牌巡目" value={(stats.avg_win_turn ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均自摸巡目" value={(stats.avg_hwin_turn ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均铳和巡目" value={(stats.avg_rkwin_turn ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均结束巡目" value={(stats.avg_turn ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="鸣牌率" value={`${((stats.meld_rate ?? 0) * 100).toFixed(2)}%`} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均鸣牌组数" value={(stats.avg_meld_count ?? 0).toFixed(2)} /></Col>
                  </>
                ) : null}

                {/* Player-specific stats */}
                {stats.player_name || stats.player_id ? (
                  <>
                    <Col span={24}><Divider>玩家数据</Divider></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="总小局数" value={stats.tot_rounds || 0} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="流局率" value={`${((stats.drawn_game_rate ?? 0) * 100).toFixed(2)}%`} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="和牌率" value={`${((stats.win_rate ?? 0) * 100).toFixed(2)}%`} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="放铳率" value={`${((stats.shoot_rate ?? 0) * 100).toFixed(2)}%`} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="自摸率" value={`${((stats.hwin_rate ?? 0) * 100).toFixed(2)}%`} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="被摸率" value={`${((stats.selfdrawned_rate ?? 0) * 100).toFixed(2)}%`} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均和牌点数" value={(stats.avg_win_pt ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均自摸点数" value={(stats.avg_hwin_pt ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均铳和点数" value={(stats.avg_rkwin_pt ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均放铳点数" value={(stats.avg_shoot_pt ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均被摸点数" value={(stats.avg_selfdrawned_pt ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均小局点数" value={(stats.avg_round_pt ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均和牌巡目" value={(stats.avg_win_turn ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均放铳巡目" value={(stats.avg_shoot_turn ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均被摸巡目" value={(stats.avg_selfdrawned_turn ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均结束巡目" value={(stats.avg_turn ?? 0).toFixed(2)} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="鸣牌率" value={`${((stats.meld_rate ?? 0) * 100).toFixed(2)}%`} /></Col>
                    <Col xs={12} sm={8} md={6}><Statistic title="平均鸣牌组数" value={(stats.avg_meld_count ?? 0).toFixed(2)} /></Col>
                  </>
                ) : null}
              </Row>
            )}
          </Card>
        ) : null}

        {/* ── Fan statistics ─────────────────────────────────────── */}
        {!loading && activeColumn === 'data' && fanStats.length > 0 ? (
          <Card className="stats-page__card" style={{ marginTop: 16 }}>
            <Title level={4}>番种统计</Title>
            <Table<FanStatItem>
              dataSource={fanStats}
              columns={[
                { title: '番种', dataIndex: 'fan_name', key: 'fan_name', width: 120 },
                { title: '出现次数', dataIndex: 'occurance_count', key: 'occurance_count', width: 100,
                  sorter: (a, b) => a.occurance_count - b.occurance_count,
                  defaultSortOrder: 'descend' },
                { title: '出现率', dataIndex: 'occurance_rate', key: 'occurance_rate', width: 100,
                  render: (rate: number) => `${(rate * 100).toFixed(2)}%`,
                  sorter: (a, b) => a.occurance_rate - b.occurance_rate },
              ]}
              rowKey="fan_name"
              size="small"
              pagination={false}
            />
          </Card>
        ) : null}

        {/* ── Records column ──────────────────────────────────────── */}
        {activeColumn === 'records' ? (
          <Card className="stats-page__card">
            <Table<RoundEntryRecord>
              dataSource={recordsData}
              columns={recordColumns}
              rowKey={(r) => `${r.replayFolder}_${r.roundIndex}`}
              loading={recordsLoading}
              onChange={(pagination, _filters, sorter) => {
                const sorterState = Array.isArray(sorter) ? sorter[0] : sorter as SorterResult<RoundEntryRecord>
                const nextSortField: RecordSortField = sorterState?.columnKey === 'fan' ? 'fan' : 'time'
                const nextSortOrder: RecordSortOrder = sorterState?.order === 'ascend' ? 'asc' : 'desc'
                const nextPage = pagination.current ?? 1
                const nextPageSize = pagination.pageSize ?? pageSize

                setCurrentPage(nextPage)
                setPageSize(nextPageSize)
                setRecordSortField(nextSortField)
                setRecordSortOrder(nextSortOrder)
                requestRecords(nextPage, nextPageSize, nextSortField, nextSortOrder)
              }}
              pagination={{
                current: currentPage, pageSize, total: totalRecords,
                showSizeChanger: true, pageSizeOptions: ['8', '16', '32', '64'],
              }}
              size="small" scroll={{ x: 800 }}
            />
          </Card>
        ) : null}
      </Content>
    </Layout>
  )
}
