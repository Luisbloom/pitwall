/**
 * md2pdf.mjs — genera un PDF a partir de un Markdown del proyecto.
 *
 * El .md es la unica fuente de verdad. Este script solo lo renderiza.
 * Uso: node md2pdf.mjs <entrada.md> [salida.pdf]
 *
 * Incluye: portada, indice generado, numeracion de paginas, tablas legibles,
 * codigo monoespaciado, diagramas Mermaid renderizados y pie de pagina.
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import MarkdownIt from 'markdown-it';
import anchor from 'markdown-it-anchor';
import puppeteer from 'puppeteer-core';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const inputPath = path.resolve(process.argv[2]);
const outputPath = path.resolve(
  process.argv[3] ?? inputPath.replace(/\.md$/i, '.pdf')
);

const CHROME_CANDIDATES = [
  'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
  'C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe',
  'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
  'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe',
];
const chromePath = CHROME_CANDIDATES.find((p) => fs.existsSync(p));
if (!chromePath) throw new Error('No se ha encontrado Chrome ni Edge.');

// ---------------------------------------------------------------- front matter
const rawSource = fs.readFileSync(inputPath, 'utf8');
const fmMatch = rawSource.match(/^---\r?\n([\s\S]*?)\r?\n---\r?\n/);
const meta = {};
if (fmMatch) {
  for (const line of fmMatch[1].split(/\r?\n/)) {
    const m = line.match(/^(\w+):\s*"?(.*?)"?\s*$/);
    if (m) meta[m[1]] = m[2];
  }
}
const body = fmMatch ? rawSource.slice(fmMatch[0].length) : rawSource;

// ---------------------------------------------------------------- markdown
const md = new MarkdownIt({ html: true, linkify: true, typographer: false });

// Los bloques ```mermaid se convierten en <pre class="mermaid"> para que
// la libreria los localice en el navegador.
const defaultFence =
  md.renderer.rules.fence ??
  ((tokens, idx, options, _env, self) => self.renderToken(tokens, idx, options));
md.renderer.rules.fence = (tokens, idx, options, env, self) => {
  const token = tokens[idx];
  if ((token.info || '').trim().toLowerCase() === 'mermaid') {
    return `<pre class="mermaid">${md.utils.escapeHtml(token.content)}</pre>\n`;
  }
  return defaultFence(tokens, idx, options, env, self);
};

const slugify = (s) =>
  'sec-' +
  s
    .toLowerCase()
    .normalize('NFD')
    .replace(/[\u0300-\u036f]/g, '')
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '');

const toc = [];
md.use(anchor, {
  slugify,
  level: [2, 3],
  callback: (token, info) => {
    toc.push({ level: token.tag === 'h2' ? 2 : 3, text: info.title, slug: info.slug });
  },
});

const contentHtml = md.render(body);

const tocHtml = toc
  .map(
    (e) =>
      `<div class="toc-entry toc-l${e.level}"><a href="#${e.slug}"><span class="toc-text">${md.utils.escapeHtml(
        e.text
      )}</span><span class="toc-dots"></span></a></div>`
  )
  .join('\n');

// ---------------------------------------------------------------- plantilla
const esc = (s) => md.utils.escapeHtml(s ?? '');
const html = `<!doctype html>
<html lang="es"><head><meta charset="utf-8">
<title>${esc(meta.title)}</title>
<style>
  @page { size: A4; margin: 20mm 16mm 20mm 16mm; }
  :root {
    --ink:#14181d; --muted:#5b6672; --line:#d5dbe2; --accent:#b8232f;
    --accent-soft:#f4e7e8; --code-bg:#f5f7f9; --head:#0d1117;
  }
  * { box-sizing:border-box; }
  html { -webkit-print-color-adjust:exact; print-color-adjust:exact; }
  body {
    font-family:"Segoe UI","Inter",system-ui,sans-serif;
    font-size:9.4pt; line-height:1.5; color:var(--ink); margin:0;
  }

  /* ---------- portada ---------- */
  .cover { height:257mm; display:flex; flex-direction:column;
           justify-content:space-between; page-break-after:always; }
  .cover-rule { height:6px; background:var(--accent); width:100%; }
  .cover-main { padding-top:38mm; }
  .cover-eyebrow { font-size:9pt; letter-spacing:.22em; text-transform:uppercase;
                   color:var(--accent); font-weight:700; }
  .cover-title { font-size:40pt; line-height:1.03; font-weight:800;
                 color:var(--head); margin:6mm 0 4mm; letter-spacing:-.02em; }
  .cover-sub { font-size:13pt; color:var(--muted); max-width:145mm; line-height:1.4; }
  .cover-meta { border-top:1px solid var(--line); padding-top:5mm;
                display:grid; grid-template-columns:repeat(2,1fr); gap:3mm 10mm; }
  .cover-meta div { font-size:8.6pt; }
  .cover-meta .k { color:var(--muted); text-transform:uppercase;
                   letter-spacing:.09em; font-size:7.4pt; }
  .cover-meta .v { font-weight:600; }
  .cover-note { font-size:7.6pt; color:var(--muted); line-height:1.45;
                border-top:1px solid var(--line); padding-top:3mm; margin-top:4mm; }

  /* ---------- indice ---------- */
  .toc-page { page-break-after:always; }
  .toc-title { font-size:20pt; font-weight:800; color:var(--head);
               border-bottom:2px solid var(--accent); padding-bottom:2mm; margin:0 0 6mm; }
  .toc-entry a { display:flex; align-items:baseline; text-decoration:none;
                 color:var(--ink); gap:2mm; }
  .toc-entry { margin:.9mm 0; font-size:8.8pt; }
  .toc-l2 { font-weight:650; margin-top:2.2mm; }
  .toc-l3 { padding-left:7mm; color:var(--muted); font-size:8.3pt; }
  .toc-dots { flex:1; border-bottom:1px dotted var(--line); transform:translateY(-2px); }

  /* ---------- contenido ---------- */
  h1 { font-size:22pt; font-weight:800; color:var(--head); margin:0 0 4mm;
       letter-spacing:-.01em; }
  h2 { font-size:15pt; font-weight:750; color:var(--head); margin:9mm 0 3mm;
       padding-bottom:1.6mm; border-bottom:2px solid var(--accent);
       page-break-after:avoid; break-after:avoid; }
  h3 { font-size:11.4pt; font-weight:700; color:var(--head); margin:6mm 0 2mm;
       page-break-after:avoid; break-after:avoid; }
  h4 { font-size:9.8pt; font-weight:700; margin:4mm 0 1.5mm;
       color:var(--accent); page-break-after:avoid; }
  p { margin:0 0 2.4mm; }
  ul,ol { margin:0 0 2.6mm; padding-left:5.5mm; }
  li { margin:.7mm 0; }
  strong { font-weight:680; }
  a { color:var(--accent); text-decoration:none; }
  hr { border:0; border-top:1px solid var(--line); margin:6mm 0; }

  code { font-family:"Cascadia Mono","Consolas",ui-monospace,monospace;
         font-size:8.2pt; background:var(--code-bg); padding:.4mm 1mm;
         border-radius:2px; border:1px solid var(--line); }
  pre { background:var(--code-bg); border:1px solid var(--line);
        border-left:3px solid var(--accent); border-radius:3px;
        padding:2.6mm 3mm; overflow:hidden; margin:0 0 3mm;
        page-break-inside:avoid; }
  pre code { background:none; border:0; padding:0; font-size:7.8pt;
             line-height:1.42; white-space:pre-wrap; word-break:break-word; }

  blockquote { margin:0 0 3mm; padding:2.4mm 3.4mm; background:var(--accent-soft);
               border-left:3px solid var(--accent); border-radius:0 3px 3px 0; }
  blockquote p:last-child { margin-bottom:0; }

  table { width:100%; border-collapse:collapse; margin:0 0 3.6mm;
          font-size:7.9pt; page-break-inside:avoid; }
  thead { background:var(--head); color:#fff; }
  th { text-align:left; padding:1.5mm 2mm; font-weight:650; font-size:7.6pt;
       letter-spacing:.02em; }
  td { padding:1.4mm 2mm; border-bottom:1px solid var(--line);
       vertical-align:top; word-break:normal; overflow-wrap:anywhere; }
  tbody tr:nth-child(even) { background:#fafbfc; }
  td code, th code { font-size:7.2pt; }

  .mermaid { background:#fff; border:1px solid var(--line); border-radius:3px;
             padding:3mm; margin:0 0 4mm; text-align:center;
             page-break-inside:avoid; }
  .mermaid svg { max-width:100%; height:auto; }
</style>
</head><body>

<section class="cover">
  <div>
    <div class="cover-rule"></div>
    <div class="cover-main">
      <div class="cover-eyebrow">${esc(meta.docid)} &nbsp;·&nbsp; Documento de ingenieria</div>
      <div class="cover-title">${esc(meta.title)}</div>
      <div class="cover-sub">${esc(meta.subtitle)}</div>
    </div>
  </div>
  <div>
    <div class="cover-meta">
      <div><div class="k">Version del documento</div><div class="v">${esc(meta.version)}</div></div>
      <div><div class="k">Estado</div><div class="v">${esc(meta.status)}</div></div>
      <div><div class="k">Fecha</div><div class="v">${esc(meta.date)}</div></div>
      <div><div class="k">Autor</div><div class="v">${esc(meta.author)}</div></div>
    </div>
    <div class="cover-note">
      Documento generado automaticamente a partir de <code>${esc(path.basename(inputPath))}</code>.
      El fichero Markdown es la unica fuente de verdad; este PDF es un artefacto derivado y no debe editarse por separado.
      <br><br>
      PitWall es un proyecto no oficial sin afiliacion con Electronic Arts Inc., Codemasters, la FIA
      ni Formula One World Championship Limited. Las marcas citadas pertenecen a sus respectivos titulares
      y se emplean unicamente de forma descriptiva.
    </div>
  </div>
</section>

<section class="toc-page">
  <div class="toc-title">Indice</div>
  ${tocHtml}
</section>

<main>
${contentHtml}
</main>

<script type="module">
  import mermaid from 'https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs';
  mermaid.initialize({
    startOnLoad: false,
    theme: 'base',
    themeVariables: {
      fontFamily: 'Segoe UI, Inter, sans-serif',
      fontSize: '13px',
      primaryColor: '#f5f7f9',
      primaryTextColor: '#14181d',
      primaryBorderColor: '#8c98a4',
      lineColor: '#5b6672',
      secondaryColor: '#f4e7e8',
      tertiaryColor: '#ffffff',
    },
    flowchart: { htmlLabels: true, curve: 'basis' },
    securityLevel: 'loose',
  });
  try {
    await mermaid.run({ querySelector: '.mermaid' });
  } catch (e) {
    console.error('mermaid', e);
  }
  window.__mermaidDone = true;
</script>
</body></html>`;

// ---------------------------------------------------------------- render
const tmpHtml = path.join(__dirname, '.render.html');
fs.writeFileSync(tmpHtml, html, 'utf8');

const browser = await puppeteer.launch({
  executablePath: chromePath,
  headless: 'new',
  args: ['--allow-file-access-from-files', '--font-render-hinting=none'],
});
const page = await browser.newPage();
page.on('console', (m) => {
  if (m.type() === 'error') console.warn('   [page]', m.text());
});

await page.goto(pathToFileURL(tmpHtml).href, {
  waitUntil: 'networkidle0',
  timeout: 120000,
});
await page.waitForFunction('window.__mermaidDone === true', { timeout: 120000 });

const diagrams = await page.$$eval('.mermaid svg', (n) => n.length);
const broken = await page.$$eval('.mermaid', (nodes) =>
  nodes
    .map((n, i) => ({ i, bad: !n.querySelector('svg') || !!n.querySelector('.error-icon, .error-text') || /Syntax error|Parse error/i.test(n.querySelector('svg')?.textContent || '') }))
    .filter((x) => x.bad)
    .map((x) => x.i));
if (broken.length) { console.warn('   DIAGRAMAS CON ERROR:', broken); }
const expected = (body.match(/```mermaid/g) || []).length;
console.log(`   diagramas Mermaid renderizados: ${diagrams}/${expected}`);

const footer = `
<div style="width:100%;font-size:7pt;color:#5b6672;
            font-family:'Segoe UI',sans-serif;padding:0 16mm;
            display:flex;justify-content:space-between;border-top:1px solid #d5dbe2;
            padding-top:2mm;">
  <span>${esc(meta.docid)} · ${esc(meta.title)} · v${esc(meta.version)}</span>
  <span>${esc(meta.date)}</span>
  <span>Pag. <span class="pageNumber"></span> de <span class="totalPages"></span></span>
</div>`;

await page.pdf({
  path: outputPath,
  format: 'A4',
  printBackground: true,
  displayHeaderFooter: true,
  headerTemplate: '<div></div>',
  footerTemplate: footer,
  margin: { top: '16mm', bottom: '18mm', left: '14mm', right: '14mm' },
});

if (process.env.PW_SHOT) {
  await page.setViewport({ width: 840, height: 1188 });
  await page.screenshot({ path: process.env.PW_SHOT, clip: { x: 0, y: 0, width: 840, height: 1188 } });
}
await browser.close();
fs.unlinkSync(tmpHtml);

const kb = (fs.statSync(outputPath).size / 1024).toFixed(0);
console.log(`   PDF generado: ${outputPath} (${kb} KB)`);
