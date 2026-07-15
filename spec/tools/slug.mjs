// Slug function used both by the marked heading renderer (so PDF anchor IDs
// match) and by insert_section_links.mjs (so §-reference link targets match).
//
// Differs from marked's default slugger in one place: periods between digits
// are converted to hyphens rather than stripped, so §3.1.1 maps to a slug
// containing "3-1-1" instead of "311".
export function slugify(s) {
  return s
    .toLowerCase()
    .replace(/`/g, '')
    .replace(/\.(?=\d)/g, '-')   // period before digit -> hyphen
    .replace(/\./g, '')          // other periods (trailing) -> strip
    .replace(/[^\w\s-]/g, '')    // strip remaining punctuation
    .trim()
    .replace(/\s/g, '-');        // each whitespace char -> one hyphen
}
