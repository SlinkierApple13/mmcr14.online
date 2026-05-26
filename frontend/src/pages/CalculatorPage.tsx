import { useState } from 'react'
import {
  Alert,
  Button,
  Card,
  Col,
  Form,
  Input,
  Layout,
  Row,
  Space,
  Typography,
} from 'antd'
import { ArrowLeftOutlined, CalculatorOutlined, ClearOutlined } from '@ant-design/icons'
import { useNavigate } from 'react-router-dom'

import { ApiError, apiRequest } from '../lib/backend'
import './CalculatorPage.css'

const { Header, Content } = Layout
const { Text } = Typography

const syntaxReference = [
  { label: '牌张', value: '123456789m/p/s, ESWNCFP' },
  { label: '门风', value: '! 东, @ 南, # 西, $ 北' },
  { label: '和牌', value: '% 自摸, ^ 岭上开花/抢杠, & 海底/河底, * 天和/地和' },
  { label: '副露', value: '[] 暗杠, () 其余副露' },
]

interface CalculatorFormValues {
  expression: string
}

export default function CalculatorPage() {
  const navigate = useNavigate()
  const [form] = Form.useForm<CalculatorFormValues>()
  const [result, setResult] = useState('')
  const [error, setError] = useState('')
  const [loading, setLoading] = useState(false)

  async function handleFinish(values: CalculatorFormValues) {
    setLoading(true)
    setError('')
    setResult('')

    try {
      const response = await apiRequest<string>('/calc', {
        method: 'POST',
        body: { expression: values.expression.trim() },
      })
      setResult(response)
    } catch (caughtError) {
      const message =
        caughtError instanceof ApiError ? caughtError.message : '无法连接到计算服务，请稍后重试。'
      setError(message)
    } finally {
      setLoading(false)
    }
  }

  function handleReset() {
    form.resetFields()
    setResult('')
    setError('')
  }

  return (
    <Layout className="calculator-page">
      <Header className="calculator-page__header">
        <div className="calculator-page__header-bar">
        <Button type="text" icon={<ArrowLeftOutlined />} onClick={() => navigate('/')}>
            返回大厅
        </Button>
        </div>
      </Header>

      <Content className="calculator-page__content">
        <div className="calculator-page__shell">
          <Row gutter={[18, 18]}>
            <Col xs={24} lg={15}>
              <Card
                className="calculator-page__card"
                title={
                  <Space>
                    {/* <CalculatorOutlined /> */}
                    <span>计算器</span>
                  </Space>
                }
              >
                {error ? (
                  <Alert
                    type="error"
                    showIcon
                    message="计算失败"
                    description={error}
                    style={{ marginBottom: 16 }}
                  />
                ) : null}

                <Form
                  form={form}
                  layout="vertical"
                  initialValues={{ expression: '' }}
                  onFinish={handleFinish}
                >
                  <Form.Item
                    name="expression"
                    label="输入和牌 (和张放在最后)"
                    rules={[{ required: true, message: '请输入和牌。' }]}
                  >
                    <Input.TextArea
                      rows={6}
                      className="calculator-page__textarea"
                      placeholder="例: [WWWW](123m)789m78999s#%&"
                    />
                  </Form.Item>

                  <Space wrap>
                    <Button type="primary" htmlType="submit" loading={loading} icon={<CalculatorOutlined />}>
                      计算
                    </Button>
                    <Button icon={<ClearOutlined />} onClick={handleReset} disabled={loading}>
                      清空
                    </Button>
                  </Space>
                </Form>

                <div className="calculator-page__result-panel">
                  <Text className="calculator-page__result-label">计算结果</Text>
                  <pre className="calculator-page__result-box">{result || ''}</pre>
                </div>
              </Card>
            </Col>

            <Col xs={24} lg={9}>
              <Space direction="vertical" size={18} className="calculator-page__side">
                <Card className="calculator-page__card" title="输入说明">
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    {syntaxReference.map((item) => (
                      <div key={item.label} className="calculator-page__syntax-row">
                        <Text strong>{item.label}</Text>
                        <Text className="calculator-page__syntax-value">{item.value}</Text>
                      </div>
                    ))}
                  </Space>
                </Card>
              </Space>
            </Col>
          </Row>
        </div>
      </Content>
    </Layout>
  )
}