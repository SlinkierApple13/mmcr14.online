import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import {
  Avatar,
  Button,
  Card,
  Col,
  Divider,
  Dropdown,
  Form,
  Input,
  InputNumber,
  Layout,
  Modal,
  Row,
  Space,
  Switch,
  Table,
  Typography,
  notification,
} from 'antd'
import type { TableProps } from 'antd'
import {
  BookOutlined,
  DownloadOutlined,
  FileTextOutlined,
  LoadingOutlined,
  LockOutlined,
  LoginOutlined,
  LogoutOutlined,
  PlusCircleOutlined,
  UserOutlined,
} from '@ant-design/icons'
import { ApiError, apiRequest, buildWebSocketUrl } from '../lib/backend'
import { encodeWatchingSeat } from '../lib/replay'
import { rankToChinese } from '../lib/chinese'
import {
  clearStoredAuth,
  clearStoredSessionId,
  loadStoredAuth,
  saveStoredAuth,
  saveStoredSessionId,
} from '../lib/storage'
import type {
  ActiveSessionSummary,
  AuthSession,
  LobbyListPayload,
  PendingSessionSummary,
  SessionSnapshot,
  SessionSnapshotPayload,
} from '../lib/types'

const { Header, Content, Footer } = Layout
const { Title, Text } = Typography
const ROUND_WINDS = ['東', '南', '西', '北'] as const
const PRIMARY_TIMER_SECONDS_MIN = 3
const PRIMARY_TIMER_SECONDS_MAX = 15
const SECONDARY_TIMER_SECONDS_MIN = 3
const AUXILIARY_TIMER_SECONDS_MIN = 0
const AUXILIARY_TIMER_SECONDS_MAX = 45
const TOTAL_ROUNDS_MIN = 1
const TOTAL_ROUNDS_MAX = 32

type LobbySessionRow = PendingSessionSummary

interface CreateQueueValues {
  primary_timer_seconds: number
  secondary_timer_seconds: number
  auxiliary_timer_seconds: number
  total_rounds: number
  recorded: boolean
  singleplayer: boolean
  debug_mode: boolean
  unranked: boolean
}

interface AuthFormValues {
  username: string
  password: string
}

interface BigWinRow {
  key: string
  winner: string
  fan: number
  fans_str: string
  time: number
  game_folder: string
  game_index: number
  winner_seat: number
}

function describeError(error: unknown, fallback: string): string {
  if (error instanceof ApiError) {
    return error.message
  }
  if (error instanceof Error && error.message) {
    return error.message
  }
  return fallback
}

function formatTimerTriple(primaryTimerMs: number, secondaryTimerMs: number, auxiliaryTimerMs: number): string {
  return `${Math.floor(primaryTimerMs / 1000)}/${Math.floor(secondaryTimerMs / 1000)}+${Math.floor(auxiliaryTimerMs / 1000)}`
}

function formatRoundReadable(roundCounter: number): string {
  const roundIndex = Math.max(0, roundCounter - 1)
  return `${ROUND_WINDS[Math.floor(roundIndex / 4) % ROUND_WINDS.length]}${(roundIndex % 4) + 1 + 4 * Math.floor(roundIndex / 16)}`
}

// function toPerspectiveLabel(rawSeat: unknown): string {
//   const seat = typeof rawSeat === 'number' ? rawSeat : Number(rawSeat)
//   if (!Number.isFinite(seat)) {
//     return 'east'
//   }
//   const normalized = ((Math.trunc(seat) % 4) + 4) % 4
//   return ['east', 'south', 'west', 'north'][normalized] ?? 'east'
// }

function displayPendingNames(names: string[]): string {
  const filtered = names.map((name) => name.trim()).filter((name) => name.length > 0)
  return filtered.length > 0 ? filtered.join(', ') : '暂无'
}

function displayActiveNames(names: string[]): string {
  const filtered = names.map((name) => (name.trim().length > 0 ? name : '(COM)'))
  return filtered.length > 0 ? filtered.join(', ') : '暂无'
}

function snapshotSessionId(session: SessionSnapshot): number {
  return session.phase === 'active' ? session.session_id : session.summary.session_id
}

function CreateQueueModal({
  open,
  loading,
  form,
  onCancel,
  onSubmit,
}: {
  open: boolean
  loading: boolean
  form: ReturnType<typeof Form.useForm<CreateQueueValues>>[0]
  onCancel: () => void
  onSubmit: (values: CreateQueueValues) => void
}) {
  const primaryTimerSeconds = Form.useWatch('primary_timer_seconds', form)
  const singleplayer = Form.useWatch('singleplayer', form) ?? false
  const debugMode = Form.useWatch('debug_mode', form) ?? false
  // const unranked = Form.useWatch('unranked', form) ?? false
  const recorded = Form.useWatch('recorded', form) ?? true

  useEffect(() => {
    if (singleplayer || debugMode) {
      form.setFieldValue('recorded', false)
    }
    if (!recorded || singleplayer || debugMode) {
      form.setFieldValue('unranked', true)
    }
  }, [form, singleplayer, debugMode, recorded])

  const validateWholeSecondsInRange = useCallback(
    (value: number | null | undefined, min: number, max: number, label: string): Promise<void> => {
      if (typeof value !== 'number' || Number.isNaN(value)) {
        return Promise.reject(new Error(`请输入${label}!`))
      }
      if (!Number.isInteger(value)) {
        return Promise.reject(new Error(`${label}必须为整数`))
      }
      if (value < min || value > max) {
        return Promise.reject(new Error(`${label}必须在 ${min} 到 ${max} 之间`))
      }
      return Promise.resolve()
    },
    [],
  )

  return (
    <Modal
      title="创建新牌桌"
      open={open}
      onCancel={onCancel}
      footer={[
        <Button key="cancel" onClick={onCancel}>
          取消
        </Button>,
        <Button key="submit" type="primary" loading={loading} onClick={() => form.submit()}>
          创建
        </Button>,
      ]}
    >
      <Form<CreateQueueValues>
        form={form}
        layout="vertical"
        onFinish={onSubmit}
        initialValues={{
          primary_timer_seconds: 7,
          secondary_timer_seconds: 4,
          auxiliary_timer_seconds: 12,
          total_rounds: 16,
          recorded: true,
          singleplayer: false,
          debug_mode: false,
          unranked: false,
        }}
      >
        <Form.Item
          label="首要时限 (秒)"
          name="primary_timer_seconds"
          rules={[
            { required: true, message: '请输入首要时限!' },
            {
              validator: (_, value) =>
                validateWholeSecondsInRange(
                  value,
                  PRIMARY_TIMER_SECONDS_MIN,
                  PRIMARY_TIMER_SECONDS_MAX,
                  '首要时限',
                ),
            },
          ]}
        >
          <InputNumber min={PRIMARY_TIMER_SECONDS_MIN} max={PRIMARY_TIMER_SECONDS_MAX} step={1} precision={0} />
        </Form.Item>
        <Form.Item
          label="次要时限 (秒)"
          name="secondary_timer_seconds"
          dependencies={['primary_timer_seconds']}
          rules={[
            { required: true, message: '请输入次要时限!' },
            ({ getFieldValue }) => ({
              validator: (_, value) => {
                const primaryValue = getFieldValue('primary_timer_seconds')
                if (typeof primaryValue !== 'number' || Number.isNaN(primaryValue) || !Number.isInteger(primaryValue)) {
                  return Promise.reject(new Error('请先输入有效的首要时限'))
                }
                return validateWholeSecondsInRange(
                  value,
                  SECONDARY_TIMER_SECONDS_MIN,
                  primaryValue,
                  '次要时限',
                )
              },
            }),
          ]}
        >
          <InputNumber
            min={SECONDARY_TIMER_SECONDS_MIN}
            max={typeof primaryTimerSeconds === 'number' ? Math.max(primaryTimerSeconds, SECONDARY_TIMER_SECONDS_MIN) : PRIMARY_TIMER_SECONDS_MAX}
            step={1}
            precision={0}
          />
        </Form.Item>
        <Form.Item
          label="储备时限 (秒)"
          name="auxiliary_timer_seconds"
          rules={[
            { required: true, message: '请输入储备时限!' },
            {
              validator: (_, value) =>
                validateWholeSecondsInRange(
                  value,
                  AUXILIARY_TIMER_SECONDS_MIN,
                  AUXILIARY_TIMER_SECONDS_MAX,
                  '储备时限',
                ),
            },
          ]}
        >
          <InputNumber min={AUXILIARY_TIMER_SECONDS_MIN} max={AUXILIARY_TIMER_SECONDS_MAX} step={1} precision={0} />
        </Form.Item>
        <Form.Item
          label="总小局数"
          name="total_rounds"
          rules={[
            { required: true, message: '请选择总小局数!' },
            {
              validator: (_, value) =>
                validateWholeSecondsInRange(
                  value,
                  TOTAL_ROUNDS_MIN,
                  TOTAL_ROUNDS_MAX,
                  '总小局数',
                ),
            },
          ]}
        >
          <InputNumber min={TOTAL_ROUNDS_MIN} max={TOTAL_ROUNDS_MAX} step={1} precision={0} />
        </Form.Item>
        <Form.Item label="保留记录" name="recorded" valuePropName="checked">
          <Switch disabled={singleplayer || debugMode} />
        </Form.Item>
        <Form.Item label="休闲模式" name="unranked" valuePropName="checked">
          <Switch disabled={singleplayer || debugMode || !recorded} />
        </Form.Item>
        <Form.Item label="单人游戏" name="singleplayer" valuePropName="checked">
          <Switch />
        </Form.Item>
        <Form.Item label="调试模式" name="debug_mode" valuePropName="checked">
          <Switch />
        </Form.Item>
      </Form>
    </Modal>
  )
}

function LobbyPage() {
  const navigate = useNavigate()
  const [auth, setAuth] = useState<AuthSession | null>(() => loadStoredAuth())
  const [verifying, setVerifying] = useState(true)
  const [loading, setLoading] = useState(false)
  const [fetchingTables, setFetchingTables] = useState(false)
  const [isRegister, setIsRegister] = useState(false)
  const [isLoginModalVisible, setIsLoginModalVisible] = useState(false)
  const [isCreateQueueModalVisible, setIsCreateQueueModalVisible] = useState(false)
  const [queues, setQueues] = useState<LobbySessionRow[]>([])
  const [games, setGames] = useState<ActiveSessionSummary[]>([])
  const wsRef = useRef<WebSocket | null>(null)
  const reconnectTimeoutRef = useRef<number | null>(null)
  const pingIntervalRef = useRef<number | null>(null)
  const [form] = Form.useForm<AuthFormValues>()
  const [createQueueForm] = Form.useForm<CreateQueueValues>()

  const [rating, setRating] = useState<{ mu: number; tau: number; sigma: number; points: number; level: number } | null>(null)
  const [bigWins, setBigWins] = useState<BigWinRow[]>([])
  const [fetchingBigWins, setFetchingBigWins] = useState(false)

  const token = auth?.session.token ?? null
  const loggedIn = auth !== null

  const applyLobbyPayload = useCallback(
    (payload: LobbyListPayload) => {
      setQueues(payload.sessions ?? [])
      setGames(payload.active_sessions ?? [])
    },
    [],
  )

  const fetchBigWins = useCallback(() => {
    setFetchingBigWins(true)
    apiRequest<{ big_wins: BigWinRow[] }>('/stats/big_wins')
      .then((resp) => {
        setBigWins(
          (resp.big_wins ?? []).map((item, index) => ({
            ...item,
            key: `${item.game_folder}_${item.game_index}_${index}`,
          })),
        )
      })
      .catch(() => {})
      .finally(() => setFetchingBigWins(false))
  }, [])

  const fetchLobby = useCallback(
    async (authToken: string | null = token) => {
      setFetchingTables(true)
      try {
        const payload = await apiRequest<LobbyListPayload>('/lobby/sessions', {
          method: 'GET',
          token: authToken,
        })
        applyLobbyPayload(payload)
      } catch (error) {
        setQueues([])
        setGames([])
        notification.error({
          message: '获取牌桌信息失败',
          description: describeError(error, '无法获取牌桌列表，请重试'),
          placement: 'topRight',
          duration: 5,
        })
      } finally {
        setFetchingTables(false)
      }
    },
    [applyLobbyPayload],
  )

  const verifyToken = useCallback(async () => {
    if (!token) {
      setVerifying(false)
      return
    }

    try {
      const verified = await apiRequest<AuthSession>('/me', { method: 'GET', token })
      saveStoredAuth(verified)
      setAuth(verified)
    } catch {
      clearStoredAuth()
      setAuth(null)
    } finally {
      setVerifying(false)
    }
  }, [token])

  useEffect(() => {
    void verifyToken()
  }, [verifyToken])

  useEffect(() => {
    if (!verifying && loggedIn) {
      apiRequest<{ mu: number; tau: number; sigma: number; points: number; level: number }>('/rating', { method: 'GET', token })
        .then(setRating)
        .catch(() => setRating(null))
    }
  }, [verifying, loggedIn, token])

  useEffect(() => {
    let cancelled = false

    const clearReconnectTimer = () => {
      if (reconnectTimeoutRef.current !== null) {
        window.clearTimeout(reconnectTimeoutRef.current)
        reconnectTimeoutRef.current = null
      }
    }

    const clearPingInterval = () => {
      if (pingIntervalRef.current !== null) {
        window.clearInterval(pingIntervalRef.current)
        pingIntervalRef.current = null
      }
    }

    let missedPongCount = 0

    const scheduleReconnect = () => {
      if (cancelled || reconnectTimeoutRef.current !== null) {
        return
      }
      reconnectTimeoutRef.current = window.setTimeout(() => {
        reconnectTimeoutRef.current = null
        connect()
      }, 3000)
    }

    const connect = () => {
      if (cancelled) {
        return
      }

      clearReconnectTimer()

      if (wsRef.current && wsRef.current.readyState !== WebSocket.CLOSED) {
        wsRef.current.close()
      }

      clearPingInterval()

      const socket = new WebSocket(buildWebSocketUrl('/ws/lobby', token))
      wsRef.current = socket

      socket.onopen = () => {
        clearReconnectTimer()
        missedPongCount = 0
        socket.send(JSON.stringify({ type: 'lobby.list' }))
        clearPingInterval()
        pingIntervalRef.current = window.setInterval(() => {
          if (socket.readyState === WebSocket.OPEN) {
            socket.send(JSON.stringify({ type: 'ping' }))
            missedPongCount += 1
            if (missedPongCount >= 2) {
              socket.close()
            }
          }
        }, 5_000)
      }

      socket.onmessage = (event) => {
        missedPongCount = 0
        const envelope = JSON.parse(event.data) as { type?: string; payload?: LobbyListPayload }
        if (envelope.type === 'lobby.list.snapshot' && envelope.payload) {
          void applyLobbyPayload(envelope.payload)
        }
      }

      socket.onerror = () => {
        if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING) {
          socket.close()
        }
      }

      socket.onclose = () => {
        clearPingInterval()
        if (wsRef.current === socket) {
          wsRef.current = null
        }
        scheduleReconnect()
      }
    }

    connect()

    return () => {
      cancelled = true
      clearReconnectTimer()
      clearPingInterval()
      wsRef.current?.close(1000, 'Lobby page cleanup')
      wsRef.current = null
    }
  }, [applyLobbyPayload, token])

  useEffect(() => {
    fetchBigWins()
  }, [fetchBigWins])

  const handleSubmit = async (values: AuthFormValues) => {
    setLoading(true)
    try {
      if (isRegister) {
        await apiRequest<AuthSession>('/auth/register', {
          method: 'POST',
          body: { username: values.username, password: values.password },
        })
        notification.success({
          message: '注册成功',
          description: '您已成功注册，现在可以登录了。',
          placement: 'topRight',
        })
        setIsRegister(false)
        form.resetFields(['password'])
        return
      }

      const loggedInSession = await apiRequest<AuthSession>('/auth/login', {
        method: 'POST',
        body: { username: values.username, password: values.password },
      })
      saveStoredAuth(loggedInSession)
      setAuth(loggedInSession)
      setIsLoginModalVisible(false)
      form.resetFields()
      notification.success({
        message: '登录成功',
        description: `欢迎回来，${loggedInSession.player.username}！`,
        placement: 'topRight',
      })
      await fetchLobby(loggedInSession.session.token)
    } catch (error) {
      notification.error({
        message: isRegister ? '注册失败' : '登录失败',
        description: describeError(error, isRegister ? '注册失败，请重试' : '登录失败，请重试'),
        placement: 'topRight',
        duration: 5,
      })
    } finally {
      setLoading(false)
    }
  }

  const handleLogout = async () => {
    try {
      await apiRequest('/auth/logout', { method: 'POST', token })
    } catch {
      // Local cleanup remains authoritative.
    }

    clearStoredAuth()
    clearStoredSessionId()
    setAuth(null)
    setQueues([])
    setGames([])
    notification.success({
      message: '已退出登录',
      description: '本地会话已清除。',
      placement: 'topRight',
    })
    void fetchLobby(null)
  }

  const handleCreateQueue = async (values: CreateQueueValues) => {
    if (!token) {
      setIsLoginModalVisible(true)
      return
    }

    setLoading(true)
    try {
      const payload = await apiRequest<SessionSnapshotPayload>('/lobby/sessions', {
        method: 'POST',
        token,
        body: {
          game_config: {
            primary_timer_ms: values.primary_timer_seconds * 1000,
            secondary_timer_ms: values.secondary_timer_seconds * 1000,
            auxiliary_timer_ms: values.auxiliary_timer_seconds * 1000,
            round_count: values.total_rounds,
            recorded: values.singleplayer || values.debug_mode ? false : values.recorded,
            debug_mode: values.debug_mode,
            unranked: values.unranked,
          },
          queue_config: {
            public_session: !values.singleplayer,
            singleplayer: values.singleplayer,
          },
        },
      })

      const sessionId = snapshotSessionId(payload.session)
      saveStoredSessionId(sessionId)
      setIsCreateQueueModalVisible(false)
      createQueueForm.resetFields()
      notification.success({
        message: '创建成功',
        description: `牌桌 ${sessionId} 已创建。`,
        placement: 'topRight',
      })
      navigate(`/game?gameId=${sessionId}`)
    } catch (error) {
      notification.error({
        message: '创建失败',
        description: describeError(error, '创建牌桌失败，请重试'),
        placement: 'topRight',
        duration: 5,
      })
    } finally {
      setLoading(false)
    }
  }

  const handleJoinQueue = async (sessionId: number) => {
    if (!token) {
      setIsLoginModalVisible(true)
      return
    }

    try {
      await apiRequest(`/lobby/sessions/${sessionId}/join`, {
        method: 'POST',
        token,
      })
      saveStoredSessionId(sessionId)
      navigate(`/game?gameId=${sessionId}`)
    } catch (error) {
      notification.error({
        message: '加入失败',
        description: describeError(error, '加入牌桌失败，请重试'),
        placement: 'topRight',
        duration: 5,
      })
    }
  }

  const userMenuItems = useMemo(
    () =>
      loggedIn
        ? [
            {
              key: 'logout',
              icon: <LogoutOutlined />,
              label: '退出登录',
              onClick: handleLogout,
            },
          ]
        : [
            {
              key: 'login',
              icon: <LoginOutlined />,
              label: '登录',
              onClick: () => setIsLoginModalVisible(true),
            },
          ],
    [loggedIn],
  )

  const queueColumns: TableProps<LobbySessionRow>['columns'] = [
    {
      title: '桌号',
      dataIndex: 'session_id',
      key: 'session_id',
      render: (value: number) => <Text>{value}</Text>,
    },
    {
      title: '玩家数量',
      dataIndex: 'occupied_seat_count',
      key: 'occupied_seat_count',
      render: (count: number) => `${count}/4`,
    },
    {
      title: '总小局数',
      dataIndex: 'round_count',
      key: 'round_count',
    },
    {
      title: '时限',
      key: 'time_limits',
      render: (_, record) =>
        formatTimerTriple(record.primary_timer_ms, record.secondary_timer_ms, record.auxiliary_timer_ms),
    },
    {
      title: '保留记录',
      dataIndex: 'recorded',
      key: 'recorded',
      render: (_recorded: boolean, record: LobbySessionRow) =>
        record.debug_mode ? '调试模式' : (_recorded ? '是' : '否'),
    },
    {
      title: '等待玩家',
      dataIndex: 'names',
      key: 'names',
      render: (names: string[]) => displayPendingNames(names),
    },
    {
      title: '操作',
      key: 'action',
      render: (_, record) => (
        <Button
          type="primary"
          onClick={() => {
            void handleJoinQueue(record.session_id)
          }}
          disabled={!record.can_join || loading}
        >
          加入
        </Button>
      ),
    },
  ]

  const gameColumns: TableProps<ActiveSessionSummary>['columns'] = [
    {
      title: '桌号',
      dataIndex: 'session_id',
      key: 'session_id',
      render: (value: number) => <Text>{value}</Text>,
    },
    {
      title: '总小局数',
      dataIndex: 'round_count',
      key: 'round_count',
    },
    {
      title: '当前进度',
      dataIndex: 'round_counter',
      key: 'round_counter',
      render: (roundCounter: number, record) => (record.ended ? '结束' : formatRoundReadable(roundCounter)),
    },
    {
      title: '时限',
      key: 'time_limits',
      render: (_, record) =>
        formatTimerTriple(record.primary_timer_ms, record.secondary_timer_ms, record.auxiliary_timer_ms),
    },
    {
      title: '保留记录',
      dataIndex: 'recorded',
      key: 'recorded',
      render: (_recorded: boolean, record: ActiveSessionSummary) =>
        record.debug_mode ? '调试模式' : (_recorded ? '是' : '否'),
    },
    {
      title: '玩家',
      dataIndex: 'names',
      key: 'names',
      render: (names: string[]) => displayActiveNames(names),
    },
    {
      title: '操作',
      key: 'action',
      render: (_, record) => {
        if (auth && record.names.includes(auth.player.username)) {
          return (
            <Button type="default" onClick={() => navigate(`/game?gameId=${record.session_id}`)}>
              返回
            </Button>
          )
        }
        return null
      },
    },
  ]
  // (big win feature removed — backend endpoint not implemented)

  // ── Rating card help text ───────────────────────────────────

  // ── Pending (queue) columns ─────────────────────────────────

  return (
    <Layout style={{ minHeight: '100vh', background: '#ffffff' }}>
      <Header
        style={{
          background: '#ffffff',
          backdropFilter: 'none',
          borderBottom: '1px solid rgba(0, 0, 0, 0.08)',
          padding: '0 24px',
        }}
      >
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', height: '100%' }}>
          <Space>
            <Title level={3} style={{ margin: 0, color: '#1890ff' }}>
              云雀
            </Title>
          </Space>

          <Space>
            <Text style={{ color: 'rgba(160, 160, 160, 0.8)', fontSize: '14px' }}>约桌/交流群: 1015232788</Text>
            <Dropdown
              menu={{
                items: [
                  {
                    key: 'rules',
                    label: (
                      <a href="/rule.pdf" target="_blank" rel="noopener noreferrer">
                        规则文档
                      </a>
                    ),
                    icon: <BookOutlined />,
                  },
                  {
                    key: 'scoring',
                    label: (
                      <a href="/scoring-samples.pdf" target="_blank" rel="noopener noreferrer">
                        计分示例
                      </a>
                    ),
                    icon: <DownloadOutlined />,
                  },
                  {
                    key: 'brief',
                    label: (
                      <a href="/brief.pdf" target="_blank" rel="noopener noreferrer">
                        一页纸
                      </a>
                    ),
                    icon: <FileTextOutlined />,
                  },
                ],
              }}
              placement="bottomRight"
              trigger={['click']}
            >
              <Button type="link" style={{ color: 'black', fontSize: '16px', padding: '4px 8px' }}>
                查看规则
              </Button>
            </Dropdown>
            <Button type="link" onClick={() => navigate('/calc')} style={{ color: 'black', fontSize: '16px', padding: '4px 8px' }}>
              计算器
            </Button>
            <Button type="link" onClick={() => navigate('/stats')} style={{ color: 'black', fontSize: '16px', padding: '4px 8px' }}>
              统计数据
            </Button>
            <Text style={{ color: 'black', fontSize: '16px' }}>{auth ? auth.player.username : '未登录'}</Text>
            <Dropdown menu={{ items: userMenuItems }} placement="bottomRight" trigger={['click']}>
              <Avatar icon={<UserOutlined />} style={{ cursor: 'pointer' }} />
            </Dropdown>
          </Space>
        </div>
      </Header>

      <Content
        style={{
          padding: '40px 24px',
          background: 'transparent',
          display: 'flex',
          flexDirection: 'column',
          justifyContent: 'flex-start',
        }}
      >
        {verifying ? (
          <div style={{ textAlign: 'center', color: '#1f1f1f', fontSize: '20px' }}>
            <LoadingOutlined style={{ marginRight: 8 }} />
            加载中...
          </div>
        ) : (
          <Row gutter={[24, 24]} style={{ maxWidth: 1200, margin: '0 auto', width: '100%' }}>
            {rating && (
              <Col xs={24} lg={24} xl={24}>
                <Card title="玩家信息" style={{ borderRadius: '12px', marginBottom: 8, borderColor: 'rgba(0, 0, 0, 0.06)', boxShadow: '0 14px 32px rgba(0, 0, 0, 0.06)' }}>
                  <Space direction="vertical" size={2}>
                    <Text strong style={{ fontSize: '18px' }}>{auth?.player.username}</Text>
                    <Text style={{ fontSize: '14px', color: '#555' }}>
                      {rankToChinese(rating.level)} · {rating.points.toFixed(2)}pts · R {rating.mu.toFixed(2)}±{rating.tau.toFixed(2)} · σ {rating.sigma.toFixed(2)}
                    </Text>
                  </Space>
                </Card>
              </Col>
            )}
            <Col xs={24} lg={24} xl={24}>
              <Card
                title={
                  <Space>
                    <span>最近大和</span>
                    {fetchingBigWins && <LoadingOutlined style={{ marginLeft: 8 }} />}
                  </Space>
                }
                style={{ height: '100%', borderRadius: '12px', borderColor: 'rgba(0, 0, 0, 0.06)', boxShadow: '0 14px 32px rgba(0, 0, 0, 0.06)' }}
                bodyStyle={{ padding: '0px' }}
              >
                <Table<BigWinRow>
                  columns={[
                    { title: '和家', dataIndex: 'winner', key: 'winner', width: 80, ellipsis: true },
                    { title: '番数', dataIndex: 'fan', key: 'fan', width: 60,
                      render: (fan: number) => fan.toFixed(2) },
                    { title: '番种', dataIndex: 'fans_str', key: 'fans_str', width: 200, ellipsis: true,
                      render: (str: string) => str || 'N/A' },
                    { title: '时间', dataIndex: 'time', key: 'time', width: 150,
                      render: (t: number) => {
                        const d = new Date(t)
                        return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')} ${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`
                      } },
                    { title: '', key: 'action', width: 80,
                      render: (_: unknown, r: BigWinRow) => (
                        <Button type="link" size="small"
                          onClick={() => window.open(`/replay?session=${encodeURIComponent(r.game_folder)}&round=${r.game_index}&perspective=${encodeWatchingSeat(r.winner_seat)}`, '_blank')}>
                          回放
                        </Button>
                      )},
                  ]}
                  dataSource={bigWins}
                  rowKey="key"
                  pagination={false}
                  loading={fetchingBigWins}
                  locale={{ emptyText: '暂无大和记录' }}
                  size="small"
                />
              </Card>
            </Col>
            <Col xs={24} lg={24} xl={24}>
              <Card
                title={
                  <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
                    <Space>
                      <span>等待中</span>
                      {fetchingTables && <LoadingOutlined style={{ marginLeft: 8 }} />}
                    </Space>
                    <Button
                      type="primary"
                      size="small"
                      onClick={() => {
                        if (!loggedIn) {
                          setIsLoginModalVisible(true)
                          return
                        }
                        setIsCreateQueueModalVisible(true)
                      }}
                      icon={<PlusCircleOutlined />}
                    >
                      创建牌桌
                    </Button>
                  </div>
                }
                style={{ height: '100%', borderRadius: '12px', borderColor: 'rgba(0, 0, 0, 0.06)', boxShadow: '0 14px 32px rgba(0, 0, 0, 0.06)' }}
                bodyStyle={{ padding: '0px' }}
              >
                <Table
                  columns={queueColumns}
                  dataSource={queues}
                  rowKey="session_id"
                  pagination={{ pageSize: 5 }}
                  loading={fetchingTables}
                  locale={{ emptyText: '暂无游戏' }}
                />
              </Card>
            </Col>

            <Col xs={24} lg={24} xl={24}>
              <Card
                title={
                  <Space>
                    <span>已开始</span>
                    {fetchingTables && <LoadingOutlined style={{ marginLeft: 8 }} />}
                  </Space>
                }
                style={{ height: '100%', borderRadius: '12px', borderColor: 'rgba(0, 0, 0, 0.06)', boxShadow: '0 14px 32px rgba(0, 0, 0, 0.06)' }}
                bodyStyle={{ padding: '0px' }}
              >
                <Table
                  columns={gameColumns}
                  dataSource={games}
                  rowKey="session_id"
                  pagination={{ pageSize: 5 }}
                  loading={fetchingTables}
                  locale={{ emptyText: '暂无游戏' }}
                />
              </Card>
            </Col>

          </Row>
        )}
      </Content>

      <Modal
        title={isRegister ? '注册' : '登录'}
        open={isLoginModalVisible}
        onCancel={() => {
          setIsLoginModalVisible(false)
          form.resetFields()
        }}
        footer={null}
        destroyOnClose
        width={400}
      >
        <Form<AuthFormValues>
          name="auth"
          form={form}
          onFinish={(values) => {
            void handleSubmit(values)
          }}
          layout="vertical"
          size="large"
        >
          <Form.Item
            name="username"
            rules={[
              { required: true, message: '请输入用户名!' },
              { max: 30, message: '用户名不能超过30个字符' },
            ]}
          >
            <Input prefix={<UserOutlined />} placeholder="用户名" />
          </Form.Item>

          <Form.Item
            name="password"
            rules={[
              { required: true, message: '请输入密码!' },
              { max: 30, message: '密码不能超过30个字符' },
              {
                pattern: /^[a-zA-Z0-9]+$/,
                message: '密码只能包含字母和数字',
              },
            ]}
          >
            <Input.Password prefix={<LockOutlined />} placeholder="密码" />
          </Form.Item>

          <Form.Item>
            <Button type="primary" htmlType="submit" loading={loading} block style={{ borderRadius: '8px', height: '44px' }}>
              {isRegister ? '注册' : '登录'}
            </Button>
          </Form.Item>
        </Form>

        <Divider plain>
          <Text type="secondary">{isRegister ? '已有账户？' : '没有账户？'}</Text>
        </Divider>

        <Button type="link" onClick={() => setIsRegister((current) => !current)} block style={{ height: '40px' }}>
          {isRegister ? '切换到登录' : '立即注册'}
        </Button>
      </Modal>

      <CreateQueueModal
        open={isCreateQueueModalVisible}
        loading={loading}
        form={createQueueForm}
        onCancel={() => {
          createQueueForm.resetFields()
          setIsCreateQueueModalVisible(false)
        }}
        onSubmit={(values) => {
          void handleCreateQueue(values)
        }}
      />

      <Footer
        style={{
          textAlign: 'center',
          background: '#ffffff',
          color: 'rgba(0, 0, 0, 0.55)',
          borderTop: '1px solid rgba(0, 0, 0, 0.08)',
        }}
      />
    </Layout>
  )
}

export default LobbyPage