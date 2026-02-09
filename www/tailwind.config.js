/** @type {import('tailwindcss').Config} */
export default {
  content: ['./src/**/*.{html,js,svelte,ts}'],
  theme: {
    extend: {
      colors: {
        primary: '#3b82f6', // 蓝色点缀
        bs: '#ffffff', // 白色基调
      }
    },
  },
  plugins: [],
}
