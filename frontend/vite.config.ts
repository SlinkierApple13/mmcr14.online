import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

const backendTarget = process.env.MMCR_FRONTEND_PROXY_TARGET ?? 'http://127.0.0.1:8080'

const proxy = {
  '/api': {
    target: backendTarget,
    changeOrigin: true,
    secure: false,
  },
  '/ws': {
    target: backendTarget,
    changeOrigin: true,
    secure: false,
    ws: true,
  },
}

export default defineConfig({
  plugins: [react()],
  server: {
    proxy,
  },
  preview: {
    proxy,
  },
})
