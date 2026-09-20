import { defineConfig } from 'vite'
import { svelte } from '@sveltejs/vite-plugin-svelte'
import tailwindcss from "@tailwindcss/vite";

// https://vite.dev/config/
export default defineConfig({
  plugins: [tailwindcss(), svelte()],
  server: {
    proxy: {
      '/api': {
        target: 'http://localhost:8590',
      }
    }
  },
  base: './',
  build: {
    outDir: 'dist',
    assetsInlineLimit: 100000000, // Inline everything
    cssCodeSplit: false,
    rollupOptions: {
      output: {
        inlineDynamicImports: true, // Single JS file
        manualChunks: undefined,
      }
    }
  },
  define: {
    __BUILD_DATE__: JSON.stringify(new Date().toLocaleString()),
  },
})
