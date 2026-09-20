# data

What the game reads and writes. Two kinds of file live here, and the difference matters.

## Yours to edit

| File          | What it is                                                              |
| ------------- | ----------------------------------------------------------------------- |
| `settings.nya`| Volumes, graphics and key bindings. Edited in game too; both write here. |
| `theme.nya`   | The interface colours.                                                   |

Both are plain text with comments. Edit them with anything. If a value is out of range or a line does
not parse, the game names the file, the line, the key it was reading and what it expected, then carries
on with the default for that one key — a typo costs you that setting, never the file and never the run.
Delete a file to get the defaults back; it is rewritten on the next clean exit.

## Not yours to edit

`saves/` holds save data. It is checksummed on load, so an edited save is refused rather than loaded
half wrong. Copy the directory to back it up; that works and is supported.

## Where this actually lives

In a portable unpacked copy of the game, here, beside the executable. Installed from a package manager
the program directory is read only, so this tree is the set of defaults: they are copied into your own
data directory the first time the game runs, and that copy is the one you edit.

- Linux: `$XDG_DATA_HOME/gnyame`, otherwise `~/.local/share/gnyame`
- Windows: `%APPDATA%\gnyame`
