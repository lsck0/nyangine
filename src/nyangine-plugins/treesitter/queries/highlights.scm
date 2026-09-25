; Highlights for the nyangine .nya object format.
; Capture names are the standard nvim-treesitter set, so the editor's active colorscheme styles them
; with no extra configuration.

; ── Header ───────────────────────────────────────────────────────────────────
; The `nya` magic; the version and checksum after it are numbers, caught by the (number) rule below.
"nya" @keyword

; ── Keys ─────────────────────────────────────────────────────────────────────
(member key: (identifier) @property)

; ── Types ────────────────────────────────────────────────────────────────────
; The type keyword before a value or an array: b8, u32, f32, string, object, array, and the aliased
; any / null / object array headers all land here.
(type) @type.builtin

; ── Literals ───────────────────────────────────────────────────────────────────
(string) @string
(escape_sequence) @string.escape

(number) @number

(boolean) @boolean

(null) @constant.builtin

; ── Comments ───────────────────────────────────────────────────────────────────
(comment) @comment @spell

; ── Punctuation ────────────────────────────────────────────────────────────────
[ "{" "}" "[" "]" ] @punctuation.bracket
[ ":" ";" "," ] @punctuation.delimiter
