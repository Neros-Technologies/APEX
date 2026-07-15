// One-shot transformer: walk every *.md at the repo root and rewrite §-style
// section references into Markdown links that target the right heading.
//
// Three patterns, processed in order:
//   1. [text](OtherDoc.md) §X.Y     -> merged into a single link
//      [text §X.Y](OtherDoc.md#slug)
//   2. <DocName> §X.Y                -> cross-doc link (e.g. "Core §3.5")
//      <DocName> [§X.Y](DocFile.md#slug)
//   3. bare §X.Y                     -> same-doc link
//      [§X.Y](#slug)
//
// Idempotent: a re-run won't re-link references that already became Markdown
// links, because each pattern requires the bare prose form to match.

import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { slugify } from './slug.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '..');

// Map a doc's prose-name forms (as they appear in cross-doc references) to
// the doc's filename stem.
const docByProseName = {
  'Core': 'APEX_Core',
  'Device Class — Activation': 'APEX_Device_Class_Activation',
  'Device Class — Analog HMI': 'APEX_Device_Class_Analog_HMI',
  'Device Class — Wayfinding': 'APEX_Device_Class_Wayfinding',
  'Device Classes': 'APEX_Device_Classes',
};

// Build { 'X.Y.Z': 'slug-...' } for one document by parsing its headings.
function buildHeadingMap(content) {
  const map = {};
  const re = /^#{1,6}\s+(.+?)\s*$/gm;
  for (const m of content.matchAll(re)) {
    const text = m[1];
    const numMatch = text.match(/^(\d+(?:\.\d+)*)\.?\s/);
    if (numMatch) map[numMatch[1]] = slugify(text);
  }
  return map;
}

// Inject an explicit HTML anchor above each numbered heading. GitHub's
// auto-slug for "3.1.1" and "3.11" both collapse to "311", so a heading-text
// match is the only thing keeping their slugs distinct on GitHub. The
// explicit anchor sidesteps that — our links always target this slug.
//
// Both `id` and `name` are emitted for sanitizer compatibility (some pipelines
// strip one but allow the other).
//
// Idempotent: skip injection when the line directly above the heading is
// already our anchor for that heading's slug.
function injectHeadingAnchors(content) {
  const lines = content.split('\n');
  const out = [];
  for (const line of lines) {
    const headingMatch = line.match(/^#{1,6}\s+(.+?)\s*$/);
    if (headingMatch && /^\d+(?:\.\d+)*\.?\s/.test(headingMatch[1])) {
      const slug = slugify(headingMatch[1]);
      const expected = `<a id="${slug}" name="${slug}"></a>`;
      if ((out[out.length - 1] ?? '') !== expected) out.push(expected);
    }
    out.push(line);
  }
  return out.join('\n');
}

// Apply `fn` to every span of the input that is OUTSIDE any [text](url)
// markdown link. The link spans pass through verbatim. This is what keeps us
// from re-linking content already inside a link (e.g. the §-ref pattern 1
// just merged into a link target) and producing nested-link garbage.
function mapOutsideLinks(content, fn) {
  const LINK_RE = /\[[^\]]*\]\([^)]*\)/g;
  let out = '';
  let idx = 0;
  for (const m of content.matchAll(LINK_RE)) {
    out += fn(content.slice(idx, m.index));
    out += m[0];
    idx = m.index + m[0].length;
  }
  out += fn(content.slice(idx));
  return out;
}

function transform(content, thisDoc, allMaps) {
  const warnings = [];
  const sectionRe = /\d+(?:\.\d+)*/;

  // Pass 1 (Pattern 1): [text](OtherDoc.md) §X.Y  ->  merged single link.
  // Runs across the full content so it can see both sides of the link boundary.
  // The known-stem alternation is derived from the docs actually present, so
  // renaming a source file never silently breaks this pass.
  const stemAlt = Object.keys(allMaps)
    .map((s) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'))
    .join('|');
  const pass1Re = new RegExp(
    `\\[([^\\]]+)\\]\\((${stemAlt})\\.md\\)\\s+§(\\d+(?:\\.\\d+)*)`,
    'g',
  );
  content = content.replace(
    pass1Re,
    (match, text, otherDoc, num) => {
      const slug = allMaps[otherDoc]?.[num];
      if (!slug) {
        warnings.push(`${thisDoc}: pattern-1, no §${num} heading in ${otherDoc}`);
        return match;
      }
      return `[${text} §${num}](${otherDoc}.md#${slug})`;
    },
  );

  // Pass 2 (Pattern 2): "<DocName> §X.Y" outside existing links.
  content = mapOutsideLinks(content, (chunk) => {
    for (const [prose, otherDoc] of Object.entries(docByProseName)) {
      if (otherDoc === thisDoc) continue; // self-references handled in pass 3
      const re = new RegExp(`\\b${prose}\\s+§(${sectionRe.source})`, 'g');
      chunk = chunk.replace(re, (match, num) => {
        const slug = allMaps[otherDoc]?.[num];
        if (!slug) {
          warnings.push(`${thisDoc}: pattern-2, no §${num} heading in ${otherDoc}`);
          return match;
        }
        return `${prose} [§${num}](${otherDoc}.md#${slug})`;
      });
    }
    return chunk;
  });

  // Pass 3 (Pattern 3): bare §X.Y -> same-doc link, outside existing links.
  // Separate pass so this sees the links pass 2 just added.
  content = mapOutsideLinks(content, (chunk) =>
    chunk.replace(/§(\d+(?:\.\d+)*)/g, (match, num) => {
      const slug = allMaps[thisDoc]?.[num];
      if (!slug) {
        warnings.push(`${thisDoc}: pattern-3, no §${num} in same doc`);
        return match;
      }
      return `[§${num}](#${slug})`;
    }),
  );

  return { content, warnings };
}

async function main() {
  const entries = (await fs.readdir(repo)).filter((f) => f.endsWith('.md')).sort();

  // First pass: load each doc and build its heading map.
  const docs = {};
  for (const file of entries) {
    const stem = file.replace(/\.md$/, '');
    const content = await fs.readFile(path.join(repo, file), 'utf8');
    docs[stem] = { file, content };
  }
  const allMaps = Object.fromEntries(
    Object.entries(docs).map(([stem, { content }]) => [stem, buildHeadingMap(content)]),
  );

  // Second pass: inject heading anchors, then rewrite §-references, then write.
  let totalWarnings = 0;
  for (const [stem, { file, content }] of Object.entries(docs)) {
    const withAnchors = injectHeadingAnchors(content);
    const { content: out, warnings } = transform(withAnchors, stem, allMaps);
    totalWarnings += warnings.length;
    warnings.forEach((w) => console.warn(`  WARN  ${w}`));
    if (out !== content) {
      await fs.writeFile(path.join(repo, file), out);
      console.log(`  ${file}: anchored + linked`);
    } else {
      console.log(`  ${file}: no changes`);
    }
  }

  if (totalWarnings > 0) {
    console.log(`\n${totalWarnings} unresolved reference(s); see warnings above.`);
    process.exit(1);
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
