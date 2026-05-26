import { Link } from 'react-router-dom'
import { Button, Card, Layout, Space, Typography } from 'antd'

const { Header, Content } = Layout
const { Title, Paragraph, Text } = Typography

interface PlaceholderPageProps {
  title: string
  description: string
}

function PlaceholderPage({ title, description }: PlaceholderPageProps) {
  return (
    <Layout style={{ minHeight: '100vh', background: '#ffffff' }}>
      <Header
        style={{
          background: 'rgba(255, 255, 255, 0.1)',
          backdropFilter: 'blur(10px)',
          borderBottom: '1px solid rgba(255, 255, 255, 0.1)',
          padding: '0 24px',
        }}
      >
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', height: '100%' }}>
          <Title level={3} style={{ margin: 0, color: '#1890ff' }}>
            云雀
          </Title>
          <Link to="/">
            <Button>返回大厅</Button>
          </Link>
        </div>
      </Header>

      <Content style={{ padding: '40px 24px', display: 'grid', placeItems: 'center' }}>
        <Card style={{ width: 'min(720px, 100%)', borderRadius: 12 }}>
          <Space direction="vertical" size={12}>
            <Text type="secondary">功能开发中</Text>
            <Title level={2} style={{ margin: 0 }}>
              {title}
            </Title>
            <Paragraph style={{ marginBottom: 0 }}>{description}</Paragraph>
            <Paragraph style={{ marginBottom: 0 }}>
              当前状态：暂不可用。
            </Paragraph>
          </Space>
        </Card>
      </Content>
    </Layout>
  )
}

export default PlaceholderPage