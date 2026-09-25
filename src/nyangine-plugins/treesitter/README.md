# tree-sitter-nya

A [tree-sitter](https://tree-sitter.github.io/) grammar for the nyangine `.nya` object format —
the text serialization written and read by `src/nyangine/serde/serde_nya.c`. It powers editor syntax
highlighting and can back a formatter/linter (see "Formatting and linting" below).

The grammar mirrors what the engine's own parser accepts, so a `.nya` file the engine reads has no
`ERROR` node here. It was derived from the parser and lexer, not guessed:

- header: `nya <version> <checksum>` (e.g. `nya 2 0`)
- a single root object `{ ... }`, fields `key: <type> value;` (the trailing `;` is optional)
- scalar types `b8 u8 u16 u32 u64 u128 s8 s16 s32 s64 s128 f16 f32 f64 f128 char wchar string wstring void`
- `object { ... }` (the `object` keyword is optional before a brace) and bare `{ ... }`
- `null`, the empty array `[]`, and typed arrays `<element>[] [ ... ]`: `string[]`, `s32[]`,
  `object[]`, `null[]`, the heterogeneous `any[]`, and the nested `array[]`
- numbers: decimal, decimal float with `e`/`E` exponent, hex, hex float with `p`/`P` exponent
  (as the serializer writes floats, bit-exact), and binary; a leading `-` is part of the number
- comments: `// line` and `/* block */`

## Layout

```
grammar.js              the grammar
tree-sitter.json        grammar metadata (scope, file types, query paths)
queries/highlights.scm  highlight captures (standard nvim-treesitter names)
test/corpus/nya.txt     parser tests (tree-sitter test)
src/                    the generated parser — committed, so no CLI is needed to consume it
```

## Building and testing

With the tree-sitter CLI installed:

```sh
cd src/nyangine-plugins/treesitter
tree-sitter generate        # regenerate src/ after editing grammar.js
tree-sitter test            # run test/corpus
tree-sitter build           # compile to nya.so
tree-sitter parse ../../assets/config/engine.nya   # sanity-check a real file
```

This grammar was generated and validated with tree-sitter CLI 0.26.9 and parses every well-formed
`.nya` in the repo (project.nya, assets/config/engine.nya, both theme.nya, settings.nya,
plugins/hello/manifest.nya, and the serde/savegame fuzz corpora) with zero ERROR nodes.

## Neovim

These notes target the config in `~/projects/arch-dotfiles/configs/nvim`, which uses
nvim-treesitter's `main` branch and registers parsers with `vim.treesitter.language.add`.

1. Build the parser and put the library where nvim looks for parsers (`<runtime>/parser/nya.so`):

   ```sh
   cd src/nyangine-plugins/treesitter
   tree-sitter generate && tree-sitter build -o "$HOME/.local/share/nvim/site/parser/nya.so"
   ```

   Or register it from any path in `lua/plugins/treesitter.lua`, beside the existing
   `system_parsers` block:

   ```lua
   local nya_so = vim.fn.expand("~/projects/nyangine/src/nyangine-plugins/treesitter/nya.so")
   if vim.uv.fs_stat(nya_so) then
     vim.treesitter.language.add("nya", { path = nya_so })
   end
   ```

2. Filetype detection for `*.nya`, in `lua/filetype.lua`:

   ```lua
   vim.filetype.add({
     extension = {
       nya = "nya",
       -- ...existing entries...
     },
   })
   ```

   The existing `FileType` autocmd (`lua/autocmds.lua`, "Start treesitter highlighting + indent")
   then calls `vim.treesitter.start` for the `nya` buffer automatically.

3. Highlight queries, so they load on nvim's `after` runtime path like the other languages here:

   ```sh
   mkdir -p ~/projects/arch-dotfiles/configs/nvim/after/queries/nya
   cp src/nyangine-plugins/treesitter/queries/highlights.scm \
      ~/projects/arch-dotfiles/configs/nvim/after/queries/nya/highlights.scm
   ```

   (Symlinking keeps the two in step.)

Open any `.nya` file and `:InspectTree` to confirm the parse, `:Inspect` on a token to see its
capture.

## Formatting and linting

The engine already round-trips `.nya` through its own serde, so the cheapest correct formatter is to
dogfood that rather than write a second parser that could drift. `./build fmt <file.nya>`
deserializes then re-serializes pretty (`NYA_SERDE_PRETTY`), which is the canonical formatting, and
`--check` reports drift or a parse error as the lint without rewriting. See `src/nyangine-build/fmt.c`.
