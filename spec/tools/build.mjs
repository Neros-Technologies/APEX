// Build *.md -> build/*.pdf.
//
// Step 1: pre-render every ```mermaid``` block to an SVG via mermaid-cli
//         (one shared headless Chrome, opened once). Inline the SVG into
//         the markdown as a data-URI <img>. This sidesteps the timing race
//         that comes with letting md-to-pdf's Chrome render mermaid live.
// Step 2: rewrite cross-document foo.md links to foo.pdf so the per-doc
//         PDFs link to each other.
// Step 3: hand the processed markdown to md-to-pdf.

import { mdToPdf } from 'md-to-pdf';
import { renderMermaid } from '@mermaid-js/mermaid-cli';
import puppeteer from 'puppeteer';
import { PDFDocument, PDFName, PDFString } from 'pdf-lib';
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { slugify } from './slug.mjs';

// md-to-pdf serves the source over a local HTTP server during PDF generation,
// so cross-document hrefs end up baked as `http://localhost:NNNN/Foo.pdf#anchor`
// in the generated PDF's URI actions. Post-process each PDF to strip that
// prefix so the link is just `Foo.pdf#anchor`, which a PDF reader treats as a
// relative path to a sibling file.
// Post-process a generated PDF: strip the localhost prefix that md-to-pdf's
// local HTTP server bakes into link URIs, and set the document title.
//
// md-to-pdf serves the source over a local HTTP server during generation, so
// Chrome both bakes `http://localhost:NNNN/Foo.pdf#anchor` into URI actions
// AND derives the document title from the page URL (there is no <title>).
// Fix both here: rewrite the URIs to relative paths, and set a real title.
async function postProcessPdf(pdfPath, title, version) {
  const bytes = await fs.readFile(pdfPath);
  const doc = await PDFDocument.load(bytes);

  doc.setTitle(title);
  doc.setSubject(`APEX standard v${version} (DRAFT)`);

  for (const page of doc.getPages()) {
    const annots = page.node.Annots();
    if (!annots) continue;
    for (let i = 0; i < annots.size(); i++) {
      const annot = annots.lookup(i);
      const action = annot?.get?.(PDFName.of('A'));
      if (!action) continue;
      const sType = action.get(PDFName.of('S'));
      if (sType?.encodedName !== '/URI') continue;
      const uri = action.get(PDFName.of('URI'));
      if (!uri || typeof uri.asString !== 'function') continue;
      const stripped = uri.asString().replace(/^https?:\/\/localhost:\d+\//, '');
      if (stripped !== uri.asString()) {
        action.set(PDFName.of('URI'), PDFString.of(stripped));
      }
    }
  }

  await fs.writeFile(pdfPath, await doc.save());
}

// Pull a document title from the markdown's first H1 (`# APEX — Core`),
// falling back to the filename stem if there is none.
function titleFromMarkdown(markdown, fallback) {
  const m = markdown.match(/^#\s+(.+?)\s*$/m);
  return m ? m[1].trim() : fallback;
}

// marked extension: emit heading IDs using our slugger (period-between-digits
// becomes hyphen, so §3.1.1 anchors as "3-1-1-...").
const headingExtension = {
  renderer: {
    heading(text, level, raw) {
      const id = slugify(raw);
      return `<h${level} id="${id}">${text}</h${level}>\n`;
    },
  },
};

// Column-width profiles for tables, so PDF tables use a FIXED layout instead
// of the content-driven auto layout (which over-shrinks narrow columns until
// short words like "Value" wrap mid-word). Each profile is a list of column
// widths in percent, summing to 100. The wide column — Description / Meaning /
// the byte Value column — absorbs the slack; the narrow columns are sized
// generously enough that single tokens never wrap.
//
// Profiles are keyed by "<first-header-cell> | <column-count>", which uniquely
// identifies every recurring table shape across the spec set. A table whose
// shape is not listed falls back to `defaultProfile`.
const columnProfiles = {
  // Core: connector pinout
  'Pin|3': [12, 30, 58],
  // Field-layout tables (outer header, CONFIG sub-headers, frame fields)
  'Field|3': [26, 14, 60],
  'Field|2': [22, 78],
  // Value/Name/Meaning style enumerations
  'Value|3': [12, 26, 62],
  'Value|2': [16, 84],
  'Property|2': [26, 74],
  'Quantity|2': [40, 60],
  '`class_msg_id`|4': [16, 18, 22, 44],
  // Activation: byte-layout tables
  'Offset|4': [12, 30, 14, 44],
  'Byte(s)|4': [12, 22, 32, 34],
  // Activation: the 5-column Commands table
  'Value|5': [8, 22, 18, 18, 34],
};
// Fallback: narrow first column, last column wide, the rest split evenly.
function defaultProfile(n) {
  if (n === 1) return [100];
  if (n === 2) return [25, 75];
  const middle = Math.floor(50 / (n - 2));
  return [14, ...Array(n - 2).fill(middle), 100 - 14 - middle * (n - 2)];
}

// Strip HTML tags and collapse whitespace — used to read a header cell's
// plain text so it can key the profile lookup.
function cellText(html) {
  return html.replace(/<[^>]*>/g, '').replace(/\s+/g, ' ').trim();
}

// marked extension: give every table a <colgroup> with fixed column widths
// and a `.fixed-table` class (the CSS pins `table-layout: fixed` to it).
const tableExtension = {
  renderer: {
    table(header, body) {
      const cells = [...header.matchAll(/<th[^>]*>([\s\S]*?)<\/th>/g)];
      const n = cells.length;
      const firstHeader = n > 0 ? cellText(cells[0][1]) : '';
      const widths = columnProfiles[`${firstHeader}|${n}`] ?? defaultProfile(n);
      const colgroup =
        '<colgroup>' +
        widths.map((w) => `<col style="width:${w}%"/>`).join('') +
        '</colgroup>';
      return (
        `<table class="fixed-table">\n${colgroup}\n` +
        `<thead>\n${header}</thead>\n` +
        `<tbody>${body}</tbody>\n</table>\n`
      );
    },
  },
};

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '..');
const root = path.resolve(repo, '..'); // repo root (holds VERSION.txt)
const out = path.join(repo, 'build');
const require_ = createRequire(import.meta.url);
const config = require_(path.join(here, 'md-to-pdf.config.js'));

// Chrome print footer, repeated on every page. Styles must be inline and the
// font-size explicit (the header/footer template context defaults it to ~0).
// The horizontal padding matches the page's 15mm side margins.
function footerTemplate(version) {
  return (
    '<div style="width:100%; padding:0 15mm; font-size:10px; ' +
    'font-family:Helvetica,Arial,sans-serif; color:#888; ' +
    'display:flex; justify-content:space-between;">' +
    `<span>APEX v${version} — DRAFT</span>` +
    '<span>Page <span class="pageNumber"></span> of <span class="totalPages"></span></span>' +
    '</div>'
  );
}

// Inject the version into the title stanza: every doc opens with a
// `**Status:** Draft | **Scope:** ...` line; add a Version segment to it.
// Build-time injection (rather than editing the sources) keeps VERSION.txt
// the single place the version lives.
function injectVersionIntoStanza(markdown, version) {
  return markdown.replace(
    /^(\*\*Status:\*\*[^|\n]*)\|/m,
    `$1| **Version:** ${version} |`,
  );
}

const mermaidBlock = /^```mermaid\r?\n([\s\S]*?)\r?\n```$/gm;

async function inlineMermaid(markdown, browser) {
  const blocks = [...markdown.matchAll(mermaidBlock)];
  if (blocks.length === 0) return markdown;

  const svgs = await Promise.all(
    blocks.map(async ([, def]) => {
      const { data } = await renderMermaid(browser, def, 'svg', {
        viewport: { width: 1200, height: 800, deviceScaleFactor: 1 },
      });
      const b64 = Buffer.from(data).toString('base64');
      return `<p class="mermaid-fig"><img src="data:image/svg+xml;base64,${b64}" alt="mermaid diagram"/></p>`;
    }),
  );

  let i = 0;
  return markdown.replace(mermaidBlock, () => svgs[i++]);
}

function rewriteMdLinks(markdown) {
  // [text](foo.md) and [text](foo.md#anchor) -> .pdf
  return markdown.replace(/(\]\([^)\s]+?)\.md(\)|#)/g, '$1.pdf$2');
}

// Guarantee a PDF named-destination for EVERY heading anchor.
//
// Chrome's PDF printer only emits a /Dests entry for an anchor that is the
// target of a link *within the same document*. A heading linked only from
// another doc (a cross-document `Foo.pdf#anchor` reference) therefore gets no
// destination, and the cross-doc link lands nowhere. To make every anchor
// addressable from anywhere, append a hidden block that links to each heading
// slug internally — this forces Chrome to mint a destination for all of them.
// The block is removed from view by CSS (`.dest-pins { display:none }` would
// drop the links entirely, so it is positioned off-page and zero-height
// instead — the links must remain in the DOM for the destinations to exist).
function pinAllHeadingDestinations(markdown) {
  const slugs = [];
  const re = /^#{1,6}\s+(.+?)\s*$/gm;
  for (const m of markdown.matchAll(re)) {
    const numMatch = m[1].match(/^\d+(?:\.\d+)*\.?\s/);
    if (numMatch) slugs.push(slugify(m[1]));
  }
  if (slugs.length === 0) return markdown;
  const links = slugs.map((s) => `<a href="#${s}">.</a>`).join('');
  const pin = `\n\n<div class="dest-pins" aria-hidden="true">${links}</div>\n`;
  return markdown + pin;
}

async function main() {
  // The single version stamped into every PDF (footer + metadata) and the
  // README. Accept an optional leading "v" in the file; render adds its own.
  const version = (await fs.readFile(path.join(root, 'VERSION.txt'), 'utf8'))
    .trim()
    .replace(/^v/, '');

  await fs.mkdir(out, { recursive: true });
  const sources = (await fs.readdir(repo))
    .filter((f) => f.endsWith('.md'))
    .sort();

  if (sources.length === 0) {
    console.log('No .md files found.');
    return;
  }

  const browser = await puppeteer.launch({
    ...(process.env.PUPPETEER_EXECUTABLE_PATH
      ? { executablePath: process.env.PUPPETEER_EXECUTABLE_PATH }
      : {}),
    args: ['--no-sandbox'],
  });

  try {
    for (const src of sources) {
      const stem = src.replace(/\.md$/, '');
      const dst = path.join(out, `${stem}.pdf`);
      console.log(`  PDF  ${path.relative(repo, dst)}`);

      let md = await fs.readFile(path.join(repo, src), 'utf8');
      const title = titleFromMarkdown(md, stem);
      md = injectVersionIntoStanza(md, version);
      md = await inlineMermaid(md, browser);
      md = rewriteMdLinks(md);
      md = pinAllHeadingDestinations(md);
      md = `<div class="watermark">DRAFT</div>\n\n${md}`;

      await mdToPdf(
        { content: md, basedir: repo },
        {
          ...config,
          pdf_options: {
            ...config.pdf_options,
            displayHeaderFooter: true,
            headerTemplate: '<span></span>',
            footerTemplate: footerTemplate(version),
            // A little extra bottom margin so the footer doesn't crowd the
            // last body line.
            margin: { ...config.pdf_options.margin, bottom: '18mm' },
          },
          marked_extensions: [
            ...(config.marked_extensions ?? []),
            headingExtension,
            tableExtension,
          ],
          dest: dst,
        },
      );
      await postProcessPdf(dst, title, version);
    }
  } finally {
    await browser.close();
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
