import { defineConfig } from 'vite'
import { svelte } from '@sveltejs/vite-plugin-svelte'

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [svelte()],
  base: './', // 使用相对路径，方便嵌入式环境
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  }
})
