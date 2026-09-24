# nyangine

A C2Y engine built on SDL3's GPU API, for games, desktop UI, terminal programs, command line tools
and servers — in the same program, composing.

One nyangine program can mix 2D and 3D rendering, put a UI over it, serve a web interface for its own
metrics, accept messages from another process, talk to OBS over WebSocket, and be driven from a
command line or a terminal UI. With plugins, optional end to end encryption, and no framework tax.

## Three ways to read the documentation

They answer different questions, and none of them is a substitute for another.

| | Answers | Written by |
| :--- | :--- | :--- |
| **These pages** | Why a thing is shaped the way it is, and how to do something end to end | By hand |
| [**Cheatsheet**](CHEATSHEET.md) | What is this function called, what does it take | Generated from the headers |
| [**Doxygen**](doxygen/html/index.html) | What does this actually do | Generated from the source |

Start here when you are new or when you want the reasoning. Use the cheatsheet when you know what
you want and need the signature. Read the doxygen output, or the header itself, when you need the
truth — the headers are the manual, and every public one opens with a block covering what the module
is for, every function in it, a copy-pasteable example, and what was tried and rejected.

`./build docs` assembles all three into one deployable tree under `./site`: this prose, the
cheatsheet, and the doxygen output, cross-linked so they reach one another wherever the tree is served.

## Where everything else lives

- [`AGENTS.md`](https://github.com/lsck0/nyangine/blob/master/AGENTS.md) — the short version, for coding agents.
- [`TODO.md`](https://github.com/lsck0/nyangine/blob/master/TODO.md) — what is left, what was measured, and why things are the way they are.
- `packaging/README.md` — cutting a release per channel.
- `secrets/README.md` — the signing key.

## A warning about these pages

Everything here is hand written, which means it can go stale in a way the cheatsheet and the doxygen
output cannot. Where a page and a header disagree, **the header wins**. If you find a page that lies,
the fix is to correct it in the same commit as whatever made it lie.
