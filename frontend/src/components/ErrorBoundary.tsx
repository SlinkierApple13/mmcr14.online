import { Component, type ErrorInfo, type ReactNode } from 'react'

interface ErrorBoundaryProps {
  children: ReactNode
}

interface ErrorBoundaryState {
  error: Error | null
  info: ErrorInfo | null
}

/**
 * Catches render errors so the app never goes to a blank white page. When an
 * error is caught it shows the message (and a reload button) instead of
 * unmounting the whole tree.
 */
export default class ErrorBoundary extends Component<ErrorBoundaryProps, ErrorBoundaryState> {
  state: ErrorBoundaryState = { error: null, info: null }

  static getDerivedStateFromError(error: Error): Partial<ErrorBoundaryState> {
    return { error }
  }

  componentDidCatch(error: Error, info: ErrorInfo): void {
    this.setState({ info })
    // Surface the error in the console for diagnosis.
    console.error('[ErrorBoundary]', error, info)
  }

  handleReload = (): void => {
    window.location.reload()
  }

  render(): ReactNode {
    if (this.state.error) {
      return (
        <div
          style={{
            display: 'flex',
            flexDirection: 'column',
            alignItems: 'center',
            justifyContent: 'center',
            height: '100vh',
            fontFamily: 'sans-serif',
            padding: '24px',
            boxSizing: 'border-box',
            background: '#f7f7f0',
          }}
        >
          <h2 style={{ marginBottom: 8 }}>页面出现异常</h2>
          <p style={{ color: '#b03a2e', maxWidth: 640, wordBreak: 'break-all', textAlign: 'center' }}>
            {this.state.error.message}
          </p>
          <div style={{ marginTop: 16 }}>
            <button
              type="button"
              onClick={this.handleReload}
              style={{
                padding: '8px 24px',
                fontSize: 15,
                borderRadius: 8,
                border: '1px solid #888',
                background: '#fff',
                cursor: 'pointer',
              }}
            >
              刷新重试
            </button>
          </div>
          {this.state.info && (
            <pre
              style={{
                marginTop: 16,
                maxWidth: 720,
                maxHeight: 200,
                overflow: 'auto',
                fontSize: 11,
                color: '#666',
                background: '#eee',
                padding: 8,
                borderRadius: 6,
                whiteSpace: 'pre-wrap',
                wordBreak: 'break-all',
              }}
            >
              {this.state.info.componentStack}
            </pre>
          )}
        </div>
      )
    }
    return this.props.children
  }
}
