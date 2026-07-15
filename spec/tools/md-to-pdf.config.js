// md-to-pdf config: render markdown -> HTML (marked) -> PDF (headless Chrome).
// Stylesheet is GitHub's markdown CSS; matches the rendered preview on
// GitLab / GitHub closely.

const path = require('node:path');

module.exports = {
  stylesheet: [
    path.resolve(__dirname, '..', 'node_modules', 'github-markdown-css', 'github-markdown-light.css'),
    path.resolve(__dirname, 'md-to-pdf.css'),
  ],
  body_class: 'markdown-body',
  marked_options: { gfm: true },
  pdf_options: {
    format: 'A4',
    margin: { top: '15mm', bottom: '15mm', left: '15mm', right: '15mm' },
    printBackground: true,
  },
  launch_options: {
    ...(process.env.PUPPETEER_EXECUTABLE_PATH
      ? { executablePath: process.env.PUPPETEER_EXECUTABLE_PATH }
      : {}),
    args: ['--no-sandbox'],
  },
};
