/** @type {import('tailwindcss').Config} */
module.exports = {
    // 重点：告诉 Tailwind 扫描你存放 HTML 的文件夹
    // 假设你的 HTML 在 data 目录下
    content: ["index.html"],
    theme: {
        extend: {},
    },
    plugins: [],
}