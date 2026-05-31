import { lazy, Suspense } from 'react'
import { BrowserRouter, Navigate, Route, Routes } from 'react-router-dom'
import { ConfigProvider, Spin } from 'antd'
import zhCN from 'antd/locale/zh_CN'
import LobbyPage from './pages/LobbyPage'

const GamePage = lazy(() => import('./pages/GamePage'))
const CalculatorPage = lazy(() => import('./pages/CalculatorPage'))
const ReplayListPage = lazy(() => import('./pages/ReplayListPage'))
const ReplayPage = lazy(() => import('./pages/ReplayPage'))
const StatsPage = lazy(() => import('./pages/StatsPage'))

const appTheme = {
  token: {
    colorPrimary: '#1890ff',
  },
}

function LoadingScreen() {
  return (
    <div
      style={{
        display: 'flex',
        justifyContent: 'center',
        alignItems: 'center',
        height: '100vh',
      }}
    >
      <Spin size="large" />
    </div>
  )
}

function App() {
  return (
    <ConfigProvider locale={zhCN} theme={appTheme}>
      <BrowserRouter basename="/">
        <Suspense fallback={<LoadingScreen />}>
          <Routes>
            <Route path="/" element={<LobbyPage />} />
            <Route path="/game" element={<GamePage />} />
            <Route path="/game/:sessionId" element={<GamePage />} />
            <Route path="/replay" element={<ReplayPage />} />
            <Route path="/replays" element={<ReplayListPage />} />
            <Route
              path="/stats"
              element={<StatsPage />}
            />
            <Route
              path="/calc"
              element={<CalculatorPage />}
            />
            <Route path="*" element={<Navigate to="/" replace />} />
          </Routes>
        </Suspense>
      </BrowserRouter>
    </ConfigProvider>
  )
}

export default App
