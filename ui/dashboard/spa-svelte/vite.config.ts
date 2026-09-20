import { defineConfig } from 'vite'
import { svelte } from '@sveltejs/vite-plugin-svelte'
import tailwindcss from "@tailwindcss/vite";

// https://vite.dev/config/
export default defineConfig({
  plugins: [tailwindcss(), svelte()],
    resolve: {
    alias: {
      $lib: '/src/lib'
    }
  },
  define: {
    __BUILD_DATE__: JSON.stringify(new Date().toLocaleString()),
  },
})
