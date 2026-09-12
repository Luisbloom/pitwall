/**
 * build-all-pdfs.mjs — regenera el PDF de todos los documentos de docs/.
 *
 * El .md es la unica fuente de verdad. Uso: node build-all-pdfs.mjs [patron]
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const docsDir = path.resolve(__dirname, '..', 'docs');
const filter = process.argv[2];

const files = fs
  .readdirSync(docsDir)
  .filter((f) => f.endsWith('.md') && /^\d{2}-/.test(f))
  .filter((f) => !filter || f.includes(filter))
  .sort();

let failed = 0;
for (const file of files) {
  const src = path.join(docsDir, file);
  const out = src.replace(/\.md$/, '.pdf');
  process.stdout.write(`\n▶ ${file}\n`);
  const r = spawnSync(
    process.execPath,
    [path.join(__dirname, 'md2pdf.mjs'), src, out],
    { stdio: 'inherit' }
  );
  if (r.status !== 0) {
    failed++;
    process.stdout.write(`   ✗ fallo al generar ${path.basename(out)}\n`);
  }
}

process.stdout.write(
  `\n${files.length - failed}/${files.length} PDF generados en docs/\n`
);
process.exit(failed === 0 ? 0 : 1);
