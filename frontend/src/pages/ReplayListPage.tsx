import { useDeferredValue, useEffect, useMemo, useRef, useState } from 'react'
import type { Dayjs } from 'dayjs'
import dayjs from 'dayjs'
import relativeTime from 'dayjs/plugin/relativeTime'
import 'dayjs/locale/zh-cn'
import {
  Alert,
  Button,
  Card,
  Col,
  DatePicker,
  Empty,
  Input,
  Layout,
  List,
  Pagination,
  Row,
  Space,
  Spin,
  Statistic,
  Switch,
  Tag,
  Typography,
} from 'antd'
import {
  ArrowLeftOutlined,
  CalendarOutlined,
  ClockCircleOutlined,
  FilterOutlined,
  PlayCircleOutlined,
  SearchOutlined,
  UserOutlined,
} from '@ant-design/icons'
import { useNavigate } from 'react-router-dom'

import { buildWebSocketUrl, sendEnvelope } from '../lib/backend'
import type {
  ReplayInfo,
  ReplayListPagePayload,
  ReplayListQueryPayload,
  WsEnvelope,
} from '../lib/types'
import './ReplayListPage.css'

dayjs.extend(relativeTime)
dayjs.locale('zh-cn')

const { Header, Content } = Layout
const { Title, Text } = Typography
const { Search } = Input
const { RangePicker } = DatePicker

type ReplayDateRange = [Dayjs | null, Dayjs | null] | null

function formatAbsoluteTime(timestampMs: number): string {
  return timestampMs > 0 ? dayjs(timestampMs).format('YYYY-MM-DD HH:mm:ss') : '-'
}

function formatRelativeTime(timestampMs: number | null): string {
  return timestampMs && timestampMs > 0 ? dayjs(timestampMs).fromNow() : '-'
}

function displayPlayerNames(names: string[]): string[] {
  return names.map((name, index) => (name.trim().length > 0 ? name : `玩家${index + 1}`))
}

export default function ReplayListPage() {
  const navigate = useNavigate()
  const socketRef = useRef<WebSocket | null>(null)
  const reconnectTimeoutRef = useRef<number | null>(null)
  const latestRequestIdRef = useRef<string | null>(null)

  const [replays, setReplays] = useState<ReplayInfo[]>([])
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState('')
  const [connected, setConnected] = useState(false)
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(10)
  const [pageCount, setPageCount] = useState(1)
  const [totalCount, setTotalCount] = useState(0)
  const [uniquePlayerCount, setUniquePlayerCount] = useState(0)
  const [latestTimestampMs, setLatestTimestampMs] = useState<number | null>(null)
  const [sessionQuery, setSessionQuery] = useState('')
  const [playerQuery, setPlayerQuery] = useState('')
  const [exactSessionMatch, setExactSessionMatch] = useState(false)
  const [dateRange, setDateRange] = useState<ReplayDateRange>(null)

  const deferredSessionQuery = useDeferredValue(sessionQuery.trim())
  const deferredPlayerQuery = useDeferredValue(playerQuery.trim())
  const startedAfterMs = dateRange?.[0]?.startOf('day').valueOf()
  const startedBeforeMs = dateRange?.[1]?.endOf('day').valueOf()

  useEffect(() => {
    setPage(1)
  }, [deferredSessionQuery, deferredPlayerQuery, exactSessionMatch, startedAfterMs, startedBeforeMs])

  const queryPayload = useMemo<ReplayListQueryPayload>(
    () => ({
      page,
      page_size: pageSize,
      session_query: deferredSessionQuery || undefined,
      player_query: deferredPlayerQuery || undefined,
      exact_session_match: exactSessionMatch,
      started_after_ms: startedAfterMs,
      started_before_ms: startedBeforeMs,
    }),
    [deferredPlayerQuery, deferredSessionQuery, exactSessionMatch, page, pageSize, startedAfterMs, startedBeforeMs],
  )

  const sendQuery = useMemo(
    () => () => {
      const socket = socketRef.current
      if (!socket || socket.readyState !== WebSocket.OPEN) {
        return
      }
      setLoading(true)
      setError('')
      latestRequestIdRef.current = sendEnvelope(socket, 'replay.list.query', queryPayload)
    },
    [queryPayload],
  )

  useEffect(() => {
    let cancelled = false

    const connect = () => {
      if (cancelled) {
        return
      }

      const socket = new WebSocket(buildWebSocketUrl('/ws/replays'))
      socketRef.current = socket

      socket.onopen = () => {
        if (cancelled) {
          return
        }
        setConnected(true)
      }

      socket.onmessage = (event) => {
        let envelope: WsEnvelope<unknown>
        try {
          envelope = JSON.parse(event.data) as WsEnvelope<unknown>
        } catch {
          setError('收到无法解析的回放列表消息。')
          setLoading(false)
          return
        }

        if (envelope.type === 'replay.list.page') {
          if (
            typeof envelope.requestId === 'string'
            && latestRequestIdRef.current
            && envelope.requestId !== latestRequestIdRef.current
          ) {
            return
          }

          const payload = envelope.payload as ReplayListPagePayload
          setReplays(payload.replays ?? [])
          setTotalCount(payload.total_count ?? 0)
          setPage(payload.page ?? 1)
          setPageSize(typeof payload.page_size === 'number' ? payload.page_size : 10)
          setPageCount(payload.page_count ?? 1)
          setUniquePlayerCount(payload.unique_player_count ?? 0)
          setLatestTimestampMs(typeof payload.latest_timestamp_ms === 'number' ? payload.latest_timestamp_ms : null)
          setLoading(false)
          setError('')
          return
        }

        if (envelope.type === 'error') {
          const payload = envelope.payload as { message?: string }
          setError(payload.message ?? '加载回放列表失败。')
          setLoading(false)
        }
      }

      socket.onerror = () => {
        if (!cancelled) {
          setError('回放列表连接失败。')
          setLoading(false)
        }
      }

      socket.onclose = () => {
        if (cancelled) {
          return
        }
        setConnected(false)
        reconnectTimeoutRef.current = window.setTimeout(connect, 3000)
      }
    }

    connect()

    return () => {
      cancelled = true
      if (reconnectTimeoutRef.current !== null) {
        window.clearTimeout(reconnectTimeoutRef.current)
        reconnectTimeoutRef.current = null
      }
      socketRef.current?.close(1000, 'Replay list cleanup')
      socketRef.current = null
    }
  }, [])

  useEffect(() => {
    if (connected) {
      sendQuery()
    }
  }, [connected, sendQuery])

  const clearFilters = () => {
    setSessionQuery('')
    setPlayerQuery('')
    setExactSessionMatch(false)
    setDateRange(null)
    setPage(1)
    setPageSize(10)
  }

  return (
    <Layout className="replay-list-page">
      <Header className="replay-list-header">
        <div className="replay-list-header-bar">
          <Space size="middle" wrap>
            <Button type="text" icon={<ArrowLeftOutlined />} onClick={() => navigate('/')}>
              返回大厅
            </Button>
            <Title level={3} className="replay-list-title">游戏回放</Title>
          </Space>
          <Tag color={connected ? 'green' : 'default'}>{connected ? '已连接' : '重连中'}</Tag>
        </div>
      </Header>

      <Content className="replay-list-content">
        <div className="replay-list-shell">

          <Card className="replay-list-filters">
            <Row gutter={[16, 16]} align="middle">
              <Col xs={24} md={8}>
                <Search
                  allowClear
                  placeholder="搜索对局标识"
                  prefix={<SearchOutlined />}
                  value={sessionQuery}
                  onChange={(event) => setSessionQuery(event.target.value)}
                />
              </Col>
              <Col xs={24} md={7}>
                <Input
                  allowClear
                  placeholder="按玩家筛选"
                  prefix={<UserOutlined />}
                  value={playerQuery}
                  onChange={(event) => setPlayerQuery(event.target.value)}
                />
              </Col>
              <Col xs={24} md={6}>
                <RangePicker
                  value={dateRange}
                  onChange={(value) => setDateRange(value)}
                  style={{ width: '100%' }}
                  placeholder={['开始日期', '结束日期']}
                />
              </Col>
              <Col xs={24} md={3}>
                <div className="replay-list-switch-row">
                  <Switch checked={exactSessionMatch} onChange={setExactSessionMatch} />
                  <Text>精确匹配</Text>
                </div>
              </Col>
              <Col xs={24}>
                <Button icon={<FilterOutlined />} onClick={clearFilters}>清除筛选</Button>
              </Col>
            </Row>
          </Card>

          <Row gutter={[16, 16]} className="replay-list-stats">
            <Col xs={12} md={6}>
              <Card><Statistic title="匹配场数" value={totalCount} /></Card>
            </Col>
            <Col xs={12} md={6}>
              <Card><Statistic title="当前页" value={replays.length} /></Card>
            </Col>
            <Col xs={12} md={6}>
              <Card><Statistic title="涉及玩家" value={uniquePlayerCount} /></Card>
            </Col>
            <Col xs={12} md={6}>
              <Card><Statistic title="最近对局" value={formatRelativeTime(latestTimestampMs)} /></Card>
            </Col>
          </Row>

          <Card className="replay-list-results">
            {error ? <Alert type="error" showIcon message={error} style={{ marginBottom: 16 }} /> : null}

            {loading && replays.length === 0 ? (
              <div className="replay-list-loading">
                <Spin size="large" />
                <Text>正在加载回放列表…</Text>
              </div>
            ) : totalCount === 0 ? (
              <Empty
                description={loading ? '正在加载回放…' : '没有找到匹配的回放'}
                image={Empty.PRESENTED_IMAGE_SIMPLE}
              >
                <Button onClick={clearFilters}>清除筛选条件</Button>
              </Empty>
            ) : (
              <>
                <List
                  itemLayout="vertical"
                  dataSource={replays}
                  renderItem={(replay) => (
                    <List.Item className="replay-list-item" key={replay.session_identifier}>
                      <div className="replay-list-item-head">
                        <Space wrap>
                          <Text strong className="replay-list-session-id">{replay.session_identifier}</Text>
                          <Tag icon={<CalendarOutlined />}>{formatAbsoluteTime(replay.timestamp_ms)}</Tag>
                          <Tag icon={<ClockCircleOutlined />}>{formatRelativeTime(replay.timestamp_ms)}</Tag>
                          <Tag>{replay.round_count} 局</Tag>
                        </Space>
                        <Button
                          type="primary"
                          icon={<PlayCircleOutlined />}
                          onClick={() => navigate(`/replay?session=${encodeURIComponent(replay.session_identifier)}&round=1&watching=east`)}
                        >
                          观看回放
                        </Button>
                      </div>

                      <div className="replay-list-player-row">
                        {displayPlayerNames(replay.player_names).map((name) => (
                          <Tag key={`${replay.session_identifier}-${name}`} color="geekblue" icon={<UserOutlined />}>
                            {name}
                          </Tag>
                        ))}
                      </div>
                    </List.Item>
                  )}
                />

                <Pagination
                  className="replay-list-pagination"
                  current={page}
                  pageSize={pageSize}
                  total={totalCount}
                  showSizeChanger
                  showQuickJumper
                  pageSizeOptions={['10', '20', '50']}
                  onChange={(nextPage, nextPageSize) => {
                    setPage(nextPage)
                    setPageSize(nextPageSize)
                  }}
                  showTotal={(total, range) => `第 ${range[0]}-${range[1]} 条，共 ${total} 条记录（${pageCount} 页）`}
                />
              </>
            )}
          </Card>
        </div>
      </Content>
    </Layout>
  )
}