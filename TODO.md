# nyangine: what is left

`[ ]` todo · `[~]` in progress · `[⏭]` deferred

---

## Where it stands

236 tests pass locally, `check --strict` reports nothing, and debug, release, debug-windows and steam-windows
build. **CI is red, and closing**: both failures Phase 0 named are fixed and green on Linux; the Windows tests had
never compiled and are being fixed one push at a time. The title screen logs one line in twenty seconds, where it
logged 6813.

The plan from here is "Roadmap" below, drawn up 2026-09-22 when the scope became the framework for all software.

Landed since the scope widened: the build system reorganised with `./build dist`, a `secrets/` tree encrypted
with sops, and a generated changelog; an IPC control socket and a WebSocket client, both fuzzed; a crash
reporter with its own window; a cheatsheet generated from the headers and an `AGENTS.md`; collision layers,
AFL++ fuzzing, property tests and a deterministic simulation harness; Steam lobbies, peer to peer and
achievements, and Discord presence and invites behind one facade; scene persistence through reflection; the
shadow lag and the fire flicker fixed with measurements; the UI split into seven files with fixed scale and
eleven new widgets; and one system registry driving the frame for engine and game alike.

In progress now: fluids, the plugin system, and fast-forward with DQN driving the game. The HTTP server has
its own example and speaks `application/nya` as well as JSON. The terminal backend landed, sorts its cells
and can be tested; nothing wraps `gh` with it yet. The web client is the one large thing not started. See
"The stack" and "Requested".

A standing caution, learned the hard way over one long session: several things recorded here as done had
never run. A Lua script whose own comment said it existed to be exercised, a Steam module described as
"tested against a fake" with no test at all, every 3D spatial query, and the whole `nya_window_is_*` family.
A claim in this file is not evidence. The audit under "Engine" is the general form of that question.

---

## Still missing — the highlights

The big open fronts, most-blocking first. Each expands in "Roadmap" below.

- **Web client / game→web (CSR).** `[ ]` The renderer is 100% SDL_GPU with no browser backend — the wall that blocks a rendered game on the web. Needs a WebGL2/WebGPU backend, `platform/web` (clock, CSPRNG, OPFS, fetch, WebSocket, input), and the wasm toolchain. UI-only CSR runs today; a rendered game does not.
- **Layering: a server links the whole engine.** `[~]` `http`/`net` sit on `core`, and `core` is SDL, so a webapp or CLI still compiles and links the full engine. `NYA_NO_SDL` does **not** build http (proven 2026-09-24: http/net/crypto/tls/core/reflection all sit inside `#ifndef NYA_NO_SDL`, and `NYA_App` embeds the renderer by value). Real headless needs `core` split out of the SDL wall. Mitigated for deploy by `--server` (release + `--gc-sections` drops SDL at link), not fixed.
- **Components + profiles.** `[ ]` The whole "everything is a plugin" refactor: one descriptor per component the build reads, and named profiles (`cli`/`tui`/`server`/`desktop`/`game`/`web`). This is what makes the vendor subset and a true headless build real rather than link-time GC.
- **Accounts.** `[~]` The full login flow landed in `examples/accounts_api` (Argon2id hashing over monocypher, user store as a reflected Model, `POST /api/register`/`/login`/`/login/totp`/`/logout`, opaque row-backed `__Host-session` cookie with revocation, TOTP second factor, PGP seam). Round-trip tested over a loopback server (`13e1ddd8`). Open: lift it from the example into a reusable `accounts` component/routes, and passkeys (WebAuthn) as a third factor.
- **TLS in process + ACME.** `[ ]` mbedTLS/OpenSSL wrapped in `http` (1.3 floor 1.2), then ACME so one executable renews its own cert. Until then TLS is a proxy's job (missing HSTS across the scans is downstream of this).
- **CI is red.** `[!]` Every master run since 2026-09-22 fails or is cancelled; Windows tests never compiled, fixed one push at a time. Blocks trusting any "green" claim.
- **Renderer.** `[ ]` Compute passes (raymarched volumes, GPU fluids/particles, SSAO); screen-space + planar reflections. (The realistic-but-stylized showcase scene composing wind+foliage+water+dust+light-shafts landed 2026-09-24, `a8037332`.)
- **Model/SO/DTO web profile.** `[ ]` The conversion split in `http`/`db`, the `web` profile refusing model/SO headers, `@secret` encrypted fields.
- **Smaller open items:** pentest pass over `http_server`; scheduled CI (fuzz/simulation/benchmarks); pinned vendor releases (SDL/Box3D untagged); clang-format gate; job queue; passkeys (WebAuthn); DOM presenter + shadcn-like widget set + node/code editors; structured logging; a parsed-newtype helper; typed route-table client calls; web hot reload.

## Recently landed (2026-09-24, agent batch)

Headless server deploy: a `--server` build profile (release + LTO + `-Wl,--gc-sections` + strip; SDL eliminated at link, 38M→9.4M) with a minimal vendor subset macro, `web_server` decoupled from SDL (`nya_os_time_sleep_ms`, `--address` to bind `0.0.0.0`, a `--healthcheck` self-probe), a scratch Docker image (`docker images` 39.2MB, GET / = 200, HEALTHCHECK healthy) and a homelab swarm `stack.yaml` + README (`7b97eeb3`, `af8c42b4`, `c60dc309`). Caveat: the example still links the full vendor set because it compiles the whole engine graph; the subset is the real link set only once `core` leaves the SDL wall.

Batch 3: water depth-difference shoreline foam + richer wave heightfield (`d5c7c078`), WebSocket net transport + player-key allowlist + version rejection (`20360276`), our own opt-in stereo panner (`bd26b494`), `./build sbom` CycloneDX + licence-allowlist gate + CVE hook (`f6f70433`). Runtime-dependency preflight — crash at startup when a required program/library is missing, `gpg` wired (`681c8066`). Privacy pass: crash report scrubs home/user/host before it leaves the process (`7e0a794d`), server bind default pinned to loopback in one named place (`80f4e0c6`). `.wasm` served from the static bundle as `application/wasm`, so the CSR bundle hosts its own module through the engine (`31556ffa`).

Crash/reliability fixes: a shader that failed to load (missing/corrupt `.spv`) was freed at deinit as if loaded → SIGSEGV, now guarded (`ba28b0c9`); `nya_ceiling_unregister` added and the three HTTP ceilings dropped on deinit, closing a use-after-free where a metrics/HUD query after a server stopped read freed per-server counters (`e343786a`).

Verified the HTTP server (`web_server`) with server-fucker.sh (all 7 phases): no confirmed injection/XXE/deser/SQLi/smuggling/CORS/secret leak, rate-limiting trips 429, no framework/version banner; only note is missing HSTS (TLS-only). SSR (`ui_ssr`) — same scan clean, plus source-verified HTML-escaping (`_nya_ui_html_escape`, field buffer included), strict `w<n>` id validation, length-capped field input, and a strict CSP with a per-response script nonce (`nosniff` + `X-Frame-Options: DENY`, 405 handled). CSR static host is content-addressed (no filesystem path lookup → traversal structurally impossible).

Earlier: shipping hardening (`2c758ee8`, RELRO+BIND_NOW+NX+fortify ELF-verified), UI theme `.nya` files (`14e26f05`), planar water sky-reflection (`03999130`), the `web_frontend` example (`73368828`, seventh example), signed plugins (`f4c21979`, Ed25519 + pinned keys, refuse-unsigned), particles/fluid wind, weather rain/snow, instanced grass, textureGather web variant + depth/MSAA FBOs, reload-safe handlers, supervised restart, docs site, SSR write-back/styles, and the showcase-dark fix.

## Standing decisions

| Area         | Decision                                                                                     |
| :----------- | :------------------------------------------------------------------------------------------- |
| Workflow     | Edit files directly. GitHub is a backup: push to preserve work, not as review or process.    |
| Comments     | Keep the why, cut the essay. One to three lines per prose block.                             |
| Gamepad      | `NYA_InputBinding` is a union of key, gamepad button, or axis past a threshold.              |
| Subsystems   | `core_system.h` registers engine subsystems and game systems alike.                          |
| Config       | `NYA_CONFIG` hot reloads from `assets/config/engine.nya`, backed by reflection.              |
| Ceilings     | Fixed capacity arrays register with `nya_ceiling_register`.                                  |
| Verification | Every feature gets a caller in `gnyame` or one of the seven examples, not only a test, and that caller runs in CI. |
| Priorities   | Security, privacy, stability, performance, then lines of code. Secure by default, never on request. |
| Components   | Everything above `base`, `platform` and `math` is a component, listed in `assets/config/plugins.nya`. |
| Data shapes  | Model (stored), optional SO (inside the program), DTO (on the wire). Only DTOs reach a client.  |
| Servers      | One machine, one instance. TLS and simple limits in process; a proxy is optional.            |
| Programs     | Live in this tree beside gnyame for now.                                                     |
| Composition  | **Every system works with every other one unless that genuinely makes no sense** (set 2026-09-22). A feature belongs to the engine, not to the program that asked for it: permissions are for a guild as much as for a route, the UI runs on all four backends, an asset is an asset whether it is a texture or a stylesheet, and a 3D game can host an HTTP server or be one path of a CLI. When a feature would only work inside one caller, that is a design fault to fix rather than a scope to accept. |

---

# Definition of done

The engine is finished when all of this holds, in the existing style (see the style guide), with lines of code,
file size, RAM, VRAM, CPU, GPU and startup time kept to a minimum, and nothing a player or a peer does can crash
it. gnyame is where the project lives and proves that everything composes in one program; the seven examples
each prove one kind of program alone.

Scope was "a game engine". It is now **the framework for all my future software**, one united stack powerful
enough for enterprise level desktop applications, web applications and games: desktop UI, TUI,
CLI, web servers and web clients, all in the same program and all composing. One nyangine program should be able
to mix 2D and 3D rendering, put a UI over it, serve a web interface for its own metrics, accept messages from
other programs, talk to OBS over WebSocket, and be driven from a CLI or a TUI, with plugins, optional end to end
encryption and TOTP, PGP or passkey second factors. See "The stack" below for what that adds, and "Roadmap" for the order.

The priorities, in order when they conflict: **security, privacy, stability, performance**, then lines of code.
Everything is secure by default, not on request. Everything works together: a feature that only works in
isolation is not done. The codebase is clay: when something does not fit, the architecture changes rather than
the feature being bolted on, and the whole converges on one architecture over time. Encapsulation is per module
(the renderer, the string API), never per object.

| Area             | Wanted                                                                                                                                        | State                                                                                                                                                                                                                                                                                                                                                                |
| :--------------- | :-------------------------------------------------------------------------------------------------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 2D/3D renderer   | animation, particles, atmosphere, liquids, opacity, reflections, dynamic LOD, eye adaptation                                                  | `[~]` animation, particles, fog, glass, terrain and mesh LOD, eye adaptation, light shafts, aerial perspective and motion blur exist; volumetrics, liquids and reflections missing                                                                                                                                                                                   |
| Post processing  | a composable chain                                                                                                                            | `[x]` occlusion, ink, depth of field, FXAA, grade, bloom, speed lines, HDR output                                                                                                                                                                                                                                                                                    |
| Graphics options | antialiasing, motion blur, fov, ... toggleable                                                                                                | `[x]` MSAA, FXAA, shadows, post passes, fov and render scale are player settings, and 37 feature switches cover everything else including culling, sorting and the depth test                                                                                                                                                                                        |
| Renderer debug   | physics hitboxes and other debug views                                                                                                        | `[x]` buffer views, and every collision shape in either solver drawn over the scene, coloured by body type and dimmed while asleep                                                                                                                                                                                                                                                                                                                     |
| Audio            | raytraced: occlusion, diffraction, echoes, room estimation; sound post processing                                                             | `[x]` partial occlusion, transmission, diffraction, room driven reverb, echo taps; per bus chain (filters, EQ, compressor, echo, reverb, limiter). Open: interaural delay and head shadow (needs our own panner instead of SDL_mixer's)                                                                                                                              |
| UI               | immediate layout, styling, animation; widgets incl. colour picker, sliders, buttons, inputs; debug look by default, texture skins for game UI | `[~]` seven files by domain, fixed scale, nine-slice skins, full text editing with selection and clipboard, dropdowns that float, radio, tabs, draggable and resizable windows with title bar chrome, tables, charts, icons, opacity groups, scrolling, and tab/shift-tab focus for a terminal that has no pointer. Open: the shadcn-like widget set (Phase 4), a node editor, SVG, the code editor widget |
| Core             | events, entities, input, settings, cache, ... solid                                                                                           | `[x]` one system registry drives frame, tick and render for engine and game, with runtime enable/disable and per-owner accounting, and its entries are callback handles with copied names, so a system registered from a reloaded image survives the reload. Scenes and settings persist through reflection                                                          |
| Pipelines        | build, assets, reflection                                                                                                                     | `[x]`                                                                                                                                                                                                                                                                                                                                                                |
| Hot reload       | assets, code, configuration                                                                                                                   | `[x]`                                                                                                                                                                                                                                                                                                                                                                |
| Tracing          | time and memory per feature (shadows, antialiasing, particles, ...)                                                                           | `[~]` CPU spans, GPU allocation counters, and per-system and per-owner time and memory from the registry; per renderer feature attribution missing                                                                                                                                                                                                                   |
| CI/CD            | tests and builds with caching                                                                                                                 | `[!]` **red**: every master run since at least 2026-09-22 10:57 fails or is cancelled. See Phase 0 under "Roadmap". `./build dist` stages every target, the changelog is generated, secrets are sops encrypted                                                                                                                                                      |
| Crash reporting  | one funnel, a window a player can act on, everything a triage needs in it                                                                     | `[~]` log ring, composed report (crash, build, machine, stack, log), its own SDL window with close, copy and send, and a file under the log directory. Open: a transport behind `nya_crash_report_submit`, and a window on the fault path (SDL from a signal handler can deadlock)                                                                                   |
| Anti-tamper      | integrity checks like the CRC                                                                                                                 | `[x]` executable stamp, chunked code baseline and a sweep every 250 ms, per blob entry hashes, a watchdog at two inlined sites; failure logs and exits 86                                                                                                                                                                                                            |
| Networking       | attack and cheat resistant, optional end to end public key encryption                                                                         | `[x]` X25519 stateless handshake, XChaCha20-Poly1305 per packet, pinned server keys, rate limits, server authority with a violation score, delta snapshots, fuzzed decoders                                                                                                                                                                                          |
| Web server       | an HTTP server, middleware, typed DTOs, generated OpenAPI                                                                                     | `[~]` `src/nyangine/http/`: router per resource, layer chain, identity extractor, JWT over HMAC-SHA256, OpenAPI and a page generated from the route tables, a metrics resource over the app's own numbers, and a login flow (register/login/TOTP/logout, sealed session cookie) in the `accounts_api` example. Open: typed DTOs and the Model/SO/DTO web profile                                                                                                |
| Targets          | Linux, Windows, Steam Linux, Steam Windows                                                                                                    | `[x]` all four build; Steam Linux against the sniper SDK (glibc 2.31, GnuTLS). A terminal is now a fifth target through `-DNYA_TERMINAL`, verified on Linux only. Web is wanted and not started; Android is out                                                                                                                                                      |
| Layering         | a module DAG; a program links only the modules it uses; SDL only behind platform and renderer backends                                        | `[~]` `base` is clean: it includes neither `math` nor `platform`, and an `os` layer below it holds the syscalls. The rest still has cycles (core↔renderer/ui/net/physics, nn→renderer, renderer→debug, http→core), each a counted allowance in the lint rule that can only fall. `net` and `http` sit on `core`, which is SDL, so a CLI tool or a server links the whole engine. `NYA_NO_SDL` stands in for "no core" inside `base`                                                                                                       |
| Base             | preprocessor passes, reflection, introspection, errors, stack traces, memory debugging, platform info, integrity, custom static analysis      | `[x]` all present; `check --strict` runs the project's own lint rules (`src/build/lint.c`) before clang-tidy: banned calls, module order, verb pairs, callers, `.clangd` drift                                                                                                                                                                                         |
| Standard library | typesafe containers, strings, math, dynamic objects, a safe file, an ORM, crypto, time, a binary wire form                                   | `[~]` containers, strings, math, `NYA_Object`, reflection-driven ORM (as a plugin). Missing: dates and times as a type rather than a timestamp, URLs                                                                                                                                                  |
| Auth             | login, JWT in secure cookies, CSRF defence, revocation, rate limits, TOTP and PGP second factors                                              | `[~]` Landed in `accounts_api`: Argon2id password hashing, a user store, register/login/logout routes, an opaque row-backed session cookie (`__Host-`, HttpOnly/Secure/SameSite=Strict) with revocation, TOTP and a PGP challenge, JWT over HMAC-SHA256, `SameSite`+Origin CSRF, in-process rate limits. Open: lift from the example into reusable routes; passkeys (WebAuthn)                                                                                     |
| Web client       | C compiled to wasm, the same `nya_ui_*` calls, the same DTO headers as the server, transport in the `nya` format                            | `[ ]` not started                                                                                                                                                                                                                                                                                                                                                      |
| Customization    | plugins, editable config, editable UI style files                                                                                             | `[~]` Lua plugins with compile time permissions, hot reloaded `engine.nya`. No UI style files, no signed plugins, no plugin repositories, no VM budgets                                                                                                                                                                                                                  |
| Examples         | hello world, CLI app, TUI app, multiplayer 2D game, 3D game, HTTP server, web frontend                                                        | `[~]` six of seven exist under other names; the web frontend needs the web target. See "Roadmap"                                                                                                                                                                                                                                                                      |

---

# Roadmap

Drawn up 2026-09-22 against the framework goal above. Phases are ordered by what unlocks what, not by size.
Two of them are architecture and the rest are features, since features built on the current layering would
have to be moved again. Within a phase, items can land in any order. Every item says how we know it is done.

## Target architecture

One direction for every include, and the lower a module sits the fewer programs it assumes:

```
os        pages, clocks, file handles, processes, CSPRNG; later threads and sockets  (one file per target)
base      containers, strings, arenas, errors, logging, reflection, ceilings, hashing,
          the file system, commands and clocks over `os`                                  (no SDL)
platform  IPC, terminal, signals, host probes                                          (per OS, and web)
math      scalars, vectors, matrices, noise, random                                     (beside base)
serde     text and binary `.nya`, json, jsonc, the reflection bridge
crypto    monocypher behind one API, plus SHA-1/SHA-256/HMAC, base32/64, constant time compare
db        SQLCipher and the reflection ORM, with migrations and backups
permissions  roles, overwrites, resolution                 jobs   persistent queue, schedules
net       transports (UDP, loopback, WebSocket, Steam) and the encrypted session           (no entities)
tls       mbedTLS behind one API
http      server, client, routing, layers, cookies, WebSocket, OpenAPI, request logging
accounts  users, sessions, second factors (TOTP; PGP and passkeys as components)
app       the loop, systems, events, input, config, assets, windows                    (SDL from here up)
renderer  2D, 3D, terminal, web backends          audio   mixer, propagation, effects
ui        widget model, then presenters: shapes (GPU, terminal) and DOM (web — `ui_present_html`, landed 2026-09-23)
desktop_shell  native dialogs, tray, notifications
physics   Box2D and Box3D                          replicate  snapshots, prediction, lag compensation
debug     overlay, trace, crash window             testing    property, simulation, sessions, agents
```

Rules this sets:

- A module includes only modules above it in that list. `./build check` enforces the list, so a cycle is a
  lint failure rather than a discovery.
- SDL is named only inside `platform` backends, `app`, `renderer` and `audio`. A CLI tool, a TUI and a server
  link no SDL at all, which retires `NYA_NO_SDL` as a concept: a module is present or it is not.
- The web is a platform like Linux and Windows, not a special build of everything: `platform/web`, a renderer
  backend and a UI presenter. Nothing above them learns that it runs in a browser.
- One vocabulary type per concept across every module: one string, one array, one error, one dynamic value, one
  handle shape.
- Data that is stored, passed through the program, or sent to a client has up to three shapes: Model, SO and
  DTO. See the next section. A struct never plays two of those roles.

## Model, SO, DTO

Three shapes of one piece of data, each owned by the boundary it serves. Set 2026-09-22.

| Shape     | What it is                                      | Lives in                     | Crosses                    |
| :-------- | :---------------------------------------------- | :--------------------------- | :------------------------- |
| **Model** | what is stored: the database row or file record | `<thing>_model.h`, server only | the `db` boundary          |
| **SO**    | system object: how it moves through the program | `<thing>_so.h`, server only    | nothing; internal          |
| **DTO**   | what is sent to or received from a client       | `<thing>_dto.h`, shared        | the wire, both directions  |

Take a user. The Model carries `password_hash`, `totp_secret`, `created_at` and the row id. The SO carries
parsed types (`Email`, `UserId`), the resolved permissions and whatever a request joined in. The DTO carries
the name and the email the client may see, and nothing else. A new column added to the Model therefore never
reaches a client by accident: a field reaches the wire only when a DTO names it.

- **The SO is optional.** When a resource is simple enough that the Model is what the program works with,
  there is no SO and the conversions go Model ↔ DTO. Add an SO when the two would otherwise differ: parsed
  newtypes, joined data, derived fields, or state that is never stored.
- **Conversions are the only conversions.** Each is a total function beside the types, named by the style's
  `from`/`to` vocabulary: `user_so_from_model`, `user_model_from_so`, `user_dto_from_so`, and
  `user_so_from_dto`, which is fallible, because a DTO from a client is untrusted input and turning it into an
  SO is where it is parsed. Nothing converts a DTO straight into a Model. The style guide calls conversions
  between in-house types a smell; these three boundaries are the exception it allows, and nothing else gets
  one.
- **Only DTO headers reach the web client.** The `web` profile compiles `*_dto.h` and never `*_model.h` or
  `*_so.h`, and `./build check` refuses an include that breaks this. The server's storage layout, and any
  secret in it, cannot be compiled into wasm even by mistake. This is what "types shared between client and
  server" means here: the DTOs are shared, nothing else.
- **Each shape has its own reflection** and its own consumer: Models drive the ORM and the migrations, DTOs
  drive serde on the wire, OpenAPI and the typed client calls. A `@secret` field on a Model is refused as a
  DTO field by name and type, as a second line of defence behind the rule above.
- **DTOs are versioned.** A breaking change to a DTO is a new DTO and a new route, never an edit to one a
  deployed client still sends. The layout hash in binary `.nya` refuses a mismatch instead of misreading it.
- **Files are grouped by domain:** `users/user_model.h`, `users/user_so.h`, `users/user_dto.h`,
  `users/user_router.c`. One resource, one directory.
- **Not only HTTP.** The same split applies to multiplayer snapshots (DTO on the wire, entity state as the SO)
  and to save files, where the Model is the save record.

## Components: everything is a plugin

The central shape of the framework, set 2026-09-22: **every module above `base`, `platform` and `math` is a
component that can be added or removed without editing anything else.** The renderer, audio, physics, `net`,
`http`, `db`, the UI presenters, Steam, Discord, Lua, PGP, TLS: each is a component. A program is a list of
components, and the examples and gnyame differ only in their lists.

Today adding one costs edits in five places, which is the drift this file keeps recording: an `#ifdef` block in
`plugins.h` and another in `plugins.c`, a `-D` in `flags.h`, the same `-D` by hand in `.clangd`, and the vendor
lists in the project rules. Modules that are not "plugins" are worse: `nyangine.h` includes them unconditionally.

What a component is:

- A directory with one descriptor the build reads: its name, the components it depends on, its vendors, its
  flags, its sources and its tests. It is the only file that states any of these. `plugins/curl/` and
  `renderer/` look the same to the build.
- The build resolves the list: it pulls in dependencies, refuses a cycle or a missing dependency with the chain
  that caused it, and generates everything that repeats the list: the umbrella `nyangine.h`/`nyangine.c`, the
  `-DNYA_WITH_<NAME>` flags, `.clangd`, the vendor set, the cheatsheet's grouping, the Lua binding set. Nothing
  in that list is hand maintained, so none of it can drift.
- At run time a component brings itself up through what already exists: its systems in the registry (with
  itself as owner, so time and memory are per component for free), its ceilings, its typed config struct from
  Phase 1, and its assets. The host names no component.
- Cross-component calls go through a facade that stays compiled when the component is absent and answers
  `NYA_ERROR_NOT_SUPPORTED`. That is how `steam.h` works today and how `nya_http_second_factor_set` waits for
  PGP. A caller never writes an `#ifdef` for another component.
- Removing a component means deleting its line from the program's list. The build then refuses anything that
  still names it directly, so a hidden dependency shows up as an error.

The list lives in `assets/config/plugins.nya`, beside `engine.nya`, and it is the only place a program says
what it is made of. The build tool already links `serde`, so it reads the file directly. Two sections, because
they are decided at different times and by different people:

```
nya 2 <checksum>
{
    components: string[] ["renderer", "audio", "ui", "physics", "net", "http", "db", "tls", "lua"];
    plugins: object {
        hello: object { enabled: b8 true; };
    };
}
```

- `components` is read by `./build`. Editing it rebuilds with that set, and it is baked into the binary so the
  program, its crash report and `nya_build_info` can say what it contains. A shipped binary cannot change it;
  a user editing it changes nothing but the report. An example has its own `plugins.nya` beside its `main.c`.
- `plugins` is the run-time half: which user plugins are enabled, and later the repositories they come from.
  It is hot reloaded like `engine.nya`, validated the same way (the file, the key, what was found, what was
  expected), and a user copy under `data/` overrides the shipped one. This is the file the in-app plugin list
  writes, so the UI and the file cannot disagree.
- A component's own settings do not go here. They stay in `engine.nya` under the component's name, since that
  is where hot reload and the typed config struct already are.

Two tiers of plugin, and they stay separate:

| Tier                | Written in | Chosen          | Who adds it   | Trust                                         |
| :------------------ | :--------- | :-------------- | :------------ | :-------------------------------------------- |
| Component           | C          | at build time   | the developer | full, reviewed, part of the binary            |
| User plugin         | Lua        | at run time     | the user      | none: sandboxed, permission profile, signed   |

Native plugins loaded at run time (a `.so` from a user) are out. Every guarantee about user plugins rests on
running inside a VM, and loading native code would give that up. Hot reloading the game's own library is a
development tool and stays one.

## Examples

Seven, one per kind of program, each the smallest honest version of its kind. They are the answer to "how do I
start a new project on nyangine", so each builds from `./build run example <name>` and runs in CI.

| Example        | Proves                                                  | Today                                                        |
| :------------- | :------------------------------------------------------ | :----------------------------------------------------------- |
| `hello_world`  | a window and one draw                                   | `[x]`                                                         |
| `cli_app`      | args, files, serde, no window, no SDL linked            | `cli_tool`; `[ ]` still links the whole engine               |
| `tui_app`      | the terminal backend and `nya_ui_*` in cells            | `tui_dashboard`                                              |
| `multiplayer_2d` | authority, prediction, encryption, lobby             | `pong_multiplayer`; `net_echo` folds into it                  |
| `game_3d`      | 3D rendering, physics, audio propagation, post chain    | `pinball3d`                                                  |
| `http_server`  | TLS, routes, DTOs, OpenAPI, accounts, roles, db, jobs, uploads, WebSocket | `web_server`; `[ ]` all but routes and OpenAPI missing |
| `web_frontend` | the same UI toolkit in a browser, against `http_server`, as wasm (CSR) and server rendered (SSR) | `[x]` `examples/web_frontend` (`73368828`): a nya_ui todo app served SSR against an http_server db store; CSR build path via `./build wasm-ui` documented |

`plugin_scripting` stops being an example of its own: plugins are a feature of a program, so the 2D game or the
TUI loads one. `net_echo` and `plugin_scripting` are deleted once their callers have moved.

### One component, every surface (2026-09-23)

The `nya_ui_*` component renders across TUI (`ui_present_cell`), native GPU (`ui_present_shape`) and now the
browser, from the same `component()` function.

- `[x]` **SSR render** — `ui_present_html.{h,c}`: one absolutely-positioned element per widget, stable id
  (draw order), per-kind class, `data-nya` event, HTML-escaped labels, id→rect and id→kind/value-rect tables,
  `nya_ui_html_document(title, nonce)` wrapping page + stylesheet + thin client. Custom `NYA_UIStyle` colours and
  radius emitted as inline CSS. Tests: `test_ui_present_html`, `test_ui_interaction`.
- `[x]` **SSR live loop** — `ui_ssr` example: `GET /` renders, `POST /event` turns `{id,event}` into a synthetic
  pointer over the widget's rect (clicks) or a drag along its track (slider value events), runs the input pass,
  re-renders. Client-side DOM morphing by id (server is stateless), preserving focus/caret. Per-session state in
  a sealed cookie (two browsers, two counters). Page CSP: nonce'd script + `style-src 'unsafe-inline'`.
- `[x]` **CSR real engine in wasm** — `./build wasm` compiles the engine's `base` + `serde` (leaves only, not
  `base.c`/`os.c` wholesale) under emscripten and runs it: arena → `NYA_Object` → `nya_serialize(JSON)`, node-
  verified as genuine (dict order, not the fallback writer's). The port: `f16` gated to `float` on wasm,
  `<immintrin.h>` guarded off, new `os/os_wasm.c` (page/time/random over calloc/clock_gettime/getentropy),
  `serde_nya_binary.c` excluded (x87 f128 wire format is unshimmable on wasm's IEEE-quad long double). Native
  build byte-identical (every change guarded).
- `[x]` **CSR UI in wasm — the whole component, no server** — `./build wasm-ui` compiles the `ui` module +
  `ui_present_html` + input + their base/math leaves under emscripten (`src/web/wasm_ui.c`), `web/ui.html`
  mounts it. Exports `nyangine_ui_render()` (one settled draw pass to DOM body) and `nyangine_ui_event(id,event)`
  (parses `wN`, looks the widget rect up via `nya_ui_html_rect`, injects a synthetic pointer, re-renders); state
  lives in wasm globals. node-verified: click +1 → `count` 1→2, tab switch, toggle flips theme. `NYA_App` embeds
  the renderer/asset/physics systems by value so it can't be defined without the vendored headers — cleared by
  including `nyangine.h` whole (headers only, nothing from those libs compiled/linked) and compiling only the
  leaf set; `core_app.c`/`core_window.c` replaced by a small windowless wasm surface in `wasm_ui.c`. Engine edits
  all `#if OS_WASM`-gated (two GPU vertex `static_assert`s off where f16→float changes the size; SDL mutex/event
  pump and IME/clipboard no-op'd), native byte-identical. Gap: value widgets (slider drag) not wired — click
  only; text widths are the monospace-cell approximation the browser re-measures.
- `[ ]` **Game → web (CSR, the goal):** the rest of the engine in wasm — `app` loop under
  `emscripten_set_main_loop`, `ui` + a DOM or canvas presenter, and for the actual game a WebGL/GLES renderer
  backend against SDL3's emscripten port. Then a `web_frontend` caller and a game example that builds to a
  canvas. The base+serde port above is the foundation.
  - **WALL found 2026-09-23:** the renderer is 100% SDL_GPU (216 call sites; `SDL_CreateGPUDevice` with
    DXIL/MSL/SPIRV shader formats). SDL_GPU has no browser backend — no WebGL/WebGPU target in stable SDL3, so
    neither the 3D renderer nor the 2D GPU shape presenter runs in wasm as written. The DOM/UI path (SSR done,
    CSR in flight) is unaffected because it never touches SDL_GPU. To put a *rendered game* on the web needs one
    of: (a) a new WebGL2/WebGPU render backend behind the same `renderer.h` seam + SPIRV→GLSL/WGSL shader
    cross-compilation (largest); (b) a lightweight canvas-2D backend that consumes render2d's draw list and
    bypasses SDL_GPU (2D games only); or (c) wait for an upstream SDL_GPU WebGPU backend.
  - **Decision 2026-09-23 (user): the full render backend (a), targeting WebGL2/GLES3, not WebGPU.** Rationale:
    emscripten's GLES3/WebGL2 is the mature browser path, and SPIRV-Cross (already inside vendored
    `sdl-shadercross`) cross-compiles our SPIRV to GLSL ES 300 with no new toolchain — WebGPU would need Dawn +
    naga/tint and has spottier browser support. SDL_GPU has no GL backend to reuse, and the engine has no render
    seam (SDL_GPU types sit in the public `NYA_RenderSystem`/`NYA_RenderTexture` structs). So the plan is a
    **SDL_GPU-API-shaped shim over GLES3** compiled only for wasm, leaving all 216 engine call sites and structs
    unchanged. The surface is bounded: **53 distinct `SDL_*GPU*` functions** (inventory in git history of this
    line's commit). Staged:
    1. `[x]` **Shader pipeline (landed `c5725c4`)** — the build emits GLSL ES 300 beside every `.spv` via the
       vendored SPIRV-Cross C API (`_nya_asset_shader_compile_glsl_es` in `src/build/pp/asset.c`, linked into the
       build tool like libbacktrace; naming `<shader>.<stage>.glsl`; NOT indexed/bundled/loaded yet — the loader
       is untouched). **37/37 convert and validate as ESSL 300 under glslangValidator.** Findings the GLES3
       backend MUST handle:
       - **textureGather blocks lit 3D (4 shaders refused):** `mesh3d.frag`, `mesh3d_textured.frag`,
         `mesh3d_decal.frag`, `mesh3d_glass.frag` share `mesh3d_shading.hlsli:272` `map.GatherRed(...)` (4-tap PCF
         shadow) → `textureGather`, which is ESSL 310+; WebGL2 caps at ESSL 300. Fix = a web shader variant that
         does 4 explicit `texture()`/`textureLod()` taps at half-texel offsets, gated behind a `-D` at the
         HLSL→SPIRV step (so native keeps the faster gather). The shadow *pass* shaders don't include the header
         and convert fine — only the lit pass is blocked. 2D path is unaffected.
       - **Two uniform-upload paths:** 32 shaders emit real `layout(std140) uniform` blocks, but 5
         (`effect_light_shafts/lut/occlusion/occlusion_apply/output_hdr`) fall back to plain `uniform` (their
         cbuffer packing isn't std140-expressible in ES300, which has no per-member offset). The backend needs
         both: `glUniformBlockBinding`+UBO for most, `glUniform*`-by-name for those 5 — or re-lay-out those
         cbuffers to std140 (pad to vec4) so all become UBOs.
       - **No `layout(binding=)` and synthesized sampler names** (`_29`, `_78`): the backend assigns texture units
         (`glUniform1i`) and UBO binding points itself by declared/reflected order, not by name.
       - `effect_lut.frag` needs a `sampler3D` (fine on WebGL2). Dummy samplers synthesized for texelFetch passes.
    2. `[~]` **GLES3 shim (2D landed `c4d1390`)** — `src/nyangine/renderer/gpu_gles/gpu_gles.{c,h}`, `#if OS_WASM`.
       Defines the opaque SDL_GPU handle structs as GLES state (legal — no SDL linked on wasm) and implements the
       ~35 SDL_GPU functions the 2D path uses over WebGL2 (context via `emscripten_webgl_create_context`, command
       buffers execute immediately, render pass = fbo0 + clear, pipeline = linked GLSL-ES program with bindings
       assigned by reflection, push-uniforms = UBO + `glBindBufferBase`, draw = `glDrawElements`). `./build
       wasm-game` builds `web/nyangine_game.{js,wasm}` + `web/game.html`; node selfcheck asserts the exact 16-call
       2D frame sequence (PASS). Engine `.c` and `SDL_gpu.h` byte-identical. Only a browser can confirm pixels.
       **Now routed through the real `nya_render2d_*` API (landed `f826b80`).** Both blockers cleared: (a)
       `math_matrix.c` compiles on wasm via `NYA_F16_IS_F32` (`c553c98`); (b) the 2D-only bring-up seam lives
       entirely in `src/web/wasm_game.c` (a twin of `wasm_ui.c`'s app/window backend) — `game_bringup()` stands up
       the shim device, allocates the 2D batch buffers, builds the shape+textured pipelines from the GLSL-ES
       shaders, and a 5-function wasm asset backend answers `nya_asset_get`/`_graphics_pipeline`/`_is_missing`/
       `_missing_report` + `_nya_render_sampler_for` — so `render2d.c` and `renderer.c` are compiled UNCHANGED
       (native byte-identical, zero `#if OS_WASM` in either). `draw_frame` calls `nya_render2d_texture_ex` +
       `nya_render2d_flush`; node selfcheck asserts the exact 23-call real-render2d frame sequence + 1 draw/4
       verts/6 indices (PASS). Leaves pulled under emcc: render2d, render_sort, render_camera, math_matrix,
       math_shapes, render_glyph_atlas (dead-code-dropped text/TTF/cache). Only a browser confirms pixels.
       `[x]` **textureGather web variant landed (`f4d22bf`)**: the 4 lit mesh3d frags now emit valid ESSL 300
       via a `#ifdef NYA_WEB_SHADER` 4-tap PCF path + a `-DNYA_WEB_SHADER` web SPIR-V the build cross-compiles;
       native byte-identical. So the full 3D lit shader path cross-compiles to WebGL2 now. Remaining for 3D-on-web:
       depth FBOs + MSAA resolve in the gpu_gles shim, the 5 plain-uniform shaders, asset loading (SDL_image/ttf)
       in wasm, and canvas input.
    3. App loop under `emscripten_set_main_loop`; canvas input via emscripten html5 events → `NYA_Event`.
    4. A game example building to a canvas; the `web_frontend` caller. Verify a frame draws under node/headless
       where possible, then in-browser.
    Native builds stay byte-identical (every shim file gated `#if OS_WASM`).
    - **Feasibility validated 2026-09-23 (no code yet):** SPIRV-Cross is already vendored inside
      `sdl-shadercross` (`external/SPIRV-Cross`, `libspirv-cross-c-shared.so`, C API in `spirv_cross_c.h`) with
      `SPVC_BACKEND_GLSL` + `SPVC_COMPILER_OPTION_GLSL_VERSION`(300)/`_GLSL_ES`(true)/highp precision — so the
      build tool links that lib and converts each `.spv` directly (shadercross itself exposes no GLSL output).
      `emcc` 6.0.9 is installed (WebGL2/GLES3). The engine uses **no** compute pipeline or compute pass (the 53
      SDL_GPU calls are all graphics), which is exactly what WebGL2 can host; push-constant uniforms map to a UBO
      and the texture+sampler bindings map to combined `sampler2D`, both native SPIRV-Cross GLSL-ES behaviour.
- `[x]` **Remaining SSR (landed `4b3528a`)** — text-field value write-back (the client posts `{id,event:text,value}`,
    `ui_ssr` injects it into the field via focus + ctrl-A + `nya_input_text`, per-session in the sealed cookie,
    XSS-escaped) and custom style beyond colours (panel border from `ink`, scrim, and `--nya-track`/`--nya-accent`/
    `--nya-radius`/`--nya-pad` custom properties reach the DOM). Font sizing stays the monospace-cell approximation.

## Phase 0 — ground truth

Small, and first, because every later phase trusts these numbers.

- `[!]` **CI is red.** Every master run on 2026-09-22 failed or was cancelled by the next push. Run 35735287476
  (`c93f4da`) fails two ways:
  - `vendor-windows`: bootstrapping the build tool fails to link, `ld.lld: error: undefined symbol:
    BCryptGenRandom`. The bootstrap line lacks `-lbcrypt`, which something the build tool now compiles from
    `platform` needs. Everything on Windows after it is skipped.
  - `test-linux`: `test_asset_missing` exits 1 on LeakSanitizer, 66825 bytes in 2051 allocations, all from
    `ALSA_OpenDevice` through `OpenPhysicalAudioDevice`. The test opens a real audio device on a runner. Either
    it should not open audio at all, or the leak is ALSA's and wants a justified entry in `.sanitizers/`.
  Done when a push goes green on both platforms. Pushing one doc commit per minute cancels the previous run
  before its vendor cache is saved, which is how a red run hid behind "cancelled" all day; batch pushes until
  CI is green again.
  - `[x]` Both fixed (run 35742147094: `vendor-windows`, `test-linux` and every Linux job green). The build tool
    links `-lbcrypt` on Windows through `PLATFORM_LINK_WINDOWS`; `#pragma comment(lib)` was tried and clang
    ignores it for mingw. Every test build defaults to SDL's dummy audio driver, so the 34 hand written hints
    are gone and a new test cannot forget one.
  - `[~]` With the bootstrap fixed, `test-windows` got far enough to show that the tests had never compiled on
    Windows: `test_server.c` called `setenv`, and `test_render2d_merge.c` defined `TEXT` over `windows.h`'s.
    Both fixed, and every test now passes `-fsyntax-only` for `x86_64-w64-mingw32`. Running there then found
    two more, both real: `test_control`'s liar case passed vacuously on Linux (it checked for a drop before the
    server had accepted), which a Windows pipe's missing backlog exposed as `ERROR_PIPE_BUSY`; and the Windows
    log was opened `FILE_SHARE_READ` only, so a second process could not log to the same day's file.
- `[x]` `net/test_transport` leaked 81 bytes from `nya_net_transport_connect` once under a loaded parallel run.
  The cause was SDL_net's `NET_Quit` dropping addresses still queued for its resolver, and the fix at the time
  was a wait for this transport's own lookup before letting go. Closed 2026-09-23 by the sockets moving into
  `os`: the lookup is the engine's own thread now, the wait it needed is gone with it, and there is no queue
  inside somebody else's library for an address to be lost on.
- `[x]` `test_agent` gets a wall clock deadline of its own and fails with a message when it passes it.
  `testing_deadline.h`: a watchdog thread that, past the limit, names the test and sends the stuck thread a
  signal, so the crash path prints *its* backtrace. `test_deadline.c` forces a hang in a child: it fails at 1 s
  with the spinning function in its report. The hang itself was two bugs, not a slow run; see "An assertion
  nobody could dismiss" under Findings. 120 regression runs under six way parallel load: 0 failures, where
  about one in thirty hung before.
- `[x]` Custom static analysis in `./build check`, built on `base_lexer` so it costs no dependency.
  `src/build/lint.c`, run before clang-tidy by every whole `./build check` and fatal under `--strict`; it reads
  748 files in about 0.6 s. Six rules: banned calls outside `platform/`; the module order from "Target
  architecture"; verb pairs in the same header; a caller for every `NYA_API`; `.clangd` against every `-D` and
  include the build uses; and a file the lexer misread, since every other rule trusts the tokens. Each fired on a
  deliberate violation before landing. Where the tree has debt the rule allows it by name with a reason in
  `src/build/lint_allowances.h`, and an allowance that stops being needed is itself a finding, so the list only
  shrinks: 9 layering edges (Phase 1's), 82 verb pairs and 81 uncalled functions today.
  - The first run found real drift at once: five vendor defines missing from `.clangd`.
  - Two bugs in the rule itself were caught by spot checking, like the audit's before it. A C23 digit separator
    (`0x8000'0000U`) opened a character literal in the lexer and hid all of `host.c`, so the lexer learned C's
    literals behind a flag. And a walk callback that returns false ends the whole walk, not one directory, which
    had quietly cut the `tests/` tree short at the fuzz corpus.
  - The verb rule judges public names only and skips `_is_` predicates. Most of its allowances are the
    vocabulary's gaps rather than bugs: containers insert with `set` and pair it with `remove`, GPU resources
    are created and released, and arena owned objects have no destroy. Deciding those words is its own item.
- `[x]` The uncalled functions: a caller, a test or deletion, decided per cluster. The rule above counts them
  as identifiers, where the old audit matched text and counted a doc comment or a string naming a function as
  a call, so it finds 81 where the audit's count had come down to 36 (the window, Steam, fluid and renderer
  clusters are most of the difference). `_LINT_CALLERS_ALLOWED` is empty now: every entry got a test, a real
  caller, or was deleted, cluster by cluster — window, Steam, testing, asset blob, input, base, core, then the
  last five (nn's step budget and the DQN's acting network, a tensor copy, Lua's nil and the JSON responder).
  Two deletions (`nya_simulation_pick`, a duplicate of `nya_simulation_below`, and `nya_string_println`) and one
  bug found (`nya_i18n_load_bytes` kept watching the previous locale's file). Earlier: steam, entity queries,
  window state, rng, audio, nn and cursor got tests, and the window cluster found a real bug
  (`nya_window_is_visible` answered true for a handle that is not a window). No library surface needed a
  deliberate no-caller entry; nothing wanted one that the tests or gnyame didn't already want more.
- `[~]` The verb vocabulary's gaps the verb rule turned up: `set`/`remove` on every container, `create`/`release`
  on GPU resources, arena owned objects with no `destroy`, and brackets like `nya_trace_frame_end` whose other
  half is implicit. Decided in AGENTS.md's naming conventions. Containers pair with `add`, not `set` or
  `insert`, matching `nya_array_add`/`nya_array_remove`: `nya_dict_add`, `nya_hmap_add`, `nya_hset_add`,
  `nya_cache_add`, `nya_object_add` and `nya_host_environment_add` renamed, which empties that half of
  `_LINT_VERB_PAIRS_ALLOWED`. GPU resources and arena owned objects are decided too — `create`/`release` for
  what the arena does not own, no destroy at all for what it does — but stay allowances rather than a rule
  change: `create` and `release` are each already half of a different pair (`create`/`destroy`,
  `acquire`/`release`), so the checker asks for both halves independently and a correctly paired
  `create`/`release` function still needs an entry. The frame and tick markers are decided as not brackets at
  all, for the same reason renaming them would not clear the check. Left: whether the checker should accept
  any one satisfied pair instead of every pair a word belongs to, which is a rule change, not a naming one.
- `[x]` `src/nyangine/editor/` is two empty files and no editor is planned. Deleted.
- `[x]` `assets/shader/compiled/mesh3d_outline.vert.*` has no source under `assets/shader/source/`. It is left
  over from the inverted hull that screen space ink replaced. The shader rule now deletes compiled outputs whose
  source is gone, before it compiles, since everything in that directory is baked into the release blob. It
  found `mesh3d_outline.frag.*` orphaned as well: six files in all.

## Phase 1 — layering

The refactor the rest stands on. Behaviour does not change; the include graph and the link lines do.

- `[~]` `base` stops including `math` and `platform`. Both halves are done. Math: `nya_min` and `nya_max` moved
  down into `base_compare.h` and the vector and matrix array derivations up into math. Platform: an `os` layer
  below `base` (see "Where base gets pages, time and files" under Decisions) now holds the syscalls, and the
  file system, commands and clocks came down into `base` on top of it, so the `base -> platform` allowance is
  deleted rather than lowered. The ceiling registry moved too: all of it, since every function in it is a
  registry over a name, a bound and a borrowed counter, and `base_arena.c`, `base_cache.c` and `base_logging.c`
  were already reaching up for it. It is `base/base_ceiling.*` now, the `NYA_NO_SDL` guards around those calls
  are gone because there is nothing left to guard, and the build tool keeps its own ceilings as a result. The
  `http -> core` allowance fell from 2 to 1 with it.
- `[x]` Threads and sockets move into `os`, one file per target, with whatever wants an arena or an `NYA_Error`
  on top of them in `base` — the shape the file system, commands and clocks already have. **SDL_net has left the
  vendor list** as of 2026-09-23, which is one socket layer for the web backend to implement rather than SDL's.
  - Sockets: `os/os_socket.h` is a datagram socket, a listener, the connections it accepts, a wait over a set of
    them and an address that is a value rather than a refcounted pointer. Everything is non-blocking always, and
    a send takes what the host will take — so `http` grew the write queue that used to be SDL_net's, bounded by
    the same `NYA_HTTP_MAX_PENDING_WRITE_BYTES` it always claimed to enforce, and the websocket server flushes
    its protocol's own queue the same way. `base/base_socket.h` is the one thing that needs an arena and a
    thread: a name lookup that does not stop a frame. `net_port.c`, which existed only because SDL_net would not
    report a bound port, is six lines over `nya_os_socket_address` now, and the `SDL_CleanupTLS` the threaded
    server needed — SDL's per-thread error buffer, for our threads that talked to SDL_net — is deleted.
  - Still open, and deliberately: the readiness API is `poll`/`WSAPoll` rather than epoll or IOCP. The interface
    that would hide the difference is `nya_os_socket_wait`, and it is already in place, so that is a change
    inside two files on the day a server needs more than a hundred connections.
  - Threads, earlier: `os/os_thread.h` is a thread, a mutex and a counting semaphore in storage the caller
    places, and it is a semaphore rather than a condition variable because both callers wait on a count rather
    than on a predicate. `base_thread.h` adds what that layer may not have — the record in an arena, the errors,
    the assertions, and the flag a thread raises as it returns, which is the question neither host will answer
    about a thread and which `SDL_GetThreadState` used
    to answer. `core_job.c` and `http_server.c` run on it.
- `[x]` `net` split, 2026-09-23: the transport and the encrypted session need no entity and stayed `net`;
  snapshots, commands, prediction, the client and the server became `replicate` above the app loop. The
  `net -> core` allowance is deleted rather than documented, which is what the split was for.
- `[x]` `http_metrics` moved out of `http` to beside `debug`, and `nn`'s drawing out of `nn` the same way, both
  2026-09-23: one depended on the app loop from inside `http`, the other made a pure library reach for a
  renderer.
- `[x]` The engine owns its configuration, 2026-09-23: `nya_config_engine()` returns a pointer into
  `NYA_App.config_system`, loaded once by `nya_system_config_init` — ahead of "world", and so ahead of
  physics2d/physics3d, which now take `gravity` and `sub_steps` from it at `init` — and kept live the
  same way every other `nya_config_watch` is. `GNY_Config` in `gnyame/config.h` shrank to `GNY_ConfigGame`,
  the game's own half of `engine.nya`; `gny_config_attach` still exists but now only re-watches that,
  since a code reload no longer touches the engine's own half at all. Two shadow settings came back wired
  rather than deleted: `engine.physics.gravity` and `.sub_steps` had no reader anywhere (physics world
  creation hardcoded `NYA_PHYSICS{2D,3D}_GRAVITY_DEFAULT` and `NYA_PHYSICS{2D,3D}_SUB_STEPS`), and now
  feed both, falling back to those same macros at zero — which is why the shipped `engine.nya` defaults
  (9.81, 4) change nothing.
- `[ ]` Components, as described under "Components: everything is a plugin". Convert the five existing
  optional dependencies first (curl, sqlite, Lua, Discord, Steam), because they show the five edits most
  clearly. Then every module above `math`. The generated umbrella header replaces `nyangine.h`'s hand-written
  list, and the generated `.clangd` ends the "flags are hand maintained" warning in `AGENTS.md`.
- `[x]` **Programs compose**, asked for 2026-09-22 and finished 2026-09-23: a full 3D game can also host an HTTP server for something,
  and can be started as one path of a CLI. So a program's `main` is a CLI over `nya_args` (the build tool's
  command tree, already in `base`), and "run the game" is one command of it beside others such as a headless
  dedicated server with its HTTP API, a TUI dashboard over the same state, or a one shot export. gnyame is the
  proof: `gnyame` plays, `gnyame serve` runs headless and serves, `gnyame export ...` does its job and exits,
  each with only the subsystems it needs brought up. Components decide what is linked; the command decides what
  is started. Done when gnyame has those paths and CI runs each.
  - **The parts are runtime parts, not program kinds** (set 2026-09-23). A GUI app, a TUI app, a game loop, a
    net server and an HTTP server are each a thing that is started, ticked and stopped, and one program runs any
    combination of them at once: a game hosting its own HTTP API and a TUI on the same process is not a special
    build, it is three parts started. So each gets the same shape — a config in, a handle out, a tick the
    program drives or a thread it owns — and `core_system.h`, which already registers engine subsystems and game
    systems alike, is where they are registered rather than each growing its own `_start`/`_stop` pair called
    from a hand written `main`.
  - What that needed, and what each turned into: the frame is the app loop's in every mode, and a part that
    wants a thread owns one (the threaded server); a part declares what it needs through
    `NYA_SystemEntry.needs`, so a UI part with no window is refused at startup by name rather than at its
    first draw; and `NYA_AppOptions.parts` is where a program hands its list over.
  - The CLI landed 2026-09-23: `gnyame` plays, `gnyame serve` runs headless and serves, `gnyame export <path>`
    writes the world it would have generated and exits, each registering a different set of parts. The flags
    are *named* in gnyame's command tree and *interpreted* by `net_config.h` one pair at a time through
    `nya_net_config_apply`, so the help text and the meaning of `--tickrate` cannot drift apart.
  - Still open: CI running each path, and the same shape for the other examples.
- `[ ]` Profiles are just named component lists: `cli`, `tui`, `server`, `desktop`, `game`, `web`. The project,
  every example and every test names one or lists its own components. Done when `cli_app` links no SDL, its size
  is measured and written here, and removing a component from gnyame's list either builds or fails naming the
  code that still depends on it.
- `[ ]` Tests per component: a component's tests run in a build that contains it and its dependencies only. A
  test that passes only because some unrelated component happened to be linked then fails.

## Phase 2 — standard library

What every kind of program in the examples table needs and `base` does not have yet.

- `[x]` `nya_file_write_atomic`: temp file, write, fsync, rename, fsync the directory. `base_file.h`, with
  `nya_filesystem_replace` and `NYA_FILE_MODE_EXCLUSIVE` under it. `nya_serde_save_file` and so saves,
  settings, scenes, key files and trained networks use it, as do tilemap saves, the plugin `nya.file.write`
  binding, trace captures and the integrity stamp; there was no config write back to move. `test_file_atomic.c`
  fails and crashes it before every step, directly and under the simulation harness (4 seeds × 160 steps), and
  the target is always the old bytes or the new. Open: the Windows half has only been syntax checked, a real
  crash leaves a `<target>.<pid>.<n>.tmp` beside the file with nothing sweeping them, and on Windows a symlinked
  target is replaced by a plain file. The database side files and plugin installs do not exist yet.
- `[~]` A binary encoding of `.nya` beside the text one: the same `NYA_Object`, the reflection's layout hash in
  the header so a peer built from other headers is refused rather than misread, and fuzzed like the others.
  Round trip text ↔ binary ↔ object as a property test. This is the wire format for `application/nya`, for the
  web client and for saves. Landed: `serde_nya_binary` (spec in its header), `nya_reflect_layout_hash`, the
  round trip laws, a fuzz target asserting re-encode byte identity, and HTTP serving it as
  `application/nya-binary`, typed or untyped. Missing: saves still use the text form, and the OpenAPI document
  lists only JSON. The hash covers offsets, so a wasm32 peer is refused for any DTO holding a pointer; a DTO
  meant for both uses `char[N]`.
- `[~]` **Date and time.** Landed: `clock_instant.h` (`NYA_Instant` and `NYA_Duration` as one field `s64` ns
  structs, `NYA_Date`, `NYA_TimeOfDay`, ISO weekday and week, overflow asserted or refused through `_checked`
  twins, never wrapped) and `clock_format.h` (RFC 3339 and RFC 9110 IMF-fixdate both ways, each refusal naming
  its rule and byte, leap seconds and the obsolete date forms refused on purpose), property tested and fuzzed.
  `nya_instant_now` reads through `nya_instant_source_set`, which simulations and sessions install. The HTTP
  server's `Date` header is the caller. Left: time zones, locale display, reflection and serde, the UI pickers,
  JWT `exp` and TOTP on `NYA_Instant`, and moving the raw `u64` callers (crash report, build time, log
  rotation, robots) over. The original list:
  What existed was in `platform/clock/clock.h`: wall clock timestamps as raw `u64` in s,
  ms, µs and ns; monotonic time; `nya_clock_civil_from_days` and its pair (exact, property tested); and
  `nya_clock_format_utc`, which writes two fixed formats (readable and filename safe) and is signal safe for the
  crash path. Nothing parses, nothing knows a time zone, and an instant is a bare `u64`, so seconds and
  milliseconds can be mixed up. What to add:
  - Types: `NYA_Instant` (UTC, ns since the epoch, a distinct type), `NYA_Duration`, `NYA_Date` and
    `NYA_TimeOfDay`. Arithmetic only between the types where it means something. Calendar arithmetic (add a
    month, end of month, week of year) on dates, never on instants.
  - Formats in both directions, parsers fuzzed: RFC 3339 / ISO 8601 (JSON, `.nya`, logs), RFC 9110 (cookies,
    `Date`, `Last-Modified`). JWT `exp` and TOTP steps take `NYA_Instant`.
  - **Everything is UTC inside.** A time zone is applied only when a value is shown to a person or read from
    one. The zone rules come from the platform (the tz database on Linux, the OS on Windows, `Intl` on the
    web), never vendored, so they update with the system.
  - Locale aware display (month names, 12 or 24 hours, first weekday) through the i18n tables.
  - Reflection knows the types, so they serialize as RFC 3339 in JSON and `.nya`, map to a column in `db`, and
    get date and time pickers in the UI.
  - The simulated clock in the testing harness drives `NYA_Instant` too, so time dependent logic (session
    expiry, TOTP, retention sweeps) is deterministic under simulation.
- `[ ]` **A general attribute system for reflection.** Today every annotation is special cased: `@key` became
  `is_key`, `@hint` an enum of four hints, `@tag` a `tag_value`, and `@skip`, `@flags` and `@on_apply` are each
  their own path in `src/build/pp/reflection.c`. Every new use (`@redact`, `@secret`, validation, UI labels)
  would mean another field on `NYA_ReflectField` and another branch in the generator. Instead:
  - Any `@name` or `@name(arguments)` on a type, field or enum variant becomes an attribute: a name plus typed
    arguments (`NYA_Value`s), stored in `const` tables like the rest of reflection.
  - **Components own their vocabulary.** Each component's descriptor declares the attributes it understands
    and their argument types: `db` declares `@key`, `@unique`, `@index`, `@table(name)`; `http` declares
    `@redact`; serde declares `@secret` and `@since(version)`; `ui` declares `@label(text)`, `@range(min, max)`,
    `@step`, `@hint`; validation declares `@min`, `@max`, `@length(min, max)`, `@nonempty`. An attribute nobody
    declared fails the build, so `@redcat` is a typo caught at compile time, not a silently unredacted field.
  - Lookup by attribute id, resolved at generation time. Each field also carries a bitset of the argumentless
    attributes, so hot paths (redaction, serialization) test a bit instead of comparing strings.
  - The existing special cases become ordinary attributes and their bespoke fields go, as one refactor with
    the reflection tests as the guard.
  - What this buys: validation attributes generate the fallible `user_so_from_dto` parse, so a DTO's rules are
    written once on the field; UI attributes let a property panel or a form be generated from a struct; ORM
    attributes replace the ORM's own conventions; and OpenAPI reads `@range`, `@length` and `@label` into the
    schema, so the client, the server and the docs share one statement of every rule.
- `[x]` URLs and percent encoding, parsed into a type once at the boundary. `base_url.h`: `nya_url_parse` for
  absolute http/https/ws/wss URLs and `nya_url_parse_target` for a request target, one bounded copy with each
  part an offset into it (slices into the receive buffer would dangle when it shifts), strict RFC 3986, and every
  refusal naming its rule and byte. It refuses the SSRF spellings of numeric hosts (`0x7f.1`, `2130706433`),
  `.`/`..` segments however encoded, `%2F` in a segment and a leading `//`, and a query name given twice.
  The HTTP server and the websocket plugin both use it and their two hand written parsers are gone, so a request
  whose target breaks those rules now gets 400. Property tested, fuzzed (5 minutes of AFL++, nothing found).
- `[x]` A `crypto` module: monocypher's X25519, Ed25519, XChaCha20-Poly1305, BLAKE2b and Argon2id behind our own
  names, plus SHA-256, HMAC, SHA-1 (TOTP and the WebSocket handshake only), base32, a constant time compare and
  the OS CSPRNG from `platform`. `net_crypto.c`, `nya_hmac_sha256` and the JWT code move onto it. Every primitive
  against its published test vectors. All of it is in `src/nyangine/crypto/`, proven against RFC 6234, 4231, 2202,
  7693, 7748, 8032, 9106, 4648 and draft-irtf-cfrg-xchacha, with the base and websocket copies deleted. Ed25519
  comes from monocypher's optional RFC 8032 file, now vendored, and `nya_crypto_sign_verify` refuses small order
  public keys, which monocypher's own verify accepts. base64url is still private to `http_auth.c`.
- `[~]` `db` as a module. Landed 2026-09-23 as `src/nyangine/db/` at rank 4, behind `NYA_MODULE_DB`, moved out
  of `plugins/sqlite`: `db_sql.h` is the connection, `db_orm.h` the reflection-driven mapping, `db_migrate.h`
  the schema difference, `db_extensions.c` the sqlean and sqlvec glue. Migrations are derived from the
  difference between two reflections and never hand written; the derivation emits `CREATE TABLE` and
  `ADD COLUMN` and refuses a drop, a rename, a type change or a moved key, because those are the four a
  derivation cannot tell apart from a mistake. A migration that fails halfway rolls its transaction back.
  `examples/web_server` keeps its notes as rows rather than an array.
  - The key seam is in place: `nya_sql_open` takes a key of `NYA_SQL_KEY_SIZE` bytes and a build that cannot
    use one refuses it rather than opening the database in the clear, which `nya_sql_encryption_available`
    answers for. SQLCipher itself is deliberately not vendored yet, so today every build answers false.
  - Still open: SQLCipher in place of vendored sqlite, so the whole database is encrypted (see "Decisions",
    encryption at rest). sqlean and sqlvec did not become components of their own; they are one glue file
    inside `db` instead, which is where they stay unless something needs them apart from it.
- `[ ]` Encrypted fields in `.nya` files: a reflection tag (`@secret`) makes serde write that field encrypted,
  with XChaCha20-Poly1305 under a key the program supplies. When the PGP component is present, it can also be
  encrypted to a recipient's public key. For secrets that live in files rather than the database: tokens in a
  config, keys in a save.
- `[x]` Structured logging: a record is a message plus typed key/value fields, formatted per sink. A terminal
  gets a human line and a server's file gets JSON lines. Still fixed buffers and no allocation on the log path,
  as today. `NYA_LogField` (string/int/float/bool) and the `nya_log_*_fields` macros carry up to
  NYA_LOG_FIELD_MAX fields on one record; the engine composes the human line — the message then `key=value`
  pairs — for stderr, the rotating file and the ring, and a `NYA_LogRecordSink` is the seam where a second
  shape lives, with `nya_log_record_render_json` giving one JSON object per line. The stderr sink, the size
  bounded rotating file with its retention limit, and the ring were already there; this is the typed fields
  and the per-sink rendering on top of them. A field carries a correlation id the way the per-thread log tag
  does, so everything logged while serving one request, running one job or ticking one session can be found
  together.
- `[ ]` A parsed newtype helper, so `Email`, `UserId` and `Username` are one macro and a fallible `_from_string`
  each rather than bare strings.

## Phase 3 — the secure server

`http_server` becomes a real application backend. The server trusts nothing from the network, including a
logged-in user.

- `[~]` **A threaded server.** Landed 2026-09-23. `workers` on `NYA_HttpConfig` picks the shape and zero is
  the default, which is the old one: one drain a frame on the thread that called init, nothing concurrent and
  nothing locked, and it stays the mode simulation and session tests run in because threads would take their
  determinism away. Above zero a listener thread owns the sockets — it accepts, reads, parses, spends the rate
  limit token and writes — and that many worker threads run the layers, the extractor and the handler, each on
  its own arena, reset after the answer. gnyame's web interface runs with two.
  - A route declares where it runs through `NYA_HttpAffinity`. `NYA_HTTP_AFFINITY_WORKER` is the default;
    `NYA_HTTP_AFFINITY_MAIN` is for a handler that touches program state (entities, the system registry, the
    metrics resource), and its exchange is queued to the frame, answered inside `nya_system_http_tick` and
    handed back to the listener to write. `nya_thread_main_only` (`base_thread.h`) guards the calls that only
    the frame may make, so a handler that forgot its `MAIN` crashes the first time it runs rather than
    corrupting something quietly.
  - No bound moved: everything in `http_types.h` holds in both modes, and the connection table, the rate
    limit buckets and the counters live on the listener thread and are only ever touched there, which is what
    makes them safe without a lock.
  - `tests/nyangine/http/test_server_threaded.c` drives the threaded mode, including a shutdown while a
    handler is still running, and runs under ThreadSanitizer.
  - Still open: the sockets. They are still SDL_net polled per pass rather than non-blocking over an OS
    readiness API (epoll on Linux, IOCP or `WSAPoll` on Windows) in `platform`, which is the Phase 1 move, and
    the worker count is a field a program passes rather than a setting in `engine.nya` with a default derived
    from the core count.
- `[~]` **Request logging with redaction.** Landed 2026-09-23 as `src/nyangine/http/http_log.{h,c}`, the layer
  moved out of `http_router.c`: a random `X-Request-Id` on every answer, set as the answering thread's log tag
  (`nya_log_tag_set`) around the dispatch, so every line one request causes carries it in either server mode;
  the three levels (`summary`, `headers`, `bodies`) configured under `engine.http_log` and applied again on
  hot reload through `@on_apply`; the header deny list, always applied and extensible through `deny`; bodies
  redacted structurally through the route's `request_type` and `response_type` reflection, with `@redact` read
  by the reflection generator; query parameters redacted by name; the address policy (truncated to /24 or /48
  by default, full or none by setting); and fail closed — a body that does not parse as its route's DTO is
  logged as its size and eight bytes of BLAKE2b, never as bytes. `./build check` refuses a reflected field
  named `password`, `token`, `secret` or `code` that carries neither `@redact` nor `@loggable`, and
  `tests/nyangine/http/test_log.c` ends with the property: a marker in every tagged field reaches no sink, at
  any level.
  - Every exchange gets a random request id, returned as `X-Request-Id`. It is attached to every log line,
    error, trace span and crash report produced while serving that request, including a handler queued to the
    main thread.
  - Three levels, set per server in `engine.nya` and changeable on reload: `summary` (the default: method,
    route, status, duration, bytes in and out, request id, user id, address), `headers`, and `bodies`. Bodies
    are capped at a written-down size. A binary `.nya` body is logged decoded to text. Any other binary body is
    logged as size and hash only.
  - **Redaction happens in one place, before the record reaches any sink**, so no sink (file, ring, crash
    report, terminal) can leak what another would not:
    - Headers on a deny list are always redacted, whatever the level: `Authorization`, `Cookie`,
      `Set-Cookie`, `Proxy-Authorization`, plus a configurable list for API keys.
    - Body fields are redacted structurally, through the DTO's reflection rather than a regex: a DTO field
      tagged `@redact` (password, TOTP code, recovery code, PGP response, invite token) is logged as
      `<redacted>` at any depth.
    - Query parameters are redacted by name the same way.
    - Fail closed: a body that does not parse as its route's DTO is logged as size and hash, never raw, since
      an unparsed body is exactly where an unredacted secret would hide.
  - **Privacy:** client addresses are truncated in request logs by default (/24 for IPv4, /48 for IPv6), and
    can be configured to full or none. Security events (failed logins, reused refresh tokens, permission
    denials) keep the full address with their own, shorter retention, because abuse handling needs it.
  - The negative space as a property test: fill every `@redact` field of every DTO with a random marker, send
    it through every level, and assert that the marker appears in no sink's output. A DTO field named like a
    secret (`password`, `token`, `secret`, `code`) with no `@redact` tag fails `./build check` unless it is
    marked as deliberately loggable.
  - Tagged fields are redacted wherever reflection writes them, so the same rule covers `net` messages, IPC
    control commands and debug dumps, not only HTTP.
  - Still open: a trace span and the crash report's own fields — the request id reaches a report only through
    the log ring — and the security-event retention below, which is still one retention for everything.
- `[ ]` The Model/SO/DTO split in `http` and `db`: the conversion naming, the `web` profile refusing model and
  SO headers, and the `@secret` check. `web_server` moves onto it first. Today its notes resource keeps one
  `ExampleNote` in memory and builds the response by hand as an `NYA_Object` in `note_to_value`, so it has a
  Model and no DTO type at all.
- `[~]` **Role based permissions, modelled on Discord.** Landed 2026-09-22 as `src/nyangine/permission/`, rank
  3, below `net` and `http`: bits, roles with positions, per resource overwrites, Discord's resolution order
  (with a naive oracle as the test), the hierarchy rules enforced inside the calls, an audit ring, and runtime
  labels so a role editor asks the table what exists rather than being compiled against one program's bits. Two
  callers: gnyame's guild (the pause menu shows your rank and greys the kick button by the resolver) and
  gnyame's web interface (the same table over QUERY and DELETE, where the refusal is a 403), and the role
  editor in gnyame's pause menu, which is the caller that changes the table rather than asking it: a toggle is
  disabled when the resolver would refuse the edit, and the rows are built from the roles the table holds and
  the permissions that carry a label, so the window never learns what game it is editing. Routes declare the
  permission they need and the extractor resolves it before the handler runs, so a handler behind one cannot
  be reached unchecked. Missing: the resolution cache and its version counter, which wait for `db`;
  `SECOND_FACTOR` becoming authentication strength rather than a scope; and per resource overwrites in the
  editor, which today edits roles only.

  The original entry, kept because the parts above are what remains of it: a `permissions` component with no
  dependency on `http`,
  so the multiplayer server (kick, ban, mute), the IPC control socket and the HTTP routes all check one thing.
  Today there are four fixed JWT scope bits (`READ`, `WRITE`, `ADMIN`, `SECOND_FACTOR`) and no roles. The model:
  - **Permissions** are bits in a `u64`, declared by the program as a `@flags` enum, with a few reserved by the
    engine (`ADMINISTRATOR`, `MANAGE_ROLES`, `MANAGE_USERS`). OpenAPI lists what each route requires.
  - **Roles** have a name, a position and a permission set. A user holds any number of roles, and everyone
    implicitly holds `@everyone`. A user's base permissions are the OR of their roles. `ADMINISTRATOR` grants
    everything. The owner sits above every role.
  - **Overwrites per resource**, which is what Discord calls a channel. Any resource (a room, a document, a
    project) can carry allow and deny sets per role and per user, applied in Discord's order: `@everyone`'s
    deny then allow, then every role's denies together, then every role's allows together, then the user's own
    deny and allow. The result is a pure function of roles and overwrites, `nya_permission_resolve`.
  - **Hierarchy stops escalation.** A user edits or assigns only roles below their own highest, grants only
    permissions they hold themselves, and cannot act on a user whose highest role is equal or above. Property
    tests assert "no sequence of operations yields a permission its actor did not hold" against random role
    operations from the simulation harness, and the resolver against a naive oracle.
  - **Tokens carry who, not what.** The JWT names the user and how they authenticated (password, second
    factor). Permissions are resolved per request from `db`, cached and invalidated by a role version counter,
    so taking a role away applies to the next request rather than when a token expires. `SECOND_FACTOR`
    stops being a scope and becomes the session's authentication strength. A route can require it for
    dangerous permissions, the way Discord requires 2FA for moderators.
  - **Deny by default.** A route declares the permission it needs and how the resource is found. The extractor
    resolves and checks before the handler runs, so a handler cannot be reached unchecked. A route that
    declares nothing is refused at merge unless it is explicitly `PUBLIC`.
  - **Every change is audited:** who changed which role or overwrite, from what to what, and when, in an
    append-only table.
  - **Not an HTTP feature** (set 2026-09-22). A guild in a game is the same problem as a project on a server:
    roles, ranks, who may kick whom, who may edit the shared thing. So `permissions` sits below `http` and
    below `net`, knows nothing about a request, and takes ids rather than tokens. A game asks it whether this
    player may demote that one; the HTTP extractor asks it the same question about a route. One resolver, one
    hierarchy rule, one audit trail, and the same role editor UI over either, since the UI is `nya_ui_*` and
    runs on all four backends.
  - **Callers:** the role editor in `web_frontend` (reorder roles, toggle permissions, set overwrites per
    resource) is the demonstration, the multiplayer example gives the host a moderator role, and gnyame gets a
    guild whose ranks are the same roles with the same resolver — the proof that nothing here is web only.
  - Plugin permissions stay what they are: compile time, per build, for code rather than people. The two
    systems share no bits.
- `[~]` **Users and sessions**: an `accounts` component over `db`, `crypto` and `permissions`, with a user
  Model, SO and DTO as in "Model, SO, DTO". Passwords use Argon2id, with the parameters written down beside
  their measurement. Landed 2026-09-23 as `src/nyangine/accounts/` at rank 5: the module and its whole model
  are in; what remains is mounting it on a caller (the example/frontend) and a couple of self-service edges.
  - **Self service:** register, log in, log out, change the password (ends every other session), change the
    username, set up and remove a second factor, regenerate recovery codes, list and revoke one's own sessions.
    In: register/authenticate/password_change (ends sessions)/session list/revoke/revoke_all, recovery
    regenerate. Missing: change the username, and the second-factor set-up/removal wired to an account (the
    TOTP primitive exists in `http_totp`, unbound to a user).
  - **Registration policy** — DONE. `accounts_invite.h`: open, invite (single-use codes hashed in db with an
    expiry, spent-only-on-success), or closed. `nya_account_register` enforces it. First account is the CLI's
    to make with `nya_account_create` and no default password anywhere.
  - **Recovery without email** — DONE. `accounts_recovery.h`: ten single-use base32 codes hashed in db,
    throttled, feeding a password reset that ends every session. Admin reset is `nya_account_password_reset`.
  - **Admin management** — model DONE, the screen is the caller's. `disabled_set` (refused at login, sessions
    ended), `roles_set`, `find`, session revoke, and the append-only **audit table** (`accounts_audit.h`) that
    every mutation records into. Missing: ban with a reason and optional expiry as its own state (disable + an
    audit reason stands in today), and the admin *screen*.
  - **Lockout without denial of service** — DONE. `accounts_throttle.h`: a doubling wait per account and per
    address, never an outright lock.
  - **The user's data is theirs** — DONE. `nya_account_export` (redacted `NYA_Object`, `.nya` or JSON) and
    `nya_account_destroy` (removes rows + sessions + identities + recovery + invites, keeps audit as a
    tombstone). Both required, both in.
  - **Username and display name** — normalised form + control-character refusal + length bounds are in
    (`nya_account_username_normalize`); the display-name newtype and look-alike (UTS #39) folding beyond ASCII
    case are still open.
  - **Beyond the reference, DONE:** provider identities so one account arrives through Steam/Discord/OIDC or a
    password (`accounts_identity.h`); a rotating **keyring** so a signing-key roll logs nobody out
    (`http_keyring.h`); **sealed cookies** for stateless client state (`http_seal.h`); **refresh rotation with
    stolen-token detection** (`nya_account_session_rotate`); the session **sweep** on a timer; `permission`'s
    absolute `permissions_forbidden` and ownership-aware `may_act`.
  - **Callers:** `web_frontend` has the self service screens and an admin screen, and `cli_app` gains the
    owner bootstrap and an admin command set over the IPC control socket, for a server with no browser open.
    STILL OPEN — nothing mounts `accounts` over HTTP yet, so the two-user IDOR/BFLA test has no target. This is
    the next step.
  - **Reference:** `~/projects/webapp-template/services/core/server` already implements this stack in Rust
    (axum, diesel, postgres), about 2.9 kLOC for auth, sessions, users and roles. Port its shape, not its code.
    It has these, and they are carried over:
    - Tables `users`, `sessions`, `roles`, `user_roles`, `invites`, `login_attempts`, `login_restrictions` and
      `encryption_keys` (`crates/models/migrations/0001_auth/up.sql`). Sessions record `valid`,
      `invalidated_at` and `invalidated_reason`, are closed as abandoned after 30 days unused, and only the
      newest 100 invalidated rows are kept. Postgres triggers do that sweeping there; here it is a system on a
      timer, since SQLCipher has no scheduler.
    - A keyring of the current signing key plus previous ones (`crates/auth/src/keyring.rs`), so the JWT key
      rotates without logging everyone out — DONE, `http_keyring.h`: newest key seals, every non-expired key
      verifies, `nya_http_keyring_rotate` mints when due and prunes the expired. Ported onto `http_seal` rather
      than a JWT. Persisting the ring is the program's call.
    - `constant_time` in `crates/auth/src/defense.rs`: every auth response is padded to at least 200 ms plus
      up to 15 ms of jitter, so the response time cannot reveal whether a user name exists.
    - TOTP in two steps (`/otp/enable`, then `/otp/validate` before it takes effect), `/otp/disable`, and
      regenerating backup codes.
    - Permissions that know ownership: `CanEditOwnPost` beside `CanEditAnyPost` — DONE,
      `nya_permission_may_act(subject, resource, owner, any, own)`.
    - Roles with a unique priority and both `permissions` and `permissions_forbidden`, on roles and on users
      directly — DONE, `nya_permission_role_forbid_set` / `nya_permission_subject_forbid_set`, an absolute deny
      resolved after every allow and overwrite, below only the owner. Discord's per-resource overwrites already
      add to that.
    - Tests split by domain into simulation cases (`tests/simulation/auth_cases.rs`, `session_cases.rs`,
      `user_cases.rs`) plus fuzzing. Mirror the same split in `tests/nyangine/accounts/`.
    - Random ids (UUIDs there) rather than sequential ones, so ids cannot be enumerated.
    Where this plan differs, it does so on purpose. The refresh token is an opaque random value stored hashed,
    where the template stores JWTs in the row. And there is a refresh token reuse check, which the template
    does not have — DONE, `nya_account_session_rotate`: a retired token replayed revokes the whole session.
- `[~]` Sessions: an access token and a refresh token. In as of 2026-09-22: cookies in both directions
  (`http/http_cookie.*`), the extractor reading the access token from the `__Host-session` cookie when there is
  no `Authorization` header, and `examples/web_server` issuing one where a real server would — after the second
  factor proves the account, signed with a secret it makes from the CSPRNG at startup. The `__Host-` and
  `__Secure-` rules are enforced at the call rather than left to the browser to drop silently, and the parser is
  fuzzed with re-rendering as its oracle. The refresh token, its rotation with reuse detection, logout that
  revokes, and the idle and absolute limits all landed 2026-09-23 in `accounts_session.*`
  (`nya_account_session_issue`/`validate`/`rotate`/`revoke`/`list`, sliding idle bounded by an absolute life).
  Missing now: wiring the access token as a short `http_seal` over the keyring, and mounting the signed-in
  devices list on a caller.
  - **Access token:** a JWT naming the user, the session and how they authenticated, living minutes (the
    number written down with its reasoning). Sent in a `__Host-` cookie: `HttpOnly`, `Secure`,
    `SameSite=Strict`, `Path=/`. Checked by signature alone, no database read, which is why it must be short.
  - **Refresh token:** 256 random bits from the CSPRNG, not a JWT, sent in its own `__Secure-` cookie whose
    `Path` is only the refresh route, so it is never sent with an ordinary request. Stored only as a BLAKE2b
    hash in a session row (user, created, last used, address, user agent, absolute expiry).
  - **Rotation with reuse detection:** every refresh issues a new pair and retires the old refresh token. A
    retired token presented again means it was stolen, so the whole session is revoked and the user is told.
  - **Limits:** idle timeout and absolute lifetime per session. A password change or a lost second factor ends
    every session but the current one.
  - **Logout** revokes the session row and clears both cookies. The user can list their sessions ("signed in
    devices") and revoke any of them, which is a `web_frontend` screen.
  - A cookie parser that rejects rather than repairs, fuzzed. Both tokens are refused on any route over plain
    HTTP.
- `[~]` CSRF: `SameSite=Strict` plus an `Origin` check on every non-GET route as a layer, so a handler cannot
  forget it. The origin half is in: dispatch refuses a request that changes something (any method that is not
  safe) with 403 before every layer and the handler when `Sec-Fetch-Site` is anything but `same-origin` or
  `none`, or when `Origin` names another host than `Host`; one with neither is not a browser page and passes.
  The router's table check makes every write route declare 403. `SameSite=Strict` is in with the cookies:
  `NYA_HTTP_SAME_SITE_STRICT` is the default of `NYA_HttpCookie`, so a session cookie carries it unless a caller
  writes otherwise, and `http_auth.h` says why the cookie form of a token is only safe with both defences.
- `[~]` Simple limits in process, as layers and at accept, each bound a `#define` with its reasoning: connections
  per address and in total, requests per address as a token bucket, and a stricter bucket with backoff for
  login and second factors. Over a limit answers 429 or refuses the accept; it never queues unbounded. Limits
  key on the peer address, never on `X-Forwarded-For` unless a trusted proxy address is configured. In:
  the per address connection cap at accept, and a token bucket per address in the server (64 tracked, the
  stalest evicted) answering 429 with Retry-After. Missing: the login bucket with backoff (waits for the
  login route) and the trusted proxy address.
- `[~]` **Second factor: TOTP through authenticator apps.** It lands first, and it needs no new dependency.
  Landed 2026-09-22: `crypto/crypto_totp.*` is the primitive (counter to six digits) and `http/http_totp.*` the
  enrolment, the verification, the replay guard and the per account limit, with `examples/web_server/` holding
  the secret and the guard since there is no user store yet. Proven against RFC 4226 appendix D and all six
  RFC 6238 SHA-1 rows. Missing: the QR encoder (the URI is text only), disable and reissue, the constant time
  response padding, and the per address limit on the login route that does not exist yet.
  - RFC 6238 with the parameters every app accepts: HMAC-SHA1, 6 digits, a 30 second step, and a 160 bit
    secret from the CSPRNG. Proven against the RFC's published vectors.
  - Enrolment in two steps, like the template: the server shows an `otpauth://totp/<issuer>:<user>?secret=...`
    URI as a QR code and as base32 text, and the factor turns on only after the user enters one valid code.
    Recovery codes are issued at that moment, shown once and stored hashed.
  - Verification accepts one step of clock skew either side. It remembers the last step used per user, so one
    code cannot be replayed within its window. It is rate limited by the login bucket and padded by the
    constant time defence.
  - Disabling it or regenerating recovery codes requires a current code or the password again.
  - A QR encoder is the only new piece. Nayuki's `qrcodegen` (C, MIT, one file, widely used) vendored, or a
    small one in `base`. It draws through `nya_ui_*` as filled cells, so it works in the native, terminal and
    web presenters alike.
- `[~]` **Second factor: PGP by decryption.** The encryptor landed 2026-09-23 as
  `src/nyangine/plugins/pgp/pgp.h`: `nya_pgp_encrypt` encrypts a short message to a user's armored
  public key, `nya_pgp_fingerprint` reads the fingerprint an account stores and a person checks, and
  `nya_pgp_available` is what a program asks at startup. The test generates a key, encrypts to it and
  decrypts with gpg, because a round trip is the only thing that proves any of it.
  - **Nothing is vendored**, decided 2026-09-23 after looking at the field. `gpg` is spawned through
    `base_command.h` with a `--homedir` of its own per call, so the server's keyring is never touched
    and a machine without gpg loses this feature and nothing else. What was weighed:
    - **GPGME** is a wrapper that drives this same binary over IPC. It would add libassuan and
      libgpg-error to the link to buy a typed API over a twenty line spawn.
    - **RNP** (Thunderbird's) is C++ with CMake and its own crypto backend — Botan or OpenSSL —
      duplicating the crypto module this engine wrote itself, and it has historically had no smartcard
      support, which is the case this feature exists for.
    - **Sequoia** is Rust, which would put a second toolchain in a build that is one clang.
    - **Writing it** means, for encryption alone, OpenPGP packet writing, ECDH with a key derivation
      nobody else uses, RSA PKCS#1 v1.5 *encryption* (crypto only verifies), AES in a mode this has
      none of, and the interoperability surface where OpenPGP bugs live. It is the right answer only
      if the run time dependency ever becomes unacceptable — a Windows service, say, where gpg4win is
      not a reasonable ask.
    - The asymmetry that makes this easy: every **private** key operation in this design happens on the
      user's machine, where gpg, the agent, the PIN and a YubiKey's touch policy already live. The
      server does one public key operation and a constant time comparison.
  - Still open, and unchanged: the state. A pending login, the code stored hashed, single use and
    expiring, is what the accounts work brings, and until then there is a primitive and no flow. The server encrypts a one-time code to the user's public key; the
  user decrypts it with their private key (`gpg -d`, or an OpenPGP smartcard such as a YubiKey) and types the
  code back. The server only ever does public key operations and a constant time comparison, and the private
  key never leaves the user.
  - The encrypted message carries context before the code: the program, the account, the time and the
    requesting address. Someone who decrypts it sees what they are approving, which blunts phishing.
  - The code is 128 random bits shown as a short word list or base32, so it is easy to type. It is single use,
    expires in minutes, is bound to the pending login, and is stored hashed. That needs state, so today's
    stateless HMAC challenge, which can be replayed within its window, is replaced.
  - Enrolment in two steps: upload the public key, then decrypt one challenge before it counts. Keys that
    cannot encrypt, or that are expired or revoked, are refused with a reason.
  - The seam changes: `nya_http_second_factor_set` takes "encrypt this to that key", not "verify this
    signature". The PGP component fills it, and without the component the option does not appear.
  - A `pgp` component, off by default; see "Decisions" for the library.
  A user may enrol both factors and use either one. Recovery codes cover losing both.
- `[~]` A security header layer on by default: a strict CSP generated from what the app serves, HSTS,
  `nosniff`, `frame-ancestors 'none'`, `Referrer-Policy: no-referrer`, and COOP/COEP, which wasm threads need
  for `SharedArrayBuffer` anyway. Done in the response head writer rather than a layer, so no route, layer or
  error path can skip it: every answer carries `default-src 'none'` with framing, `<base>` and form targets
  shut, `nosniff`, `no-referrer`, COOP, COEP and CORP. A response that sets a header of the same name replaces
  the default; `/docs` does, naming its one style block by SHA-256, and `test_server` checks the hash against
  the page it serves. Left: HSTS, which waits for TLS, and generating the policy for the static web bundle.
- `[~]` A WebSocket server in `http`, sharing one frame codec with the curl client rather than a second copy.
  It is the live channel for the web client and the transport for browser multiplayer. Landed 2026-09-22:
  `http/http_websocket.*` is the RFC 6455 wire format for both ends, the curl plugin includes it and lost ~350
  lines, and `http/http_websocket_server.*` runs the upgrade and the sockets on the listener that was already
  there. The upgrade spends a rate limit token, keeps its connection slot and per address cap, and goes through
  the cross site check — which it did not before, since the router only applied that to unsafe methods and an
  upgrade is a GET. Fuzzed with 34 committed inputs, where a decoded header must re-encode byte for byte and
  the same bytes fed whole and one at a time must end in the same state. Missing: permessage-deflate and every
  extension, subprotocol negotiation, sending a message in fragments, authentication on a stream, and 426 for a
  bad version (it answers 400 with the version in the problem body, since the status enum has no 426).
- `[~]` Static serving of the web bundle: content hashed names, immutable caching, ETags, precompressed at build.
  HTML, CSS and JS are assets like any other (set 2026-09-22): they go through the asset system, so they get
  its handles, its hot reload in development and its baked blob in a release, and the server serves them from
  there. No second pipeline for web files. Landed 2026-09-22 as `http/http_static.*`: one exact route per file
  and no path resolution anywhere, so a request either is a mounted string or it is a 404 and there is no root
  to escape. SHA-256 at mount spells both the hashed name and the ETag, `If-None-Match` answers 304, and the
  caller reads the bytes through `nya_asset_read` so `http` does not grow an edge into `core`. Missing: gzip or
  brotli, which needs a vendored compressor a browser can decode — LZ4 is not one, so nothing sets
  `Content-Encoding` and nothing sets `Vary`. No ranges, no `Last-Modified`, and a mount is a snapshot rather
  than hot reloaded, because the hash has to describe the bytes going out.
- `[ ]` TLS in process: mbedTLS vendored and wrapped in `http`, TLS 1.3 first and 1.2 as the floor, certificate
  and key from files named in config and hot reloaded on change. A handshake is fuzzed like the request parser.
  Plain HTTP is loopback only, and on a public address it only redirects to HTTPS. The server never sets a
  `Secure` cookie over plain HTTP.
- `[ ]` ACME (Let's Encrypt) so a single executable gets and renews its own certificate. Without it, "one
  binary" still needs certbot beside it. After TLS works with a supplied certificate.
- `[~]` `docs/http.md` ("What belongs to a proxy") and the headers of `http.h` and `http_server.h` say TLS
  belongs to a proxy; the rate limit half is updated. They change in the same commits as the code above.
- `[~]` **Bots and webhooks**, asked 2026-09-23. A Discord or Twitch bot is a program that talks to an API over
  HTTPS, holds a WebSocket open, and takes signed callbacks. Three of those four are in: `plugins/curl` does
  HTTPS requests and `wss` client sockets, `http` receives, and `http_webhook.h` (2026-09-23) proves an incoming
  callback came from who it claims — HMAC-SHA256 as Twitch EventSub and GitHub sign, Ed25519 as Discord signs an
  interactions endpoint, both over the bytes that arrived, both with a timestamp window against replay.
  - The Discord **bot** landed 2026-09-23 as `src/nyangine/plugins/discord_bot/`, beside `plugins/discord`
    rather than inside it: that one is the GameSDK — rich presence and join secrets — and this one is a program
    that *is* a Discord client. `discord_gateway.h` is the `wss` half (identify, heartbeat with the jittered
    first beat, sequence numbers, resume on a resumable close, a fresh identify on one that is not, and the
    six close codes that must never be retried), and `discord_rest.h` the HTTP half (send a message, register
    a slash command, answer an interaction), queued and drained against Discord's per-*bucket* rate limit
    headers, because a client that ignores those does not get a slower bot, it gets a banned one.
    `examples/discord_bot` is the program.
  - The Telegram **bot** landed 2026-09-23 as `src/nyangine/plugins/telegram_bot/telegram.h`: `getUpdates` with
    the offset as the acknowledgement, messages and callback queries as updates, `sendMessage` and
    `answerCallbackQuery` queued behind a cooldown a 429's `retry_after` sets, and the webhook secret compared in
    constant time. Long polling is off unless a caller sets `poll_timeout_s`, because the transfers are
    synchronous and a frame cannot wait thirty seconds for a quiet chat.
  - The Twitch **bot** landed 2026-09-23 as `src/nyangine/plugins/twitch_bot/`: `twitch_eventsub.h` is the
    socket (welcome, keepalive silence as the only liveness signal there is, the reconnect that keeps the old
    socket until the new one is welcomed, and the duplicate and replay rules Twitch documents), and
    `twitch_helix.h` the calls that subscribe it to something and answer over it. Chat is no longer IRC: it
    arrives as `channel.chat.message` over EventSub and a reply is `POST /helix/chat/messages`, so a chat bot
    needs no second protocol and no TLS of its own — the one reason this item wanted TLS in process is gone.
    - Still open for Twitch: minting and refreshing tokens, which is a browser flow ending at a redirect url
      and a client secret this deliberately never holds. A program that owns a browser owns that.
  - Missing for **sending** a webhook: nothing but a helper. `nya_request_post` posts a JSON body today; what a
    sender owes is a signature over what it sends, a retry with backoff, and not blocking the frame while it
    does either.
  - A bot is also the first program that is not a game and not a server: it is the composition entry above, with
    a net part and no window.
  - **One client per service, and no "connections" module**, decided 2026-09-23 when Telegram made it two. The
    protocols do not rhyme: Discord holds a socket and reads rate limit buckets out of headers, Telegram polls an
    offset and is told to wait by a number in a body, Twitch will do both at once. A facade over the three would
    be a switch statement with three arms and a lowest common denominator that fits none of them, and the thing a
    caller actually wants — "answer this message" — is one call either way.
    - What repeats is mechanical, and Twitch made it three on the same day, so the extraction is now owed: a
      queue of calls with attempts and a backoff, the `perform` / `now_ms` seam that lets a test drive a client
      with no network, the bounded copies, and the token that is wiped and never logged. Three copies of that
      exist in `discord_rest.c`, `telegram.c` and `twitch_helix.c`, and the third was where the copying started
      to read as copying rather than as each client saying its own thing.
      - The shape to extract, when it happens: a `bot_rest` with the queue, the attempts, the backoff and the
        transport seam, taking a per-service vtable for "build this call's body", "which route and method", and
        "read the rate limit out of this reply" — the three places the services genuinely differ. Each client
        keeps its own header and its own vocabulary, so a caller still writes `nya_telegram_send` rather than
        something generic with a service argument.
    - Inbound is already shared, and was the piece worth sharing: `http_webhook.h` verifies a signed callback for
      whoever sends one, and Telegram's weaker "echo my secret back" check sits beside it saying so.
- `[x]` **OpenID Connect**, asked 2026-09-23 and landed the same day as `src/nyangine/plugins/oidc/`: the
  authorization code flow with PKCE, discovery, a JWKS cached by `kid` with a rate limited refetch for a
  rotation, and an id_token this engine verifies itself rather than trusting because TLS carried it.
  - **RS256 and ES256 both**, because a provider signs with one or the other and `crypto` had neither.
    `crypto_rsa.h` is PKCS#1 v1.5 over SHA-256 with the padding rebuilt and compared whole; `crypto_ecdsa.h`
    is P-256 with the on-curve check every key goes through. Both are verification only, on published
    numbers, for the reasons their headers give — and where this engine signs, it still signs Ed25519.
  - `alg` is compared against those two spellings and nothing else, which is the whole of `alg: none` and
    the HS256 downgrade: the module never computes an HMAC, so there is no confused-algorithm path to fall
    into. The key a `kid` names must also be of the family `alg` asked for, or an RSA key could be handed to
    the ECDSA verifier.
  - Every refusal is its own message: the issuer, the audience in both its string and array forms, the
    expiry, an `iat` in the future, the nonce, an `azp` that is not this client, an unknown `kid`, a
    signature that does not verify. A refused token leaves the claims struct empty rather than half filled.
  - The pending login — state, nonce, verifier — is the caller's to keep, because a provider is a
    long lived singleton and a login is not. That also means **the caller compares the `state` it stored
    against the one the callback carried**; the module never sees a query string and says so in its header.
  - The token endpoint is form encoded, as RFC 6749 section 4.1.3 requires: `NYA_REQUEST_BODY_FORM`
    landed in `plugins/curl/request.h` on 2026-09-23, so there is nothing to decide per provider.

- `[ ]` A pentest pass over `http_server` (authn/authz bypass, session fixation, CSRF, injection through the ORM,
  path traversal in static serving, request smuggling, resource exhaustion). Every finding becomes a test.

## Phase 4 — the web client

C compiled to wasm, the same headers as the server, no HTML, CSS or JS written by hand.

- `[ ]` Toolchain: clang's `wasm32` target, which the host clang already has, over wasi-libc, and a JS loader
  the build generates. The first milestone is `base`, `serde` and `crypto` with their tests running under node.
  See "Decisions" for the probe.
- `[ ]` `platform/web`: clock, CSPRNG, storage (OPFS), `fetch`, WebSocket, input events, clipboard. The same
  interfaces as `platform/linux`.
- `[~]` The UI's presenter seam. Landed 2026-09-22: `ui/ui_present.h` is the interface, `ui_present_shape.c`
  today's drawing behind it, and `ui_present_record.c` a presenter that draws nothing and keeps the widget
  stream, reachable from gnyame on one key and used by the tests to drive a UI with no font and no GPU.
  Dispatch is one indirect call per widget, never per glyph, and the twenty widget kinds go through one switch
  so a new kind is a `-Wswitch` error rather than a null pointer. The claim below was false when it was
  written: `ui_widgets.c`, `ui_layout.c`, `ui_text.c` and `ui_window.c` held about sixty direct
  `nya_render2d_*`/`nya_font_*` calls, all now behind the seam, and `NYA_Font` left the module. Left before a
  DOM presenter: the UI still hit tests the pointer itself, so a presenter cannot report an activation; there
  is no per pass retire for an element that stopped being declared; and text is measured synchronously rather
  than placed by the backend. The original entry, kept because its four backend rule still stands:
  Today `ui_draw.c` is the only file naming a primitive, which is exactly right
  for GPU and terminal, but a DOM presenter needs widgets, not shapes. The widget model and layout stay one; a
  presenter receives widgets and either draws them (shapes) or keeps a DOM in step with them. Native and terminal
  go through the shape presenter unchanged. **One widget, four backends** (set 2026-09-22): the same `nya_ui_*`
  call works unchanged on the GPU, in a terminal, in the browser as wasm (CSR) and rendered on the server (SSR,
  below). A widget that only works on some of them is not finished, and each widget's example runs on all four.
- `[ ]` The DOM presenter: real `<input>` elements so password managers, autofill, IME and screen readers work,
  styles from the same `NYA_UIStyle`. A login form drawn on a canvas is refused by every password manager, and
  that alone decides against canvas-only for apps.
- `[ ]` **Server rendered UI, htmx style (SSR).** Asked for 2026-09-22: a program that does not want a whole wasm
  client returns its UI from the server and keeps the state between requests. The same UI code, run per request:
  - An HTML presenter beside the DOM one. Where the DOM presenter keeps live elements in step, this one writes
    the widgets out as HTML text once, escaping in one place. Every interactive widget becomes a real form
    element carrying its widget id, which is already a stable hash of scope and label, so a posted id finds its
    widget again on the next request with no table of its own.
  - A request is one UI pass: the server decodes the post into the same input the other backends feed the UI
    (which widget was activated, what a field now holds), runs the pass, and answers the panel that changed as a
    fragment. htmx swaps it in (`hx-post`, `hx-target`, `hx-swap`), so there is no page reload and no JS of ours.
    Without JS it still works as plain forms and full page loads.
  - htmx vendored and pinned like any other vendor (one file, 0BSD, about 16 KB compressed), an asset served
    from the static bundle so the CSP needs only `script-src 'self'`. Configured with `allowEval` off and
    `selfRequestsOnly` on, so it can neither run strings as code nor talk to another origin. Nothing of it is
    hand written.
  - State between requests: the UI's own state (focus, which panel is open, a window's position, a half typed
    field) in a cookie, encrypted and authenticated with XChaCha20-Poly1305 under a server key, bound to the
    session so it cannot be replayed into another one, and bounded by the 4096 byte cookie limit with the bound
    asserted. Anything larger, and anything that is really the user's data, lives in the session row in `db`
    instead. A cookie is never trusted as more than a hint: a tampered one fails to decrypt and the UI starts
    fresh.
  - Security is what the server already has: CSRF through `SameSite=Strict` and the `Origin` layer, permissions
    checked per request before the pass runs, and redaction of the posted fields through their DTOs.
  - Caller: `http_server` serves the same screens `web_frontend` runs as wasm, from the same source file, so the
    two can be compared side by side. Depends on the presenter seam, cookies and the threaded server.
- `[ ]` A WebGPU backend for render2d, then render3d. Shaders go HLSL → SPIR-V → binding rewrite → naga → WGSL
  at build time; see "Decisions" for what was measured. Games draw into a canvas
  through it, with the DOM presenter or the shape presenter over them.
- `[ ]` Typed calls from the route tables: the client calls a route by its table entry with the request DTO and
  gets the response DTO, both through binary `.nya`. The client sees DTOs only; see "Model, SO, DTO". The
  same tables that generate OpenAPI generate this, so a route cannot drift between the two sides.
- `[ ]` **A widget set like shadcn/ui.** Small, plain, composable widgets that look good with no styling at
  all, where everything visual comes from theme tokens (colours, radius, spacing, type scale) in the UI style
  file, the way shadcn/ui draws from CSS variables. Every widget works in all three presenters (GPU, terminal,
  DOM), is reachable by keyboard, and has a caller in an example. What exists today (`ui.h`) and what is
  missing:

  | Group      | Have                                                                   | Add                                                                                   |
  | :--------- | :--------------------------------------------------------------------- | :------------------------------------------------------------------------------------ |
  | Inputs     | button, text input, toggle, radio, slider, dropdown (select), colour picker | checkbox, switch, textarea, input OTP (TOTP and PGP codes), combobox (select with search), toggle group, date picker and calendar (on `NYA_Date`), number input, file drop zone |
  | Forms      | label, disabled scope                                                   | form: fields from a struct's reflection attributes (`@label`, `@range`, `@length`), inline validation messages, submit state |
  | Overlays   | scrim, modal event, floating dropdown                                   | dialog, alert dialog (confirm), sheet and drawer, popover, tooltip, hover card, context menu, dropdown menu |
  | Feedback   | —                                                                       | toast (non-blocking, queued), alert (inline), progress, spinner, skeleton               |
  | Layout     | panel, window, section (collapsible), tabs, scrolling, space            | card, separator, accordion, resizable split panels, sidebar, aspect ratio box         |
  | Navigation | tabs                                                                    | menubar, breadcrumb, pagination, command palette (search over registered commands)   |
  | Data       | table, line and bar chart, icon                                         | data table (sort, filter, select rows, paginate; not virtualised to a million rows), badge, avatar, key-value list, QR code |

  - Themes as tokens: a light and a dark theme ship, following the OS preference where there is one, and a
    theme is a `.nya` style file (Phase 6 makes those user editable). The existing flat, minimal UI defaults
    are the starting point; no drop shadows or outlines unless the theme asks.
  - Overlays share one layer stack with focus trapping and escape to close, so a dialog over a sheet over a
    page closes in the right order, and a pointer never reaches what an overlay covers.
  - Toasts and dialogs are driven by state the caller owns, like `NYA_UIWindowState` today, because an
    immediate mode widget that held its own visibility could never be reopened.
  - Each widget gets a property test against the agent harness: an agent drives it by keyboard and pointer, and
    the widget's state never leaves its valid range.
- `[ ]` `web_frontend`: register, log in with TOTP, list and edit something stored in `http_server`'s database,
  see another browser's edit arrive over WebSocket, upload a file, manage signed-in devices, and, as an admin,
  manage users and edit roles. Built only from the widget set below.
- `[ ]` Hot reload on the web: the dev server pushes a rebuilt module over the WebSocket.

## Showcase-dark bug — FIXED (`ae6df37c`)

Root cause: the sky's `ground` (lower-hemisphere) colour was near-black, and the showcase camera orbits looking DOWN into the valley, so most of the frame sat below the horizon line and showed that near-black ground → a black void past the terrain. `renderer_stress` has the same dark ground but a level camera, so it never showed the bug. Fix: a warm haze `ground` colour close to the fog/horizon. Bisected by forcing the no-post path (still dark → post ruled out) and comparing a full-res `grim -g` capture against renderer_stress's working sky.

## Phase 5 — engine features, continued

The current track, reordered around one missing primitive.

- `[ ]` Compute passes in the renderer. Raymarched volumes, GPU fluids, GPU particles and screen space
  reflections all wait on it ("Fluid volumes" says so); it comes before any of them.
- `[ ]` Reflections: screen space for the scene, planar for still water.
- `[~]` Water as a surface: a heightfield of waves with shoreline foam, refraction through the glass path, and
  the grid solver for what is in the air above it.
  - `[x]` **Flowing river surface (landed `ae19305`)** — `nya_render3d_water`/`_style`, `render_water.{c,h}`,
    `water.{vert,frag}.hlsl`. Dual scrolling-normal flow map + summed-sine/Gerstner waves + Fresnel reflection
    tint + refraction (reuses the glass capture) + shoreline foam; reads the wind field for chop. Example
    `water3d`, test `test_water`, cross-compiles to GLSL ES 300. Follow-ups: true depth-difference foam (needs a
    scene-depth capture beside the colour), planar/SSR reflection (line 1111), an authored normal map.
- `[~]` Weather and sky: one wind field read by particles, fluids, foliage and audio; rain, snow, clouds, stars
  and fog confined to volumes. Kept to the flat stylized look, never photoreal.
  - `[x]` **Wind field + foliage sway (landed `ee21e65`, 2026-09-23)** — the "one missing primitive"; no compute
    pass (analytic CPU field + vertex displacement). `render_wind.{c,h}`: `NYA_WindField` value type,
    `nya_wind_field`/`_set`/`_advance`/`_at`/`_sample`/`_base`; the gust is 3 layered sines proven in [-1,1], no
    grid. Foliage draw path (`nya_render3d_foliage`/`_style`/`_disturb`) reuses the skinned-mesh segment (one draw,
    per-object vertex uniform b1) so a model-space mesh keeps its pivot at y=0; per-vertex flexibility rides vertex
    colour alpha (base 0 → tip 1) so `NYA_Vertex3D` stays 36 bytes. New pipeline `foliage.vert.hlsl` + shared
    `mesh3d.frag` (lit, shadowed), camera-pass only. Grass/leaves/branches are one shader+field, amplitude/
    frequency/stiffness/flutter params. Look: height² cantilever falloff, per-instance phase+amplitude hash,
    two-octave bend, two-axis leaf flutter, normal leaned by the applied displacement. **Physics interaction:**
    `nya_render3d_foliage_disturb(pos,radius,strength)` — up to 16 disturbers (ceiling-registered), nearest 4 per
    plant pushed into the uniform, shader adds a `(1-d/r)²` push-away on top of wind; the example feeds a dynamic
    box3d body's position so a ball parts the grass. `foliage.vert` cross-compiles to GLSL ES 300 (web-ready).
    Example `foliage3d`, test `test_wind.c`. Verified on master: check 0/959, debug build + a foliage3d run under
    ASan+LSan+UBSan shut down clean (no leak — the reported leak did not reproduce). Follow-ups: instanced grass
    for density. `[x]` particles (`nya_particles_wind_set`, `d.../21bf7ec`) and fluids (`nya_fluid_wind_set`, `d3e5521`) now sample the same wind field, so foliage, water, particles and fluids all read one `NYA_WindField`.
- `[x]` **Realistic-but-stylized showcase scene (user, 2026-09-24)** — `examples/showcase` is one cohesive valley
  composing wind + foliage + flowing water + dust/pollen + sky/atmosphere + volumetric light beams (light shafts)
  + aerial-perspective fog, all reading the one `NYA_WindField`, kept inside the flat stylized art style. It is not
  three separate examples but a single world that proves `renderer_stress` + `foliage3d` + `water3d`'s systems
  compose: a tiled LOD heightfield with a carved river, hashed bank foliage bent by the wind and parted by a herd of
  physics balls, wind-ridden pollen, fading ground-decal trails, refractive crystals, cascaded shadows, and the
  cube3d-style post chain (bloom, fog, light shafts, tonemap). Last gap closed (`a8037332`): the light-shaft pass
  only gathers when the sun faces the camera, and the fly-through opened looking away from it, so the shafts never
  drew — the sun now sits over the down-valley end the eye starts turned toward, so the beams greet the opening
  frame. Verified: `NYA_SHOWCASE_FRAMES` headless run shuts down clean under ASan+LSan+UBSan, the light-shaft
  pipeline loads and runs, and a Wayland capture reads bright (foliage, water, balls, trails, crystals all visible).
- `[x]` Our own stereo panner for interaural delay and head shadow. Landed 2026-09-24 (`bd26b494`), opt-in:
  equal-power gains, Woodworth ITD and head shadow, replacing SDL_mixer's positioning on stereo devices. It does
  not remove SDL_mixer (still the decoder, resampler and mixer), so SDL_mixer stays in the vendor list.
- `[~]` Multiplayer: the WebSocket transport, player-key allowlist and immediate version rejection landed
  2026-09-24 (`20360276`). Open: fragmentation of a message across frames, lag compensation at render time, and
  the browser-side client half.

## Phase 6 — customization and desktop

- `[ ]` UI style files: `NYA_UIStyle` from a `.nya` theme through reflection, hot reloaded, validated like
  settings (the file, the key, the value, the range), user editable under `data/`.
- `[~]` Signed plugins: Ed25519 signature over the plugin directory with publisher keys pinned in the program,
  refusing anything not signed by a pinned key, landed 2026-09-24 (`f4c21979`). Open: a signed repository index
  and repositories by URL as under "Plugins".
- `[ ]` VM budgets: an instruction count hook and a heap ceiling through the allocator, so a plugin can be slow
  or large but never hang or exhaust the host.
- `[ ]` The ambient current UI for Lua, runtime asset roots, and the in-app toggles for plugins and systems.

## Phase 7 — hardening and shipping

Most of this is cheap and should be picked up whenever a phase leaves room.

### Resilience (user, 2026-09-24)

- `[x]` **Circuit breaker (`base_circuit`, landed `143ff36`)** — per-key closed→open→half-open, fails fast when a
  dependency is down so retries stop hammering it, one half-open probe heals or re-trips. Wired into the curl
  client beside the rate limiter (`NYA_Request.breaker`, keyed like the limiter; open ⇒ `NYA_ERROR_TIMEOUT`,
  status 0, no socket). Test `test_circuit`. Different question from the limiter (over budget) and the backoff
  (when to retry a call still worth making).
- `[x]` **Idempotency keys in `http` (landed `6b7bab3`)** — `http_idempotency.{c,h}`: `nya_http_layer_idempotency`
  + a fixed 128-entry, TTL-expiring, mutex-guarded store keyed by `Idempotency-Key`, fingerprinted by
  BLAKE2b(method+path+body). Unsafe methods only: no entry ⇒ reserve/run/capture; same key+fingerprint ⇒ replay
  the stored status+body (`Idempotency-Replayed: true`), handler NOT re-run; in-flight ⇒ 409; same key, different
  body ⇒ 422; malformed key ⇒ 400. Added `NYA_HTTP_STATUS_CONFLICT`. Caller: a `web_server` notes route; test
  `test_idempotency` (injected clock) green under ASan+LSan+UBSan.
- `[x]` **Reconnect-with-backoff + health routes (landed `4e36c14`)** — `base_reconnect` (a composable value
  type with an injected clock, delay drawn from `nya_backoff_ms` full jitter; opt-in `NYA_ReconnectPolicy`) wired
  into the curl WS client (new `NYA_WEBSOCKET_STATE_RECONNECTING`, re-dial + re-report OPEN on recovery).
  `http_health`: `GET /healthz` (liveness, always 200) + `GET /readyz` (readiness, 200/503) over a bounded
  registry of named checks, with a `base_circuit` tie-in (OPEN ⇒ not-ready); `web_server` wires a real db check.
  Tests `test_reconnect`, `test_health`.
- `[x]` **Self-healing beyond fail-fast** — `base_supervisor` re-execs the process on a fatal after the crash
  report is written, behind `NYA_SUPERVISE=1` (default off). `nya_supervisor_should_restart` is the pure,
  tested decision — at most N restarts in a rolling window, give up past that so the crash surfaces, reset after
  a quiet window — spacing them with `nya_backoff_ms`. The re-exec is `execve("/proc/self/exe", …)` with the
  restart count carried across in `NYA_SUPERVISE_STATE`, async-signal-safe so it runs from the fault handler
  too. Test `test_supervisor`.
- `[x]` **Callback-backed HTTP route handlers (landed `4a2f5fa`)** — `NYA_HttpRoute.handler_callback` /
  `handler_identified_callback` carry a `nya_callback` token; the router resolves it every dispatch through a
  resolver a program installs with `nya_http_router_resolvers_set`, so a reloaded DLL's handler runs live and a
  raw pointer no longer dangles. Layering-correct: http (rank 6) holds only the opaque `u64`; the program (which
  may use core's callback registry, rank 7) composes the resolution in — one seam, not an HTTP reload mechanism.
  Checker enforces exactly one of pointer or token. `test_router_callback` proves the swap. Follow-up: install a
  `nya_callback_get`-based resolver + convert `web_server`'s routes to demonstrate end-to-end.

### Docs deployment (user, 2026-09-24)

- `[x]` **One deployed docs site** — `./build docs` (the `assemble_docs` rule) stages one deployable tree under
  `./site`: it regenerates the cheatsheet, copies the hand-written GitBook prose and `SUMMARY.md` in beside it,
  writes a `.gitbook.yaml` rooted at the tree, then runs doxygen with `OUTPUT_DIRECTORY = ./site/doxygen`. The
  landing page and `SUMMARY.md` link across to the cheatsheet and the doxygen index, and the generated cheatsheet
  links back to the prose and doxygen — all relative, so the three tiers resolve as one site from the same deploy.
  `./site` and `docs/doxygen/` are gitignored; only the sources (prose, `doxygen.config`, `.gitbook.yaml`,
  `SUMMARY.md`, the build rule) are committed. Follow-up: a CI step that runs `./build docs` and publishes `./site`.

- `[x]` Shipping flags: `_FORTIFY_SOURCE=3`, `-fstack-clash-protection`, full RELRO and `-z now`, NX, checked on
  the produced binary rather than trusted from the flag list. Landed 2026-09-24 (`2c758ee8`, ELF-verified).
- `[ ]` Pinned releases: SDL is at `release-3.4.0-1237`, an untagged commit on main, and Box3D is pre-1.0. Pin
  each vendor to a release tag, or write down beside the submodule why not.
- `[x]` An SBOM and a licence allowlist generated from the vendor rules, and a CVE check against it in CI.
  `./build sbom` (src/build/sbom.c) reads `.gitmodules` and the commit HEAD pins each submodule to, detects
  each dependency's licence from its LICENSE/COPYING file, and writes a CycloneDX 1.5 document plus a human
  summary under `sbom/` (gitignored). The checked-in allowlist is `src/build/vendor/licence_allowlist.h`;
  a detected licence off it — or one that could not be determined — fails the command, so a new copyleft
  or unknown dependency is caught. The CVE step is a hook over osv-scanner: it scans the CycloneDX document
  when osv-scanner is installed and `NYA_SBOM_CVE_SCAN=1` is set (CI), and skips with a notice otherwise
  rather than failing offline. Nothing is vendored and nothing reaches the network by default.
- `[ ]` Scheduled CI: fuzzing from the committed corpus, simulation with random seeds keeping every failure,
  benchmarks on a fixed runner with regressions flagged.
- `[x]` Privacy pass: crash reports strip the home directory, user name and host name before anything leaves
  the machine, the metrics resource binds loopback unless told otherwise, and nothing phones home. Landed
  2026-09-24 (`7e0a794d` scrub in `nya_crash_report_compose`, `80f4e0c6` loopback bind default). Follow-up: a
  confirm-what-will-be-sent view before the report leaves.
- `[ ]` The clang-format gate, as one reformat commit in a quiet window.
- `[ ]` The open items under "Distribution".

## Enterprise level: what it adds

The phases above make each kind of program possible. "Enterprise level" means more: a company could run its
business on a program written on this stack. Swept on 2026-09-22 against that bar. The accepted items are
tagged with the phase they join when that phase begins. The rest were considered and are not planned; they are
kept below so they are not proposed again without a reason.

One boundary shapes all of it, decided the same day: **a server is one machine and one instance.** No
clustering, no replicas, no second database backend. In-process state (rate limit buckets, the permission
cache, live subscriptions, the job queue) never has to be shared with another process, and every design here
may rely on that.

Accepted:

- `[ ]` **Undo and redo**, built on reflection snapshots of the edited state: a bounded history of deltas
  between snapshots, grouped into user-level actions, and the same for a game editor tool or a form. Phase 6.
- `[ ]` **Plural rules and locale formats.** `core_i18n.h` has no plural support today. CLDR plural categories
  per locale, and number, currency, percentage, date and time formats per locale, from tables generated at
  build time by the i18n pass rather than an ICU dependency. Phase 2, beside date and time.
- `[ ]` **Passkeys (WebAuthn)** as a third second factor beside TOTP and PGP, and later as a passwordless
  login. Needs CBOR parsing and ES256 verification (P-256), neither of which monocypher has; mbedTLS, already
  vendored for TLS, has both. The attestation and assertion parsers are fuzzed. Phase 3.
- `[ ]` **Job queue:** persistent in `db`, with retries, exponential backoff, deadlines, unique jobs,
  scheduled (cron style) jobs, and a bounded worker count. Survives a restart: a job running when the process
  died runs again, so jobs are written to be idempotent, and the simulation harness kills the process at
  random points to prove it. The template has `src/tasks/`; here it is a component that server and desktop
  programs share. Phase 3.
- `[ ]` **File uploads and downloads:** streaming bodies and multipart, size limits per route, storage by content
  hash on disk with the metadata in `db`, `Range` requests so downloads resume, the content type decided by
  the server rather than trusted from the client, and served with `Content-Disposition: attachment` unless
  the route says otherwise. Phase 3.
- `[ ]` **Live updates over WebSocket:** publish and subscribe to topics, with presence. A subscription is
  authorised by the same permission check as the route that reads the same data, and re-checked when the
  subscriber's roles change. Messages are DTOs in binary `.nya`. Phase 3.
- `[ ]` **CORS**, configured per route rather than globally, off by default, with the allowed origins listed
  exactly (no wildcard with credentials, ever). Phase 3.
- `[ ]` **Prometheus metrics and OpenTelemetry:** a `/metrics` exposition on a separate loopback port by
  default, fed from what the ceiling, arena and system registries already know plus request counters and
  latency histograms; OpenTelemetry traces exported over OTLP/HTTP, with the request id as the trace id.
  Phase 3.
- `[ ]` **Graceful shutdown:** on SIGTERM or the Windows equivalent, stop accepting, finish in-flight requests
  up to a deadline, let running jobs reach a checkpoint or be requeued, flush logs, close the database. The
  deadline is config, and passing it falls back to the crash path, which already has to be safe. Phase 3.
- `[ ]` **Save and replay versioning:** a save written by an older version loads, through the same `@since`
  attributes and migrations as `db`. Deterministic replays built on the simulation harness: record the seed
  and the inputs, play them back, compare the checksums. Phase 5.
- `[ ]` **Player accessibility:** colour blind palettes (applied through the grading LUT), subtitles and
  captions for positional sound with a direction indicator, remappable everything (bindings exist), controller
  glyphs per pad type, and reduced motion (the speed lines and camera shake off). Phase 5.
- `[ ]` **Reproducible builds:** the same commit gives the same bytes. No timestamps or absolute paths in the
  binary (`-ffile-prefix-map`, `SOURCE_DATE_EPOCH` for the build info), a sorted link order, and pinned
  toolchains. CI builds twice and compares. It is what makes a signed binary checkable by someone else.
  Phase 7.
- `[ ]` **Soak and load tests:** a server under sustained load for hours under ASan and LSan, recording RSS,
  open descriptors and p99 latency; a desktop program and a game left running for a day. Memory or
  descriptors that grow without bound fail the run. A load generator of our own on the HTTP client, so it
  speaks binary `.nya` and the auth flow. Phase 7.

- `[ ]` **Health route:** `/health` answers 200 while the process can serve, and 503 with the reason while it
  cannot: shutting down, the database unreachable, the job queue past its limit. It sits on the metrics port
  (loopback by default), so a supervisor can ask and the internet cannot, and it takes no auth and returns no
  detail beyond the reason. Phase 3.
- `[ ]` **Right-to-left text.** Shaping already goes through SDL_ttf and harfbuzz; what is missing is the bidi
  algorithm (UAX #9) for mixed-direction lines, mirrored layout in the UI (rows, alignment, scroll bars,
  chevrons), and caret movement and selection that follow visual order in the text field. The direction
  comes from the locale, with a per-widget override. The vendored SDL_ttf has no bidi: it offers one
  direction per font (`TTF_SetFontDirection`) and one script (`TTF_SetFontScript`), and no FriBiDi. So UAX #9
  splits a line into runs, and each run is shaped with its own direction. Either SheenBidi (C, Apache 2.0,
  small, passes the Unicode conformance tests) vendored, or our own implementation held to those same tests.
  FriBiDi is LGPL, which is awkward for a static binary. Phase 6.
- `[ ]` **Tray and native dialogs.** SDL's dialog and tray subsystems come back on, behind a `desktop_shell`
  component so a game or server does not pay for them: open, save and folder dialogs with filters, a tray
  icon with a menu, and notifications. On Linux they go through the XDG desktop portal where it exists,
  which is also what makes them work inside Flatpak. Measure what switching them back on adds to the binary
  ("Binary size" recorded what switching them off saved). Phase 6.
- `[ ]` **Database backups.** SQLCipher keeps sqlite's online backup API, so a backup is a page-by-page copy of
  the live database, taken without stopping writers, and it stays encrypted under the same key. The `db`
  component runs it from the job queue on a schedule, writes it next to a checksum through
  `nya_file_write_atomic`, and keeps a bounded number (hourly, daily, weekly). Restore is a CLI command that
  refuses to overwrite a running database. The simulation harness restores a backup taken at a random point,
  opens it and checks its integrity (`PRAGMA integrity_check`), because a backup that has never been restored
  is not one. Copying backups off the machine is the operator's job; the files are encrypted, so any storage
  will do. Phase 3.

Considered, not planned (2026-09-22):

- Screen reader accessibility (UI Automation, AT-SPI). The web frontend still gets it from real DOM elements.
- Heavy business widgets beyond the shadcn-like set: a data grid virtualised to a million rows, tree view,
  rich text, docking.
- Signed automatic updates outside Steam and the packagers.
- IME for CJK input, multiple windows per program, and a readiness route separate from health.
- Organisations (tenants) and SAML single sign-on. OpenID Connect landed 2026-09-23; see Phase 3.
- Generated list conventions (pagination, filtering, sorting).
- Email (SMTP) for verification and resets. Recovery stays with recovery codes and admin resets.
- Budgets per kind of program checked in CI.
- A crash report transport with a nyangine server as the receiver.
- A written threat model per component.
- Tutorials per kind of program.

## Decisions

Each changes what gets built. A recommendation is given; the call is mine.

- `[x]` **TLS in process or behind a proxy.** Decided 2026-09-22: one executable does the backend and serves
  the frontend, so TLS (mbedTLS) and simple rate and connection limits are embedded. A proxy is optional, and
  when one is used it owns what the executable deliberately does not: geo rules, blocklists, a WAF, per-region
  connection logging and heavy abuse handling. Those are not built here.
- `[x]` **Web toolchain.** Decided 2026-09-22: try plain clang to `wasm32` first, with our own `platform/web`.
  Emscripten only if that fails. Probed the same day with the host clang 22.1.8: a C2Y file using `defer`,
  `ext_vector_type` and `-msimd128` compiles with `--target=wasm32 -nostdlib -Wl,--no-entry` to a 321 byte
  module, and node runs it with the right answer (`defer` fired). `wasm-ld` and `wasm-opt` are installed.
  What is missing is a libc: `<string.h>` is not found. Two ways: the wasi-libc sysroot (Arch's `wasi-libc`,
  small, the one wasi-sdk ships) or a freestanding shim of the few calls `base` uses. Try wasi-libc first. The
  first real milestone is `base`, `serde` and `crypto` compiled to wasm with their tests run under node.
- `[x]` **WGSL.** Translated from the SPIR-V the build already produces, never hand written. Tried on
  2026-09-22 with `naga-cli` 30.0.1 over all 43 compiled `.spv`:
  - 15 translate as they are.
  - 22 fail with "Bindings for [N] conflict with other resource". SDL's convention puts a texture and its
    sampler on the same binding (`t0` and `s0` in `space2`), and WebGPU requires distinct slots. Rewriting the
    `Binding` decorations in the SPIR-V (texture `n` → `2n`, sampler `n` → `2n+1`) fixes all 22. The WebGPU
    backend builds its bind groups by the same rule.
  - 6 fail WGSL's uniform alignment. Each cbuffer ends `float x; float3 pad;`: HLSL packs that `float3` at
    offset 4, while WGSL wants a `vec3` at a multiple of 16. Spell the padding as three `float`s. The offsets
    stay the same, so the C structs do not change. The files: `effect_lut`, `effect_output_hdr`,
    `effect_occlusion`, `effect_occlusion_apply`, `effect_light_shafts`, and `mesh3d_outline.vert`, which had
    no source and is now deleted by the shader rule (see Phase 0).
  - With both fixes, 37 of 37 translate and the WGSL parses back through naga's validator, 5.3 kLOC in all.
  - Not verified yet: Chrome compiles WGSL with Tint, which is stricter than naga about derivatives in
    non-uniform control flow. Five outputs use `dpdx`/`dpdy`/`fwidth`. Run them through Tint or a browser
    before calling this done.
  - naga is Rust, so this puts cargo on the shader build machine. That is only Linux, which is already the only
    host that can run DXC, so no new host needs it. It runs at build time only and ships in nothing. Pin it
    like a vendor. The binding rewrite is about 30 lines of C in `src/build/`.
- `[x]` **PGP.** Decided 2026-09-22: vendor a complete library as a `pgp` component, off by
  default, the way sqlite is optional today. It fills the second factor seam by encrypting a one-time code
  to the user's key, which the user decrypts (Phase 3), and it backs PGP-encrypted `.nya` fields. Candidate:
  rnp, Thunderbird's OpenPGP library, which has a C API but needs C++ and Botan. Sequoia
  needs Rust at build time and in the binary. GPGME is ruled out, because it drives a `gpg` binary found at
  run time. Measure rnp's size and cross compile (mingw, sniper) before committing to it. TOTP still lands
  first.
- `[x]` **Encryption at rest**, meaning files on disk that cannot be read without a key. Decided 2026-09-22:
  the whole database is encrypted with SQLCipher, and `.nya` files get encrypted fields (Phase 2). What
  SQLCipher costs and how it fits:
  - It is a fork of sqlite with the same API and the same file format once decrypted, so it replaces
    `vendor/sqlite` instead of sitting beside it: one sqlite in the tree, not two. BSD style licence. Its
    releases trail upstream sqlite by a few weeks; pin to its release tags.
  - It needs a crypto provider for AES-256, HMAC-SHA512 and PBKDF2. The shipped providers are OpenSSL,
    LibTomCrypt, NSS and CommonCrypto. mbedTLS, which Phase 3 vendors for TLS anyway, has all three
    primitives, so a small provider over mbedTLS avoids OpenSSL. Monocypher has no AES, so it cannot be the
    provider. Measure the provider against SQLCipher's own test suite before trusting it.
  - The key comes from the environment or from the user at start, is given once through `sqlite3_key`, never
    reaches a log, a crash report or a config file, and is wiped from memory after use. A missing or wrong key
    stops the program at startup with a message, like any other bad configuration.
  - Cost to measure when it lands: binary size, page read and write throughput against plain sqlite on the
    ORM benches, and cold open time, since key derivation deliberately costs tens of milliseconds.
- `[x]` **What gnyame is for once the seven examples exist.** Decided 2026-09-22: gnyame is where the project
  lives. Engine and program are tightly coupled, so the program is developed in this tree beside the engine, and
  it is the proof that everything composes in one program. Each example proves one kind of program alone and is
  the starting point for a new one. The verification rule becomes: every feature has a caller in gnyame or an
  example, and that caller runs in CI.
- `[x]` **Scaling a server past one machine.** Decided 2026-09-22: it does not. One machine, one instance.
  No standby, no replication, no Postgres. See "Enterprise level".
- `[x]` **How a new program uses nyangine.** Decided 2026-09-22: programs live in this tree for now, beside
  gnyame: one repository, one build, and every engine change tested against every program. Moving a program
  into its own repository with nyangine as a pinned submodule is for later, once the engine stops being clay.
- `[~]` **Where base gets pages, time and files.** Found 2026-09-22 while taking `base` off `platform`. The four
  includes the lint rule counts are the visible part: arenas reserve and commit pages (`platform/memory`), perf and
  logging read clocks, the log's file sink opens and appends to a file, and `base_file`, `base_build` (the build
  framework), `base_integrity` and `base_version` are OS services that happen to live in `base`. The target says
  `base` has no OS, so something has to give. Options:
  - An `os` layer below `base`: raw pages, the two clocks and raw file descriptors, written against libc and the
    syscalls with no dependency on `base` at all. `base` uses it; `platform` keeps the rich API (`NYA_String`,
    `NYA_Error`, walking, commands) on top of both. `base_file`, `base_build`, `base_integrity` and `base_version`
    move up into `platform` or a `tooling` module, since they are services rather than vocabulary.
  - Function pointers installed at startup (a page provider, a clock, a log writer). Keeps `base` pure, but it is
    startup registration, which the style guide avoids, and everything in `base` then depends on an install
    having happened.
  - Accept `platform/memory` and `platform/clock` beside `base` at rank zero, as math already is, and move only
    the services up.
  Recommendation: the first. It is what the other two converge to once the page provider and the clock need a
  home, and a web target then implements `os` once (pages from `memory.grow`, time from the host) instead of
  every module learning about wasm.

  Decided 2026-09-22, and done. `src/nyangine/os/` is rank zero, below `base`: pages, the kernel's random
  source, the two clocks in nanoseconds and the sleep, file handles with directory iteration and the paths the
  host names itself, and process spawning with its pipes. Nothing in it may include anything above
  `base_types.h`, `base_attributes.h` and `base_basic.h` — the prelude, which the lint rule exempts because it
  declares no function — so nothing there asserts: an `os` call reports failure and the layer above decides what
  it means.

  Everything `base` was reaching up for came down with it: `nya_filesystem_*` and `NYA_File` are
  `base/base_filesystem.c`, `nya_command_*` is `base/base_command.c`, and `nya_clock_*`, `nya_instant_*` and the
  RFC 9110 formatting are `base/base_clock*.c`, all pure arithmetic over `os`. The `base -> platform` allowance
  is gone rather than lowered. `platform` keeps what is OS-facing but wants engine types and is not `base`'s
  business: signals, the terminal, ipc and the host's own description.

  What it bought, measured: the file system was 1208 lines written twice and is 792 of syscalls plus one
  implementation; commands were 713 lines written twice and are 276 written once over 523 of syscalls; the clock
  was seven functions twice and is seven functions over two `os` calls. Two drains that had already drifted apart
  (one polled, one spun with a sleep) are now one.

- `[x]` **`nn` in the engine.** Decided 2026-09-22: it stays a library module, usable by any program, and
  it is also a testing mechanism: a DQN or NEAT agent plays a UI or a game through the input queue the way a
  person would (`testing_agent.h`). It extends to every example that has a UI, not only gnyame: the TUI and
  the web frontend are UIs an agent can play too. Its drawing still moves out of `nn` (Phase 1), so the
  module depends on nothing above `math`.
- `[x]` **Lambdas through a preprocessor pass.** Asked 2026-09-22, and built the same day: the recommendation
  below was "not yet", and the answer was to do it anyway. C has no function literals, and every callback here
  (`nya_callback`, event hooks, systems, comparators) was a named function somewhere else in the file.
  - Landed: `nya_lambda(tag, ReturnType, (params), { body })`, the macro in `base_lambda.h` and the pass in
    `src/build/pp/lambda.c`, hooked as `generate_lambdas`. The body is hoisted into a companion header per
    source file under `src/genyarated/lambdas/`, which that source includes itself, so the body compiles inside
    the file it was written in and can name that file's statics. `#line` keeps the diagnostics on the line
    somebody typed. `manifest.txt` is the watermark `nya_pp_is_current` reads, since the companion set is
    dynamic; stale companions are pruned. Nothing is written unless every tree parses, so a malformed body
    cannot leave half a generated file behind.
  - An explicit tag rather than a name built from `__LINE__`: a line-keyed name is renamed by any edit above
    it, churns every hoisted body below it in a diff, and reads as `_nya_lambda_l217` in a stack trace. It is
    also what makes the hot reload catch below a non-issue, since `nya_callback` resolves by name.
  - Captures are still not possible, and that is the safety property: the hoisted function is at file scope, so
    naming a local is an ordinary compile error rather than a lifetime bug. A struct through the `void*
    user_data` every callback already takes is the honest spelling of a capture.
  - The caller: `src/gnyame/layers/layer_pause_menu.c`, whose locale row passed a callback declared at the top
    and defined 280 lines below.
  - Known and documented rather than papered over: a lambda inside an `#if` arm is hoisted anyway, because the
    pass reads text and not preprocessor state, and then trips `-Werror` as an unused function. The parser
    itself has no unit test, since the pass lives in the build tool and the test binaries do not link it; its
    error paths were exercised by running it against deliberately malformed input.


# The stack

What "one stack for everything" adds on top of the engine. Nothing here exists yet unless it says so.

## `[~]` Web

In scope, deliberately: not only a server, but the client too.

- `[x]` An HTTP server in the engine: `src/nyangine/http/`, off until `nya_system_http_init` and drained once a
  frame on the event input arrives on. A router per resource over a `static const` route table, an onion of
  layers, the identity extractor as a precondition rather than a layer, and DTOs as the only thing crossing
  the wire, in and out through their reflections. One GET or POST per path, matched exactly: a query parameter
  picks between instances of one shape, and two shapes are two paths. Every bound is in `http_types.h` with its
  size argued, the request parser is fuzzed from a committed corpus, and rate limits and TLS stay with a proxy
  in front. See `docs/http.md`. That was reversed on 2026-09-22 (Roadmap, Phase 3): both move in process.
- `[x]` OpenAPI generated from the handler definitions and the DTO types, served by the app at `/openapi.json`,
  with `/docs` as a page generated from the same walk. Nothing is stored and nothing is hand written: unmount a
  resource and it leaves the document. A debug build asserts a route never answers with a status it did not
  declare, so the schema cannot drift from the code.
- `[~]` Auth: JWT over HMAC-SHA256 is real, with the signature checked before the payload is parsed and `alg`
  compared whole. The PGP second factor is half done: the challenge is real and stateless, the signature check
  is a seam (`nya_http_second_factor_set`) and a route that needs one with no verifier installed answers 501.
  An OpenPGP parser is its own piece of work and does not belong inside an HTTP server.
- `[⏭]` Vendoring an OpenPGP library: deferred on purpose (2026-09-22), not forgotten. There is no small C one,
  because a complete implementation needs its own bignum and RSA stack. The three real answers each cost
  something structural to a build where every vendor is C, static and cross-compiled to mingw and the sniper
  sysroot: Sequoia makes a Rust toolchain a build requirement everywhere, rnp pulls in C++ and Botan, and an
  Ed25519-only parser of our own over the vendored monocypher refuses every RSA key. Decide which users have
  before paying any of them. Decided later the same day: a complete library as an opt-in plugin, so only a
  program that enables it pays the cost. See "Roadmap", "Decisions".
- `[⏭]` TOTP as the second factor instead: also deferred. Not a wiring job — `nya_hmac_sha256` exists but TOTP
  wants HMAC-SHA1 and base32, neither of which does. About 200 lines, and RFC 6238 publishes test vectors, so
  it would be provable rather than merely written.
- `[ ]` A login route. There is no way to _get_ a token over HTTP yet, only to present one; tokens are minted
  in-process with `nya_http_jwt_encode`. That wants a user store, which nothing here has.
- `[ ]` Compile to web: a bundle of HTML, CSS, JS and wasm. WebGPU where it exists, a canvas backend otherwise.
- `[ ]` A UI backend that emits HTML, CSS and JS from the same `nya_ui_*` calls the native backend draws, ahead
  of time or on the fly. **I never write HTML, CSS or JS by hand.** That is the whole point of the exercise.
- `[ ]` Fully client side apps that talk to a nyangine server, with the types shared between the two rather than
  restated.
- `[ ]` Hot reloading on the web, matching what the native builds already do.

## `[~]` TUI

- `[x]` A terminal backend beside the GPU one, picked the way the headless one is: `-DNYA_TERMINAL` makes
  `nyangine.c` compile `renderer/render2d_terminal.c` in place of `render2d.c`, and implies `NYA_HEADLESS`
  because that is what it is. `platform/terminal/` is the device under it: raw termios, a fixed cell grid with
  damage tracking, ANSI out, an escape decoder in, SIGWINCH. **Not ncurses**: a permanent dependency that also
  needs a terminfo database at runtime, owns a global `SCREEN` and its own refresh, models colour as pairs, and
  cannot carry the kitty escape unmangled. The reasoning is written in `terminal.h` where it would be
  reintroduced.
- `[x]` Kitty image protocol, chunked as the protocol requires, degrading to drawing nothing where it is
  unsupported. The probe decides by terminal name, because the query form needs a round trip at startup that a
  terminal ignoring it would leave the program waiting on.
- `[x]` Colour degrades truecolor → 256 → 16 → none, decided by one probe at open that logs what was lost.
  Cells hold 24-bit whatever the terminal can show; only the present quantises.
- `[x]` Keys and SGR mouse reports arrive through `nya_event_dispatch`, the same path the SDL backend uses, so
  `nya_input_*` and every `nya_ui_*` widget work without knowing which backend is under them.
- `[x]` `examples/tui_dashboard` is the caller, and it is built out of `nya_ui_*` widgets: selectable arena
  rows, a dropdown, a toggle, buttons and a folding section, driven by tab, the arrows, enter and escape.
- `[x]` A TUI stands up the callback, event and input systems and no more. Two things it has to do itself:
  dispatch `NYA_EVENT_UPDATING_ENDED` once a frame, or the input system never rolls its just-pressed edges, and
  size everything in whole cells, or a size lands between two and the cell it rounds to is nobody's choice.
- `[x]` There is a sorted cell buffer now. Each cell remembers the layer it was last written at and a write
  wins only at or above it, so `nya_render2d_layer_set` decides instead of call order and a raised panel is
  on top. Two choke points, the fill and the glyph.
- `[x]` The backend is testable at all, which it was not: every test links one shared engine built without
  `NYA_TERMINAL`, so a test under `tests/nyangine/terminal/` builds its own against the terminal backend,
  and `NYA_TerminalOptions.detached` opens the grid with no tty. Off by default — a TUI for a person should
  still fail loudly rather than draw into a pipe.
- `[~]` The Windows half (`terminal_windows.c`) is written against the console's virtual terminal modes. It
  had never been *compiled* either — every target picks the GPU renderer, and the one unit that compiles the
  terminal backend is built for the host. `./build build terminal-windows` cross compiles it now, and the
  object really contains it rather than the Linux half. It still has never *run*; that needs a windows
  machine.
- `[x]` Textures by asset handle draw. The pixels are the backend's to read after all: a terminal build makes
  no GPU device, so the loader keeps the decoded RGBA8 rather than failing to upload it, and the backend
  samples one texel per cell from the centre. Point sampled, not averaged — a cell is about 10 by 18 pixels
  and an average lands every cell near the mean of the image. Rotation and flipping are dropped; a cell grid
  cannot turn a picture without resampling it into noise. `nya_render2d_terminal_image` is still the way to
  put a real picture up where kitty graphics exist.

## `[~]` IPC and talking to other programs

- `[x]` A local control socket, opt-in and off by default: `platform/ipc/` is a unix socket on Linux and a
  named pipe on Windows, and `core_control.c` is the surface an outside program drives the engine through.
  Every inbound byte is parsed at the boundary and the decoders are fuzzed (`test_fuzz_control.c`).
- `[x]` An outgoing WebSocket client over ws and wss (`plugins/curl/websocket.h`), framing, ping/pong and
  close, fuzzed. `[ ]` Nothing drives OBS with it yet, which is the point of having it.
- `[x]` An outgoing REST client exists (`plugins/curl/request.h`) but nothing calls it.
- `[x]` An HTTP server with OpenAPI generated from the handlers; see "Web" above. Its first resource is this
  program's own metrics, which `gnyame` serves when `GNYAME_WEB_PORT` names a port.

---

# Requested

Everything asked for that is not already covered by a section below. Android is explicitly out of scope and is
not listed.

## `[~]` Plugins

The model to copy is Dalamud's: a list of plugin repositories the user can add by URL, each serving a JSON
index, with per-plugin enable and disable, download counts, and a blunt warning that a plugin is arbitrary code
and can do anything the program can.

- `[x]` Lua plugins loaded from `plugins/<name>/` with `manifest.nya`, `main.lua`, `src/` and `assets/`.
  The manifest carries author, license, plugin version, engine version, dependencies, conflicts, and the
  repository URL it updates itself from. `core_plugin.h`; parsed through the reflection tables, so the struct
  is the schema and there is no second copy of it to drift. `plugins/hello/` is the working example, and
  gnyame loads it at startup.
- `[ ]` Custom plugin repositories: add a URL, it serves an index, plugins show up as installable.
- `[ ]` A plugin repo of our own, holding plugins as submodules, listed in-game as downloadable.
- `[ ]` Steam Workshop plugins. A plugin may be nothing more than a map or a character added to the selection.
- `[~]` Parity between the Lua plugin API and what can be written in C. The mechanism is there — annotate a
  declaration `@lua(PERMISSION)` and it is bound on the next build — and thirty-one calls are exposed. What is
  not bound is what the generator refuses (`luabind.h` lists each reason) and everything nobody has annotated
  yet. The UI is the gap that matters: every `nya_ui_*` call takes the `NYA_UI*` that `nya_ui_begin` returns,
  and a pointer cannot cross into a script, so a plugin's `on_render` needs the host to open the pass and an
  ambient current UI. That is a design decision, not a missing binding.
- `[x]` Lua bindings **autogenerated**. `src/build/pp/luabind.c` writes `src/genyarated/lua_bindings.c` and
  `docs/lua/nya.lua`; `.luarc.json` points a language server at the second, so `nya` is no longer an undefined
  global. Seven hand written bindings are left, each saying why it cannot be generated.
- `[x]` Namespacing that survives a large plugin collection. Three layers, all structural: a VM per plugin, so
  two plugins may both define `spawn` and never meet; `nya_plugin_qualify` prefixes everything a plugin
  registers with its own name; and the directory is the identity, so a manifest naming itself something else
  is refused.
- `[x]` Per-plugin tracing: each plugin is one system registry entry owned by itself, reporting its VM's heap
  through `NYA_SystemEntry.memory_bytes`, so the overlay's owner table already answers it.
- `[x]` Plugin errors reported to the plugin's own developer, and visibly distinct from engine errors. The
  line is `[plugin] <name> <version> by <author>: ...` followed by the repository from the manifest, and a
  plugin that fails `NYA_PLUGIN_ERROR_MAX` times is switched off rather than allowed to fill the log.
- `[x]` A permission system the game fixes **once, at compile time**: `-DNYA_PLUGIN_PERMISSION_PROFILE`, four
  profiles, refused at load with a line naming each permission the manifest asked for and did not get.
  Enforcement is which bindings exist in the VM: a denied call is a name that was never registered, not a call
  that refuses. Read PERMISSIONS in `core_plugin.h` for what that does _not_ guarantee — no instruction
  budget, no heap ceiling, no signature, permission granularity rather than object granularity.
- `[~]` Users can enable and disable engine systems and load their own assets. `nya_plugin_enable`/`_disable`
  and `nya_system_enable`/`_disable` are there; nothing in the UI drives them, and a plugin's `assets/` is not
  indexed by the asset system, which would need a runtime asset root.

## `[ ]` Distribution

Modes, restated so they stop drifting:

| Mode      | Meaning                                                 |
| :-------- | :------------------------------------------------------ |
| `debug`   | Something is wrong in the code; find it. Sanitizers on. |
| `dev`     | Ordinary development. Sanitizers off.                   |
| `release` | What users get. Optimized.                              |

`debug` and `dev` both use filesystem assets with hot reload, and both produce perf data. `release` has
submodes: plain release is native, `steam` is release through Steam, and flatpak, pacman, AUR, scoop and nix are
the packager ones.

- `[ ]` `dist/` with a folder per target: linux, windows, steam-linux, steam-windows, linux-pacman,
  linux-nixos, web, plus the Lua bindings API. **Binary releases only — a user never compiles from source.**
- `[ ]` A distribution contains: the executable; packager-specific files (manifests, desktop entry, Steam
  library, man page, licence); an optional `assets/` if not bundled into the executable; a `data/` folder
  holding user-editable settings and colour theme plus non-editable save data; and `plugins/`.
- `[ ]` Assets on the filesystem are encrypted or obfuscated so they cannot be extracted or modified.
- `[x]` Settings are user-editable and a bad line explains itself: the file, the key, what was found and what
  was expected. The half that said nothing was a value the type checker has no quarrel with — a volume of 5,
  seven MSAA samples, a field of view of 200 — which its setter corrected in silence. Both loaders compare
  what was asked for against what was kept and name the key, the value and the range. Read back from the
  setter rather than against limits written out again, so there is one place that decides.
  Save data is the opposite and already has its integrity check: `NYA_SAVE_FLAGS_DATA` writes obfuscated
  and reads with the checksum enforced, so a file altered outside the game is refused with
  `NYA_ERROR_CORRUPT` rather than half loaded. `test_scene.c` flips one byte in the body and holds it to
  that, including that the world it was going to load into is left alone.
- `[x]` `CHANGELOG.md`, generated from history, shipped with every release.
- `[x]` `secrets/` committed to GitHub, encrypted with sops and gpg, holding the signing key among other things.
- `[ ]` CI/CD produces every release build so Steam and the packagers can pick up a new version.

## `[ ]` Reported bugs

- `[x]` Resizing the window broke the UI and the fonts. `_nya_ui_scale_derive` derived the scale from the
  window's height, `_nya_ui_look_build` baked fonts at `size * scale`, and the glyph atlas was keyed
  `path@points` with capacity 8 and eviction `REFUSE`, so each scale step minted a key until all 8 were gone
  and text drew blank. On a HiDPI display that happened at startup with no resize: 6813 "no free glyph atlas
  slot" warnings in a 25 second run, for `Aldrich.ttf@19` and `@16`. Fixed three ways: the scale is 1 unless a
  player sets one and never follows the window, the cache evicts least recently used after flushing the batch
  that may still name the texture, and a refusal reports once per handle. The title screen now bakes two
  atlases where it baked seven, and warns nothing.
- `[x]` The UI must not autoscale with screen size. `NYA_UIStyle.scale` is the only thing that moves it, with
  `follow_display_scale` as an opt-in for HiDPI.
- `[x]` Log spam, including things logged as errors that are not. Two causes, both gone: the atlas warning
  above logged per frame rather than per handle, and `nya_app_init_with_options` logged "No supported
  SDL_GPU backend found" as an ERROR where it was the expected state. A subsystem is now `optional` and an
  unavailable one is reported at debug level, which also means a dedicated server starts on a box with no
  GPU at all — it could not before. A 20 second run of the title screen now logs one line total, the
  Wayland icon warning, once per process as it should.
- `[x]` Shadows moved laggily. The light basis snapped elevation and azimuth to 0.5° steps, which halved
  the pixels changing per frame by freezing most of them: over 240 frames at 60 fps with the two minute
  day, 197 of 239 frames were frozen and the worst single frame jumped 4.678 texels. Following the sun
  exactly: 0 frozen frames, worst jump 1.481.
- `[x]` The fire flickered. A draw went to the opaque stream whenever its colour was fully opaque, and a
  particle is born at exactly alpha one, so for its first tick every flame particle drew through the
  opaque pipeline: depth written, no addition, a solid square punched through the plume. Blend mode is
  explicit caller intent and alpha is a heuristic, so additive now never counts as opaque.
- `[x]` Both flaky tests are fixed, and fixed in their shape rather than by widening a number until it
  stopped failing. Neither depends on the wall clock any more.
  - `test_robots` asserted movement in a single tick. `brain_fitness > 0` says a genome flies toward the
    player eventually; it does not say it thrusts on any particular tick, and which genome wins depends on
    how many generations the training job got through, which under a loaded parallel run is not the same
    number twice. It now watches a horizon of `GNY_ROBOT_MOVE_HORIZON_TICKS` and asks whether any drone
    moved, which is what the fitness actually promises.
  - `test_trace` needed a tight upper bound on a wall clock measurement — 2 ms of spin had to read under
    2.9 — which a sanitized build on a loaded machine oversleeps straight through. The child now spins five
    times the parent instead of half, so attribution separates the two by the whole of the child's spin
    whatever the machine does.

  Measured rather than assumed: three full parallel suite runs, both green in all three, 230 of 230 each
  time.
- `[x]` `test_attack` failed once in a full suite run on 2026-09-22, at "an over-length fragment inside a
  legal sealed datagram is refused": the server held no peer afterwards. Two suites on one machine were
  crossing ports — a parallel checkout saw the same failure while another `test_attack` held UDP port 48100.
  Fixed in its shape: every net and http test binds port zero and reads back what the kernel gave, and every
  `FIRST_PORT`/`LAST_PORT` scan is gone from the tree. That needed the transport to be able to say what it
  bound (`nya_net_transport_port`, `nya_net_server_port`) and `net_port.c` to ask the OS for one, since
  SDL_net has no call that reports it — the one place in the engine that opens a socket without SDL_net, and
  it says why. `examples/net_echo` connects its client to the port the server was given, and gnyame's HUD
  shows what was bound rather than what was asked for. Proven with 12 consecutive net suite runs and 8
  concurrent server and client pairs on 8 distinct ports.
- `[x]` `test_transport` went red on test-windows with "80 ms of added round trip measured as 257.1 ms" against
  a bound of 250. The bound was the bug: the drain loop sleeps between polls, a sleep rounds up to the host's
  scheduler tick (about a millisecond here and fifteen on Windows), and under that link most datagrams are
  lost, the survivors queue behind sixty sends, and the estimate is an average still climbing. It reads 52 ms
  on a loaded machine and 180 on an idle one for the same 80 imposed. A baseline measured with nothing imposed
  did not help either: the estimate is timed off the ack fields a datagram carries, so a phase where only one
  side sends never samples anything. What the case asserts now is what holds on any host — an estimate cannot
  exceed the exchange it was measured in. That latency is applied at all is the case above it, which times one
  datagram against the wall clock.
- `[x]` RenderDoc closed immediately because its Vulkan layer has no Wayland support: SDL cannot build an
  instance that can make a surface, its Vulkan backend reports itself unsupported, and the renderer
  subsystem fails at startup. Not the anti-tamper check, not the validation layers (the release build fails
  the same way), and not process injection (the capture layer alone reproduces it). `SDL_VIDEO_DRIVER=x11`,
  XWayland here, captures fine. Nothing to fix in the engine, so the error says all of that now.
- `[x]` `monocypher.h` not found, `NYA_LuaVM` unknown, `windows.h` not found, and the
  `modernize-redundant-void-arg` lint were all `.clangd` gaps. Fixed.
- `[x]` `build-steam-linux` red in CI: monocypher was missing from the steamrt vendor set. Fixed.
- `[x]` `test-windows` red on `test_replica`: a 624 KB `NYA_NetReplicaMap` on a 1 MB Windows stack. The first
  fix moved the local to the arena and stayed red, because `*map = (NYA_NetReplicaMap){ 0 }` builds the same
  624 KB as a temporary when unoptimized, and so did `nya_net_replica_map_clear`, `_despawn_all` and
  `nya_net_client_attach` in the engine itself. Those zero with a memset now, and every build compiles with
  `-Wframe-larger-than=262144`, so a frame that only Windows would overflow fails on Linux first.

## `[~]` Crash reporting

- `[x]` On an assertion, a window showing the assertion and the log leading up to it, with Close, Copy and
  Send to developer. It is an SDL window of its own using the 2D renderer and SDL's built in font, not the
  engine UI: the thing most likely to have crashed is the GPU device or the code feeding it, and a reporter
  that needs six subsystems alive cannot report the crash that took one of them down. See `debug_crash.h`.
- `[x]` The report carries build info, platform info (CPU, RAM, VRAM), the log ring and the error with its
  stack. Build metadata comes from `nya_build_info` so the report, the menu corner and the log agree.
- `[x]` Send to developer writes a file under the log directory and names the path.
- `[x]` The report says what the variables held, asked for 2026-09-22 and built the same day, in two tiers.
  `nya_assert_eq/_ne/_lt/_le/_gt/_ge` print both operands rather than only the expression text, through one
  `_Generic` map over every width up to `u128`, the floats, `bool`, `char`, C strings and `NYA_String*`, with
  each side captured once so an operand with a side effect is evaluated once. A function marked `// @watch`
  with `nya_watch(name)` below its declarations gets generated code registering its locals — name, type and a
  pointer — into a 64 entry thread local ring that the report walks innermost first; `defer` unregisters on
  every early return, so a pointer into a dead frame is never read, and `nya_expect_crash`'s `longjmp`, which
  runs no defer, saves and restores the mark by hand. The walk allocates nothing and takes no lock, because it
  runs where the heap may already be broken.
  - Not done, and the header says why: DWARF locals. A location is an expression evaluated against a per frame
    register context, which needs the unwinder to hand back registers and a small stack machine, and a release
    build has largely dropped the locations anyway. The opt in ring is what pays without that.
  - The pass reads plain declarators only. Arrays, several declarators in one statement, function pointers and
    locals in an inner block are written into the generated companion as "not watched", so the omission shows
    up in review rather than silently.
- `[ ]` A transport behind `nya_crash_report_submit`. Deliberately deferred: no endpoint chosen yet.
- `[ ]` A window on the hardware fault path. A fault arrives in a signal handler on the faulting thread, and
  calling SDL from there can deadlock against a lock that thread already holds, so a fault writes the file
  and names it on stderr instead.

## `[~]` UI

- `[x]` Not two files. `ui.c` and `ui.h` became `ui.c` (lifetime and the one static state), `ui_layout.c`,
  `ui_style.c`, `ui_input.c`, `ui_draw.c`, `ui_widgets.c`, `ui_text.c` and `ui_internal.h`.
- `[x]` Text input editing: selection, shift+arrows, ctrl+word, ctrl+A, copy, cut, paste, ctrl+backspace,
  double click word and drag select. The header's old "selection and clipboard rejected" rationale is gone.
- `[x]` Dropdowns, radio buttons, tabs, draggable panels, tables, line and bar charts, icons, subtree
  opacity, click bounce, horizontal scrolling and clipping.
- `[x]` The dropdown floats. It hangs under its row over whatever follows, takes no room, is measured into
  nothing and is cut by the window rather than by the panel holding it. The part that looked hard, keeping the
  pointer off the widgets it covers, is not the panel stack's job: a float is declared before everything it
  covers, so it claims its rectangle as it closes and what comes after refuses a pointer inside one. Exact and
  forward only, which is why panels still occlude from where they were last laid out instead.
- `[x]` Windows: a title bar with a hamburger, a collapse chevron and a close X, dragged by the bar and resized
  by a corner grip, plus `nya_ui_section_begin` for folding part of a panel away. The caller keeps the
  `NYA_UIWindowState`, because a widget that held its own visibility could never be shown again.
- `[x]` Tab and shift-tab step through every widget whatever line it is on, and focus entering a top level
  panel raises it. Up and down alone could not reach a UI that is all rows, and a terminal has no hover.
- `[ ]` Icons exist in the engine but nothing in gnyame draws one: the menu sheet has no icon regions, and
  inventing some was not worth it. Compiled, not run.
- `[ ]` A node editor.
- `[ ]` SVG buttons.
- `[ ]` A large text editor widget for writing code in-game, with treesitter syntax highlighting. Needed for
  in-game scripting, and again for the ruey rewrite.
- `[ ]` Transitions between screens.

## `[ ]` Renderer

- `[x]` A flag for every feature: 37 switches per window in `render_features.h`, fed from
  `engine.renderer.features` and hot reloaded. Frustum culling, backface culling, draw sorting and the depth
  test had no toggle before. Tri-state rather than `b8`, so a zeroed struct overrides nothing and no name
  carries a negation. Nothing in it is backend specific, which is what a terminal or web backend needs.
- `[ ]` Reflections still do not exist; the switch is there and answers off.
- `[~]` 2D and 3D fluids, Navier-Stokes. In progress.
- `[ ]` Better 2D and 3D skyboxes. Fog in specific regions rather than only globally, rain, clouds, stars.
- `[x]` Placeholders for missing assets. Magenta with a black cross, drawn with the shape pipeline rather
  than a generated texture, so it needs no GPU resource and cannot itself fail to load. `nya_asset_is_missing`
  keeps an asset that is merely still LOADING out of it, or every load would flash, and
  `nya_asset_missing_report` warns once per handle rather than once per frame. A missing model gets the
  same treatment: an outlined magenta box at the caller's own scale and transform, outlined rather than
  solid so a missing prop does not hide the scene behind it too. Not reachable from a test, since
  `render3d_headless.c` is a stub; verified by pointing the demo's model handle at a file that is not
  there and watching it warn once and carry on.
- `[ ]` Character customization: recolour, retexture and paint a loaded default model. Clothes and hair later.

## `[ ]` Engine

- `[x]` One system registry for engine and game alike. `core_app.c`'s hardcoded per-frame call lists are gone:
  frame, tick and render phases run from the registry in the same order they ran in before. Systems register,
  unregister, enable and disable at runtime, and a mutation issued from inside a running phase queues and
  applies when that run ends. Every entry carries an owner (engine, game or a named plugin) with per-owner
  time and memory, which is what the plugin system will key off. The overlay's systems page toggles one by
  hand, so `physics2d` off is a freeze frame with everything else still running.
- `[x]` Registry entries are callback handles, not raw function pointers, so a system registered from the
  hot-reloaded game library runs the generation that is loaded rather than the one that registered it. The
  three names are copied into the registry too, since a string literal also lives in the image being
  replaced. The callback registry stopped being a system of its own for this: every entry resolves through
  it, so it comes up before the registrations and goes down after the last `deinit`. `NYA_INTERNAL_CALLBACK`
  is internal where nothing reloads and visible where something does, because dlsym finds neither a static
  nor a hidden symbol. `test_system_reload.c` replaces a callback the way main.c does after a reload and
  checks the registry runs the new one.
- `[x]` Fast-forward: a session installs a clock it steps by exactly one tick per frame and never sleeps
  against, so the loop books one tick, runs it and comes straight back. `test_session.c` plays one seed
  fast-forwarded and again paced to the wall clock and compares the digests: 120 ticks in 2.4 ms against
  1955 ms, same number. `real_time` exists for that comparison and for nothing else. Three latent bugs were
  in the way; see the Findings entry.
- `[x]` DQN and NEAT driving the real application as a user. `testing_agent.h` fills the session's policy
  seam with a network, `gnyame/agent.c` says what the agent may press and what it can see, and
  `./build run agent --kind random|dqn|neat` trains one. The agent presses keys and moves the mouse through
  `nya_event_dispatch`, so it reaches window handling, the input system and every layer exactly as a player
  does. `./build run test` plays a short seeded run with each kind. `[ ]` The scenes are stubbed in that
  runner, because building one needs a GPU device a headless run has not got, so what the agent drives today
  is the title screen, the pause menu and the screen stack. The scenes themselves are the next step.
- `[x]` Scene and settings persistence (`core_scene.h`, reflection driven), for save files and the editor.
- `[x]` Collision layers, named, for both solvers, through Box2D's and Box3D's own filters rather than a
  callback.
- `[x]` Reflection-driven parsing to and from objects and the `nya` format, with the engine's own types
  described too (`reflection_engine.c`), used by scenes and settings rather than only by config.
- `[ ]` Reflection data given at least token protection against reverse engineering. Today every struct and
  field name sits in `.rodata` verbatim.
- `[ ]` Use the config system more, starting with gnyame's `constants.h`.
- `[~]` A better debug UI. A systems page landed; the rest is open.
- `[x]` The main menu shows build kind, commit hash, build time and version in the bottom left corner, from
  `nya_build_info`, which the crash report and the startup log read too so they cannot disagree.

## `[ ]` Steam

- `[x]` Lobbies, peer to peer, achievements, stats and Cloud. `net_steam.c` is a real transport now.
- `[x]` It is tested against a fake now, which it was not when this said so: `test_steam.c` was written
  today. `[ ]` None of it has been exercised against a running Steam client.
- Note: `plugins/steam/steam.c` **is** compiled and linked for the steam targets. The "Steam is dead code"
  section further down predates that and is stale.

## `[ ]` Discord

- `[x]` Rich presence wired to real game state, through the `core_social.h` facade over Discord and Steam.
- `[x]` Invites: `ACTIVITY_JOIN` and `ACTIVITY_JOIN_REQUEST` arrive as engine events, and a join secret is
  the launch config on the wire. `[ ]` Untested against a running Discord client.

## `[ ]` Testing

- `[x]` Fuzzing through AFL++, `./build run fuzz <target>`, corpus committed. AFL++ is probed, not vendored:
  without it every target still replays its corpus through `./build run test`.
- `[x]` Deterministic simulation testing: `src/nyangine/testing/`, atomic actions and faults composed from
  one seed, simulated clock, assertions as the oracle, failing seeds committed in `simulation_seeds.txt`.
  Every draw is `siphash(seed, step, draw_index)` rather than a stateful generator, so adding a draw inside
  one action does not invalidate every seed recorded before it.
- `[x]` Property tests with a shrinker (`testing_property.h`), covering the round trips.
- `[x]` Played sessions: `testing_session.h` drives the real application headless through the input queue,
  with no wall clock wait, and `testing_agent.h` puts a DQN or a NEAT population behind the choice of what
  to press. `./build run agent`, and a short run of each kind in `./build run test`.

## `[ ]` Docs and examples

- `[x]` `docs/CHEATSHEET.md`, generated from the headers by `src/build/pp/cheatsheet.c`, and it regenerates
  now. It had drifted because `generate_cheatsheet` was declared in `src/build/asset_rules.h`, a file the
  build reorganisation had already folded into `pp/pp.h` and orphaned: nothing included it, so the rule was
  never dispatched and the reference was stale from the day it landed. Moved into `pp/pp.h`, the orphan
  deleted, and regenerating added 523 lines.
- `[x]` A GitBook site under `docs/` for the prose, with `.gitbook.yaml` rooting it and `SUMMARY.md` as the
  navigation. Three tiers that answer different questions: these pages for why and how, the cheatsheet for
  signatures, doxygen for what the code does. Only the first is hand written, so the landing page says the
  header wins where they disagree.
- `[ ]` More tutorials. Three exist (first window, drawing a UI, adding a system); nothing covers
  networking, physics, audio, plugins, scenes or the testing harnesses.
- `[x]` `AGENTS.md` at the root pointing at the cheatsheet.
- `[~]` More examples beside hello_world. Landed: `cli_tool` (no window), `plugin_scripting` (the Lua
  surface as it is today), `tui_dashboard` (a real TUI on the terminal backend), `net_echo`
  (server and client over the UDP transport), `pong_multiplayer` (one authority, predicted paddles,
  interpolated replicas) and `pinball3d` (3D physics with joints, impulses and collision events; the
  flippers are driven by their velocity so the sweep throws the ball, see "A teleported body has no
  speed to give away").
  `[x]` `web_server` — an HTTP resource with the four verbs, `/docs` and `/openapi.json` generated from
  the route table, and `application/nya` negotiated against JSON. Gives `src/nyangine/http` its first
  caller outside tests.
  `[ ]` Still to do: the client half of that web app, which needs the web target.
- `[x]` The 3D demo's graphics menu. It gets no settings menu of its own: the pause menu's graphics panel owns
  `NYA_SettingsGraphics` and escape reaches it from the 3D scene now, and a second panel over the same values
  would be two places that can disagree. What it got instead is the switchboard nothing else drove: `0` opens a
  panel with one row per `NYA_RenderFeature`, cycling auto/on/off into `engine.renderer.features`, which
  `gny_config_renderer_apply` already feeds to `nya_render_features_set` every frame. Until now only a config
  file edit reached those 28 switches.

---

# Open

## `[~]` Stylized renderer

The goal is a stylized, flat coloured look, not a cartoon. Defaults stay restrained: faceted flat colour with
smooth wrapped lighting (materials at high roughness fade the bands into a gradient), a cool tint in shade,
soft ambient occlusion (`softness` spans the band into a gradient), a gentle grade (`vivid.cube` at 0.4),
bloom on lights only, scuff marks and blob shadows as the only decals. Ink outlines, banded shading, speed
lines, tilt shift and the rest stay as options.

Every feature is an options struct set per window, fed from `engine.renderer` in the config, and costs
nothing when off.

- Grading through a `.cube` LUT (`NYA_ASSET_TYPE_LUT`, an RGBA8 3D texture), after ink, occlusion and FXAA
  and before bloom. `vivid.cube` (was `toon.cube`) saturates muted colours and keeps black and white exact; mean saturation in
  the 3D demo 49 to 63, about 0.35 ms at 1280x720. Key 4 in both scenes, `grade_lut` and `grade_strength`
  in the config.
- A cool shade colour gathers in cast shadow and the dark band; sky and ground ambient colours replace flat
  `ambient`; highlight and rim soften over exactly one pixel.
- Screen-space ink from depth and normal discontinuities replaces the inverted hull: silhouettes, hard
  creases past 40°, thinning with distance and fading into fog. Banded ambient occlusion at half resolution,
  FXAA, and debug views (normals, depth, occlusion, ink, cascades). The 3D pass writes an RGBA16F normal and
  distance buffer as a second colour target, only into render textures created with `.normals`; pipelines
  build that variant on first use. Keys 1, 2, 3 and v in the 3D demo, fed from `engine.renderer` in the
  config. Release 1280x720: +0.11 ms and +39.5 MiB at 4x MSAA, +0.06 ms and +11.4 MiB at 1x.
- Ink silhouettes predict the far pixel from the near side's distance slope and check back from two samples
  past the edge; creases must still turn against the faces beyond, so slivers draw nothing. The dotted cube
  edge and valley dashes are gone. `[ ]` Faint short dashes remain on terraces descending away from the
  camera, which are real sub-pixel ledges; a distance-relative floor removed them but cut cube silhouettes.
- Ambient occlusion reaches at least `min_radius` pixels (32). `[ ]` At the default orbit the band is still
  subtle; that is strength and band threshold, not radius.
- Bloom is a built-in half resolution pass (`NYA_PostBloom`, `engine.renderer.bloom`) sharing the depth of
  field target: 0.095 to 0.056 ms, +0.88 MiB while depth of field is off.
- `[ ]` A smaller normal buffer: view space normal only, or sample the multisampled buffer where backends
  allow and skip the resolve.
- Depth of field (tilt shift, or distance focus read from the normal buffer's distance channel), speed lines
  scaled by camera speed, projected box decals in one batched pass, and HDR output (extended linear or HDR10,
  every pipeline still built for the SDR format). Keys 5 to 8. Post order: occlusion, ink, depth of field,
  FXAA, caller passes (grade, bloom, pause grey), speed lines, debug view. Release, 1280x720, 4x: decals
  +0.06 ms and +0.5 MiB, distance focus +0.05 ms and +0.9 MiB, speed lines +0.02 ms, HDR +0.02 ms and
  +3.5 MiB; off costs nothing.
- HDR lifts only the scene: `nya_render_output_scene_end` zeroes the frame's alpha before the HUD draws.
  `[ ]` HDR10/PQ untested on hardware.
- Speed lines converge on the vanishing point of the camera's velocity (`NYA_PostSpeedLines.motion`), centred
  when backing away or moving across the view.
- `[ ]` Distance focus alone pays for the whole normal buffer.
- A skinned, animated bar (bender.fbx) in the 3D demo, lit and shadow casting, posed once per tick so
  every cascade matches the camera; `f` freezes it. The 2D ledge marker is an animated sprite with a frame
  event. `game.animation_speed` sets both clocks live.
- `[x]` The skinned draw is culled now: `nya_render3d_skinned_bounds` gives the sphere the pose occupies,
  taken from the posed bone origins rather than the rest box and padded by the rest radius. It sits in
  `render_cull.c`, which both builds include, so a headless test reaches the real arithmetic.
- `[x]` The skinned draw walks the mesh's parts and binds each material's texture, through a
  `NYA_RENDER3D_PIPELINE_SKINNED_TEXTURED` that needed no new shader. The loader also folds each part's
  base colour into the skinned vertices, which it never did: that buffer is uploaded straight from the
  loader and never passes the staging the static path folds colour in, so a rigged model drew white.
- `[ ]` Nothing in the tree exercises either: `bender.fbx` is one part, white, untextured. A rigged model
  with materials would turn the new test from a guard into a demonstration.
- `[ ]` No test reaches the non-headless skinned draw.
- A ring of six standing stones around the basin in the 3D demo, which is what gave three features their first
  caller in the game. Each slab is built here rather than loaded, at three detail levels registered with
  `nya_render3d_mesh_register` and chained with `nya_render3d_lod_register` (8 section points over 3 bands, then
  over 1, then a plain 4 point block: 192, 96 and 48 vertices, 11.8 KiB of vertex data in all). Each is convex
  and opaque, so its far face is an exact occluder: `nya_occlusion_quad` takes it and `nya_render3d_occlusion`
  hands the buffer to the camera pass. The bodies are static, on `GNY_LAYER_STONE`, so the audio trace finds
  them and the pick ray does not. The HUD row shows what all three did. Release, 1280x720, 4x, camera at its
  reset orbit: frame work 0.51 to 0.54 ms average, 61 to 67 GPU draws, 31 passes either way, uploads 297.7 to
  298.7 KiB, GPU memory unchanged at the overlay's 0.1 MiB resolution; +57.7 KiB in the world arena for the
  occlusion buffer, which is kept across a visit rather than taken again. First frame 68.5 to 66.1 ms. At that
  camera the ring hides about 60 of 650 tests a frame.
- `[ ]` The occluder is the slab's far face, so the ring rejects things behind a stone and nothing else; a scene
  wanting occlusion culling to pay would need an occluder the size of a wall.

Next:

- The 3D scene between `nya_render3d_begin` and `_end` is recorded once as segments of shared state, uploaded
  in one copy pass, and drawn by each shadow cascade and the camera from the same buffers with per-pass index
  lists (a draw keeps a mask of the passes whose frustum it touches). `nya_render3d_shadow_set` replaces the
  cascade loop. Release 3D demo, 1280x720, 4x: uncapped frame 1.00 to 0.41 ms, CPU work at the cap 0.28 to
  0.19 ms, uploads 61 (203 KiB) to 12 (152 KiB), render passes 59 to 25. The shadow atlas exists only while a
  scene casts shadows (-12.6 MB off). `-mf16c` inlines half float conversion. The overlay shows draws, passes
  and upload bytes per frame.
- `[ ]` A scene past the vertex, segment, instance or group ceilings is drawn early in pieces, and earlier
  pieces miss later casters' shadows. Cascades use the light in effect when the scene first draws;
  orthographic cameras cast no shadows.
- `[ ]` Decal grids miss their cache every frame while marks shrink (probe 2.2%, staging 1.1% of samples).
- Indirect draws are not worth it yet: about 15 calls per pass, each with its own mesh buffer or material;
  removing them saved at most 0.04 ms.
- A pipeline cache on disk is not possible through SDL 3.5 (Vulkan passes a null cache; D3D12 and Metal have no
  equivalent exposed). Cold driver cache: 46 pipelines take 222 ms, 11 lazy variants 57.5 ms after the first
  frame; warm, 0.8 ms. SDL does not document pipeline creation as thread safe.
- A render graph is not planned: the pass order is fixed and short, and a graph would be more code than
  the passes it orders.

## `[~]` Fluid volumes

Incompressible Navier-Stokes on a grid (Stable Fluids: advect, diffuse, project), for smoke, fire and
shallow water. Velocity, density and temperature, with buoyancy, weight, vorticity confinement and
box obstacles. `render_fluid.h` carries the reasoning; the short version is below.

- Grid rather than SPH particles. A grid is allocated once and every step touches the same cells,
  where SPH rebuilds a neighbour structure whose cost follows how the particles clump. A grid sweep is
  a fixed loop in a fixed order, where an SPH neighbour list is gathered in whatever order the
  particles currently sit in and a different summation order is a different sum. Semi-Lagrangian
  advection is unconditionally stable, where SPH is CFL limited and one fast particle demands
  unbounded substeps. And volumetric smoke out of SPH means splatting into a grid to draw it anyway.
  What would bring SPH back is a small volume of liquid in a large empty space, a wake across a lake,
  and that is a second module rather than a rewrite of this one.
- One solver, and it is three dimensional. A 2D volume is a grid one cell deep whose two z boundary
  planes mirror the interior, and every term collapses by itself: the seven point Laplacian's z
  neighbours both equal the centre cell, so `p_l + p_r + p_u + p_d + 2p - div = 6p` is exactly the 2D
  equation, and the 3D curl with `w = 0` is the 2D scalar curl. The equality is bit for bit and a test
  asserts it, which is why the third axis is flattened once in `nya_fluid_create` and `nya_fluid_emit`
  rather than branched on per cell: a ternary inside a loop lets the compiler contract the two
  branches differently and the two spaces then disagree in the last bits.
- `NYA_FLUID_PRESSURE_ITERATIONS` is 20. On the 32x48x32 bench grid with confinement off, divergence
  entering a step is 2.09 and leaving it 2.36 at 4 sweeps, 1.39 at 8, 0.93 at 20, 0.87 at 40 and 0.89
  at 80. Twenty is where another sweep stops changing the picture.
- Drawing is off until a window asks. `NYA_FluidRenderOptions` sits on the window beside the post
  chain's scene features and is zeroed, so `nya_fluid_draw` returns before it reads the grid. 2D draws
  one bilinearly shaded quad per live cell through render2d, 3D additive camera-facing splats through
  render3d, which the post chain composites and bloom picks up.
- Volumes register as the `fluid_volumes` ceiling and their bytes as the `fluid_cells` gauge, both
  lazily on the first create, so a program with no fluid in it registers nothing.
- The solver draws no randomness, reads no clock and uses no threads, so `nya_fluid_checksum` is an
  oracle: `tests/nyangine/renderer/test_fluid.c` drives two volumes from the simulation harness and
  asserts the same seed replays the same run.
- gnyame: a 48x32 steam vent over the 2D tilemap, and a 12x18x12 column over the 3D bonfire whose
  obstacles are the pile's crates. `9` toggles the drawing in either scene.

- `[ ]` The step is the cost, not the draw: 40 Gauss-Seidel sweeps a step (20 per projection, two
  projections) over the whole grid, single threaded and serially dependent along x so it does not
  vectorize. Red-black ordering would vectorize it and keep the convergence; a multigrid V-cycle would
  beat both and is a lot more code. Measure before choosing.
- `[ ]` Dropping the first projection, the one before advection, halves the solve. Stam keeps it so
  advection rides a divergence-free field; nobody has measured what it is worth here.
- `[ ]` A raymarched 3D volume instead of splats. It needs a 3D texture uploaded every frame, a
  pipeline and a shader per backend, so it waits for compute passes.
- `[ ]` No obstacle comes from a collider's actual shape, only from an axis-aligned box the caller
  passes. A rotated crate is still a box to the fluid.

## `[~]` Networking

UDP is always encrypted, loopback never: a stateless handshake (padded CONNECT, 45 byte cookie challenge, no
amplification) authenticated with X25519 against the server's long-term key and an optional player key, then
XChaCha20-Poly1305 per packet with a key per direction and a 32 packet replay window. `--server-key` pins the
server; `nya_net_key_pair_load` keeps its identity. Handshakes are rate limited per address (8/s, burst 16) and
capped at 4 connections per IP. The server applies one command per tick (burst 4), checks command ticks, caps
speed with `max_speed` and kicks on a decaying violation score. Snapshots carry only changed entities in fixed
point against the acknowledged baseline, rotations smallest-three in 32 bits. Clients reconcile against the
command the server confirmed, follow the server's tick rate and draw replicas with a jitter-adaptive delay and
100 ms extrapolation. `--net-latency/-jitter/-loss/-duplicate/-reorder` condition the transport. Every wire
decoder is fuzzed from a fixed seed. Bench, 48 crates and 6 drones: 44.3 to 6.9 kB/s down settled, 10.3 to 3.9
up. Under 120 ms, 20 ms jitter and 5% loss, prediction converges with no corrections at about 3 kB/s each way.

- `[ ]` One fragment per datagram; 28 bytes per packet overhead dominates small snapshots.
- `[ ]` Cumulative acks only; lag compensation rewinds to the acknowledged tick, not the render time.
- `[x]` Hostname resolution no longer blocks: the name is polled from the transport's update, so connect
  returns at once. Measured 1005 ms before, 0 ms after. The failure arrives as a DISCONNECTED event now.
- `[x]` Version-rejected peers still linger until timeout. The server disconnects a refused peer (wrong version,
  or a full server) right after its REJECT, with the reason in the disconnect itself, so a peer whose REJECT was
  lost still learns why. Testing it found the loopback transport never reported a disconnect to the far end.
- `[ ]` No allowlist API for player keys; the Steam transport is a stub.

## `[~]` Steam targets and anti-tamper

`steam-linux` and `steam-windows` link the Steamworks SDK and ship its library beside the executable.
`NYA_AppOptions.steam_app_id` relaunches through Steam when started outside it, runs callbacks once a frame and
plays on without a client. steam-linux compiles every vendor and the game with clang 22 against the pinned sniper
SDK sysroot (downloaded into vendor/steamrt, 1.2 GB): GLIBC_2.29 at most, curl on the runtime's GnuTLS. CI builds
and verifies all four targets; CD publishes both depots.

Anti-tamper: executable stamp and chunked code baseline on a startup thread, a sweep of one 64 KiB chunk every
250 ms (13 µs), per blob entry hashes checked on first load, and a watchdog requiring the checks to have run and
agreed. Failure logs one line and exits 86. `base_integrity.h` states the limits: this raises the bar, the server
stays the authority.

- `[ ]` Steam Cloud rules; `net_steam.c`; a real client and depot upload not exercised.
- `[ ]` The steam-linux binary only starts inside the runtime on hosts with a newer nettle.

## `[~]` Audio propagation and effects

Positional voices are traced through a batched ray callback the app wires to Box3D or Box2D: rays over a source's
extent give partial occlusion, a reverse ray gives blocker thickness for transmission, and four probes find a way
around with a Maekawa detour loss that pulls the sound toward the opening. Fourteen listener probes, four a frame,
estimate enclosure and distance, drive the sound bus reverb and place six panned echo taps. One ray budget (64,
ceiling `audio_rays`) is shared round robin, nothing is allocated per frame, nothing is cast when off. Every bus
runs a lock-free chain: high/low pass, 3-band EQ, compressor, echo, echo taps, reverb, limiter, eased per 32
frames. Config under `engine.audio`. Cost: full chain 22.9 µs per 10 ms buffer; 16 hidden voices 5.4 µs a frame.
Behind the basin rim the fire drops 20 dB and its spectral centroid goes from 3238 to 233 Hz.

- `[ ]` Interaural time delay and head shadow need our own stereo panner in place of SDL_mixer's mono 3D path.
- `[ ]` Thickness is the span between first hits, so two thin walls read as one thick one.
- `[ ]` Echo taps do not check that the source sees the surface; one diffraction reach.

## `[~]` Interpolation between ticks

Update runs at a fixed tick and frames draw whenever the display allows, so a frame can land between ticks
or see none. `nya_app_tick_alpha` says where between the last tick and the next it sits. Particles draw from
their previous tick's position and age toward the current one; before, the smoke and sparks froze on frames
without a tick and jumped on the next, which read as flicker (captured: every other frame unchanged).

- Entities capture their transform at the start of each tick (`nya_system_entity_transforms_capture`, 0.7 ns a
  slot) and draw through `nya_entity_render_position` / `_rotation` (4.6 ns). `nya_entity_transform_snap` makes
  a jump draw instantly and carries children; spawn, teleports, zero duration `move_to` and net replicas snap.
  `NYA_Entity` is 704 bytes (was 672). The 2D camera interpolates; the skeleton animator samples the clip
  between ticks (`nya_skeleton_animator_render_pose`). At 120 fps against a 62.5 Hz tick the 3D cube region's
  per-frame change evenness went from 0.87 to 0.16 (CV), the 2D region from 0.90 to 0.04.
- Fixed on the way: 3D bodies integrated their mirrored velocity a second time and drew a tick ahead.
- `[ ]` `NYA_SkeletonPlayer` (layers, crossfades, root motion) still samples per tick; y-sorting uses the
  current tick; the frame rate cap is not in the config.

## `[~]` Shadows

Cascades are fitted to slices of the camera frustum by bounding sphere. The crossfade is verified on screen.
The light basis snaps to 0.5° steps, which halved the pixels changing per frame under a still camera. Taps
filter bilinearly and the edge is cut crisp. Cascade count, map size and shade colour are per window
(`NYA_Render3DShadowOptions`), fed live from the config with `shadow_bias`. Two cascades by default: the
third cost 0.3 ms with no visible gain.

- `[ ]` Cascade selection by depth switches hard, and casters more than two extents toward the light from a
  cascade's centre are clipped out of it. Neither shows in the demo.
- `[ ]` A faint checker on steep terrain walls under the water.
- `[ ]` Ink draws dashed streaks along the steep terrain slope and dotted along a cube's bottom edge in the
  close 3D shot.
- Point and spot light shadows are not planned: six scene passes per light.

## `[~]` Ceiling auditing: HUD done, config over macros blocked

Ceilings are registered and shown in `debug_overlay.c`, fullest first, amber past 75%, red past 90%. There
are 33 registration sites now, up from 20, as the IPC, control, WebSocket, simulation, registry and UI
tables came in.

- Was blocked on the engine owning its config instance, done 2026-09-23 (see Phase 1): `NYA_TWEEN_MAX`,
  `NYA_ENTITY_MAX` and `NYA_RENDER2D_FONT_CACHE_MAX` can now become config through `nya_config_engine()`
  the way `shadow_bias`, `shadow_cascades` and `shadow_map_size` already do. Still `[ ]`: nothing reads
  these three as config yet, only the ownership that made it possible is in place.

## `[~]` SDF text

The menu draws through the NYA_Font registry ("menu" at 22, "menu_title" at 44 as a distance field).
Compared on screen with the bitmap: at a smoothing floor of 1/16 the edge was two pixels and softer; at 1/32
edge and stroke weight match.

- A distance field line measures as wide as the same face in coverage (SDL_ttf added the spread); the title
  sits where the bitmap did, and the 28 pt title matches the bitmap within 0.8 px.
- The atlas latches its mode at bake time.

## `[ ]` Budgets

3D scene, release, 1280x720, 4x MSAA: 116 MB RSS, about 300 MB VRAM.

### Glyph atlas

Atlases are R8 coverage with `NYA_RENDER2D_PIPELINE_TEXT` for coverage text, 128 cells each (was 512). The
busiest atlas fills 53 (game HUD plus debug overlay), the menu's `@22` 28. A full atlas warns once and draws
new glyphs blank. Distance field cells include SDL_ttf's 8 texel spread on each side, which clipped 68 of 95
glyphs at 17 pt before; the `@44` distance field title atlas is 1440x544.

`NYA_RENDER2D_FONT_CACHE_MAX` holds 24 atlases and evicts least recently used, after flushing the batch, since
a queued vertex can still name the texture being released. That number is a memory budget, about 90 KB per
atlas at body size, not a correctness bound: a miss costs one rebake, not blank text for the rest of the run.

Glyphs upload one cell at a time through a cell sized transfer buffer: the menu and HUD fonts staged 3.4 MB
of transfer buffers, now 7 KB.

### The scene is recorded once a frame

Two cascades plus the camera pass by default, all drawing one upload. Before that change:

| Symbol                        | Share | Note                          |
| :---------------------------- | ----: | :---------------------------- |
| `VULKAN_UploadToBuffer`       |  2.4% | four uploads of the same data |
| `_nya_render2d_quad`          |  1.6% |                               |
| `nya_render2d_text_with_font` |  1.2% | shaping per string per frame  |
| `nya_render3d_quad`           |  1.2% |                               |

Done: `nya_render3d_sphere` draws a registered unit sphere, the shadow pass no longer sorts, terrain
physics is a heightfield.

- A depth-only shadow vertex format was tried and reverted after it broke shadows. Look at the vertex
  layout the shadow pipeline is built with before retrying.

### Vertex formats

`NYA_Vertex3D` is 36 bytes (was 64): FLOAT3 position and normal, HALF2 uv, HALF4 colour. Colour stays
half float because emissive colours exceed one. `NYA_VertexSkinned3D` is 44 bytes (was 96): the same plus
UBYTE4 bone indices and UBYTE4_NORM weights summing to exactly 255, the rounding remainder on the strongest
influence. bender.fbx's vertex buffer went from 621 KB to 285 KB.

- `[ ]` Octahedral SNORM16x2 normals would reach 28 bytes, but `mesh3d_edge` takes `fwidth` of the
  interpolated normal. Measure first.

### Asset blob

Entries are LZ4 compressed when that saves `NYA_ASSET_BLOB_MIN_COMPRESSION_SAVING_BYTES`, expanded once
and shared by reference count.

- `[ ]` Bake less: nothing checks whether an asset is ever loaded.

### Binary size

Linux release 21.5 to 11.5 MB, Windows 23.6 to 10.8 MB, with every plugin still built in:

- Vendor options drop what the engine never calls: SDL's 2D renderer, GL, camera, haptic, dialog, tray,
  KMSDRM and software blitters; WebP and libpng (SDL_image's stb_image decodes PNG and JPEG); plutosvg;
  libgme, libxmp, WavPack, MIDI and the external Vorbis/FLAC/MP3 libraries (SDL_mixer's built-in decoders);
  curl features the request plugin does not use. -5.7 MB Linux, -7.3 MB Windows.
- Vendors build at -O2 with a section per function, collected in the release link. Frame times and benches
  did not move; the engine stays -O3, 4.4% faster than -O2 on the benches.
- A Windows release exe exports nothing (-1 MB), and Windows vendors cross compile with clang, since mingw
  gcc's sections are not COMDAT and a PE link keeps them.
- No local symbols (Linux) or COFF symbol table (Windows); traces are identical without them.
- The blob bakes only the shader formats a target loads and leaves out `assets/icons`
  (`NYA_ASSET_UNUSED_DIRECTORY`): 1525 entries to 142.
- `.eh_frame` stays (0.74 MB): libbacktrace and LuaJIT unwind through vendor frames with it.
- `[ ]` SDL's dynamic API off would save 0.66 MB but stops a Steam runtime swapping SDL.
- `[ ]` harfbuzz is 1.2 MB; its HB_LEAN config, if shaping quality holds.
- After pulling, delete the vendor build trees so they reconfigure; CI rekeys its cache on the recipes.

### Static memory

From `nm --size-sort -S` on the release binary. `.bss` went from 2.45 MB to 0.77 MB.

| Object                                |         Size |                                                          |
| :------------------------------------ | -----------: | :------------------------------------------------------- |
| `NYA_ASSET_BLOB`                      |       2.4 MB | `.rodata`, LZ4 per entry                                 |
| `b3_worlds` / `b2_worlds`             |   37 + 21 KB | 8 worlds each via the vendor rules (was 596 + 344 KB)    |
| `_nya_audio_system`                   |       3.6 KB | reverb lines allocated per bus on first use (was 446 KB) |
| `_NYA_NET_CLIENT` / `_NYA_NET_SERVER` |     4 + 3 KB | replica map only for remote clients, peers per used slot |
| `dphaseTable` / `tllTable`            | 256 + 128 KB | vendored audio decoder tables                            |

Release RSS: 2D game 72.2 to 66.8 MiB, 3D demo 77.9 to 72.5 MiB. The entity table reserves address space and
commits 256 slots (172 KB) at a time as spawning reaches them, instead of 5.25 MiB zeroed at startup; spawn
plus despawn of 1024 went from 147 µs to 26 µs, mostly because `nya_tween_cancel_target` stops at the last
live tween. A checkout built before this keeps 128 world box2d/box3d until `vendor/box2d/build-*` and
`vendor/box3d/build-*` are deleted.

### VRAM

MSAA is runtime configurable (`NYA_RenderOptions.msaa_samples`, `engine.renderer.msaa_samples` in
`engine.nya`, hot reloaded). Pipelines build single sampled and multisampled variants on first use. The post
chain holds its second target, single sampled without depth, only while two or more passes run.

`gpu_textures` at 1280x720, debug, by sample count 1 / 2 / 4 / 8:

| Scene   |    1 |    2 |    4 |     8 |
| :------ | ---: | ---: | ---: | ----: |
| 2D game |  7.1 | 24.7 | 45.8 |  88.0 |
| 3D demo | 40.2 | 61.3 | 89.4 | 145.6 |

At 4x this was 77.5 and 107.0 MiB before. The debug overlay's `gpu_textures`, `gpu_buffers` and
`gpu_transfer` rows count what SDL is asked for, not the driver's pages.

- `[ ]` Half resolution bloom target.
- `[ ]` Measure driver VRAM, not only what SDL is asked for.

### Fluid volumes

Release, -O2, one whole step at 20 pressure sweeps: forces, confinement, two projections, four
advections and the scalar pass. Held is `nya_fluid_memory_bytes`, thirteen f32 fields plus a byte of
obstacle mask per cell; resident is the volume's arena. Allocated once at `nya_fluid_create`, and a
step allocates nothing.

| Grid        |  Cells | ms/step | MiB held | MiB resident |
| :---------- | -----: | ------: | -------: | -----------: |
| 2D 48x32    |   5100 |   0.300 |     0.26 |         0.26 |
| 2D 64x48    |   9900 |   0.612 |     0.50 |         0.51 |
| 2D 128x96   |  38220 |   2.598 |     1.93 |         1.95 |
| 2D 192x144  |  84972 |   5.946 |     4.30 |         4.33 |
| 3D 12x18x12 |   3920 |   0.333 |     0.20 |         0.21 |
| 3D 16x24x16 |   8424 |   0.875 |     0.43 |         0.43 |
| 3D 32x48x32 |  57800 |   8.089 |     2.92 |         2.95 |
| 3D 48x48x48 | 125000 |  20.220 |     6.32 |         6.37 |
| 3D 64x64x64 | 287496 |  50.197 |    14.53 |        14.55 |

Linear in cells, so the grid is the whole cost decision. The sweep count is the other half: the same
32x48x32 grid is 2.75 ms at 4 sweeps, 4.09 at 8, 8.09 at 20, 14.75 at 40 and 28.10 at 80. gnyame runs
48x32 in 2D and 12x18x12 in 3D, which is 0.30 and 0.33 ms a tick and what a demo can pay beside
everything else in the scene. `bench/bench_fluid.c` is where all of this comes from.

- `[ ]` Drawing is not in these numbers. A 2D volume is one quad per live cell into the existing
  batch, a 3D one a splat per live cell into the mesh batch; neither has been measured against a
  frame.

### Text shaping

`nya_text_shape_with_font` and `nya_text_measure_with_font` keep one `TTF_Text` per (wrap width, face and
size, text) in a least recently used `NYA_Cache` (`text_runs`, 128 entries, keys up to 256 bytes), tagged
with the font asset's generation. A 20 line HUD frame went from 41.5 µs to 3.4 µs; the main menu's draw from
about 25 µs to 7 µs.

- The distance field request scan runs only after an asset loads (`nya_asset_generation`).

### Integrity check

The runtime baseline hashed `__executable_start` to `etext`, which faults when the linker leaves an unmapped
page before the code segment (a release build crashed at startup). It now hashes the executable `PT_LOAD`
segment from the program headers.

### Measurement

`nya_arena_resident_bytes` measures resident pages per arena. The arena report and the debug overlay show used
and resident per arena plus process RSS; opening the overlay in gnyame logs the full report.

## `[ ]` Reports from other machines

- `[ ]` Windows: the 3D demo drew only the models, particles and sky on one machine (terrain, pile, lamps and
  water missing). Wine with Direct3D 12 and Vulkan both render correctly, so it needs that machine's log:
  the `Render system initialized (<driver>)` line and any `failed to load`.
- `[ ]` "No sound" reported more than once, then "sound is back", then gone again. Recording the game's own
  PipeWire stream on Linux gets audible impacts in the 2D game and the 3D demo, also after a code reload.
  Each process opens two output streams and only the newer one carries sound, so a mixer showing the
  first looks silent. Music starts paused by design (`m`). Needs the platform and log of a silent run.
- `[ ]` Resident memory differs by machine (300 MB, 150 MB, 50 MB + 150 MB VRAM). Arenas no longer allocate
  64 MiB regions, which Windows committed up front; the rest is the GPU driver mapping into the process,
  which on integrated GPUs counts texture memory as RAM. Measure per driver before changing anything.

## `[ ]` Startup time

Release on Linux/Wayland reaches the first frame in about 46 ms (2D) and 56 ms (3D) after exec (was 135).
Startup logs engine init, subsystems and first frame; each subsystem's bring-up time is at debug level.

- The gamepad subsystem starts after the first frame, so frame two carries its ~42 ms. It cannot move to a
  thread: SDL requires `SDL_InitSubSystem` on the main thread, IOKit binds its HID manager to the calling
  thread's run loop, and the Windows notification windows belong to their creating thread.
- `[ ]` The renderer's GPU device takes ~21 ms and window plus swapchain ~10 ms, both on the main thread.
- `[ ]` Not measured on Windows.

## `[~]` UI system

`src/nyangine/ui` is an immediate-mode module, split by domain: `ui.c` the pass lifetime and the per window
state, `ui_layout.c` containers and placement, `ui_style.c` the look and the scale, `ui_input.c` presses and
focus, `ui_draw.c` the shapes, `ui_widgets.c` the widgets and `ui_text.c` the editable field, with
`ui_internal.h` between them and `ui.h` as the contract. One function runs as an input pass in `on_update` and
a draw pass in `on_render`; presses roll per tick, so each is handled once however many ticks a frame runs. Ids
hash label and panel. Fixed tables (64 widgets per pass, 64 panels) registered as ceilings, no heap. The style
is a struct where zero is the cartoon default, fed from `engine.ui` in the config.

Widgets: label, button, selectable, radio, toggle, slider, one line text input, colour picker, tabs, dropdown,
table, line and bar chart, icon, space and scrim. Containers scroll and clip on both axes, panels can be
dragged by their title, opacity groups fade a whole subtree, and a widget pops on focus and bounces on
activation. The field selects with shift, moves and deletes by word with control, and copies, cuts and pastes
through the system clipboard.

gnyame's menus and both HUDs use it, and the pause screen's widgets panel exercises the rest while plotting
frame times. Release: pause menu draw about 0.02 ms and 20 draw calls, input pass 0.004 ms, binary +41 KB.

- `[ ]` Merge same-state 2D draw ranges after sorting, so a menu is a few draw calls instead of two per widget.
- `[ ]` A dropdown's list takes room in the layout instead of floating over what follows. Ordering it last
  would mean holding the caller's options pointer past the call.
- `[ ]` No multi-line text field, and no navigation into an open dropdown with the keys alone.
- `[ ]` The German key hint line runs past a 1280 wide window.

## `[~]` Packaging and distribution

`packaging/` holds an AUR `gnyame-bin` package, a Flatpak manifest wrapping the release tarball, winget and
scoop manifests for the portable Windows zip, and SteamPipe scripts with `upload.sh`. CD checks the tag
against `VERSION`, then publishes both binaries, the Linux tarball, the Windows zip, rendered manifests and
`SHA256SUMS`. The Windows exe carries its icon and version info. `packaging/README.md` covers cutting a
release per channel.

- `[ ]` namcap, flatpak-builder and real winget/scoop installs have not been run.
- `[ ]` Nothing submits to the AUR, winget-pkgs or a scoop bucket automatically.
- `[ ]` A C2Y build needs glibc 2.38 (C23 `strtol`/`sscanf` variants, `strlcpy`), and curl links
  `libssl.so.3`. The Steam sniper runtime has neither, so build the Linux depot in the sniper SDK with
  OpenSSL static, or target Steam Linux Runtime 4.0, where the binary already runs.
- `[ ]` A real code signing certificate; the signing hook uses the sample `.pfx`, and an unsigned browser
  download warns under SmartScreen. winget and scoop installs do not.
- Saves and logs go under the app id in the user data directory (`~/.local/share/gnyame`, `logs/` inside),
  not the working directory, which is the install folder on Steam. Settings saved under the old `nyangine`
  directory are not migrated.

## `[~]` Steam

This section used to say Steam was dead code and that `plugins/steam/steam.c` had never been compiled.
Both were already false when it was written: `-DNYA_PLUGIN_STEAM` is on the steam link lines,
`plugins.c` includes `steam.c` under that guard, and the build ships `steam_api64.dll` beside the exe.
Kept as a note because it cost an afternoon to disbelieve.

What is real now: lobbies, peer to peer through `SteamNetworkingMessages`, achievements, stats and Cloud,
with `net_steam.c` a working transport satisfying the same interface as UDP and loopback.
`SteamAPI_RestartAppIfNecessary` runs first, init falls back when no client is running, callbacks run each
frame. No SteamStub DRM wrapper: it rewrites the exe, breaking the integrity CRC and the signature.

- `[ ]` None of it has run against a real Steam client. `test_steam.c` runs the module against a fake,
  which proves the shape and nothing about Valve's behaviour. That test was written on 2026-09-22; until
  then this section claimed it existed and it did not.
- `[ ]` A real depot upload has not been exercised.

## `[~]` CI

First fully green run on 2026-09-17 (Linux and Windows: vendors, check, tests, builds). A push cancels the
run in progress, so a vendor rebuild cut short saves no cache; wait for the vendor jobs before pushing again.
`actionlint` catches workflow syntax errors locally, which GitHub reports only as a run with no jobs. Vendors now build in one job per platform that saves the cache right after the
build, keyed on submodule revisions, vendor recipes and both toolchain directories; the check, test and
build jobs restore it.

- A Windows host does not build shadercross (DXC does not compile under MinGW). CI compiles shaders in
  the Linux vendor job and passes them on; a Windows developer needs `assets/shader/compiled/` from a
  Linux machine. `[ ]` A prebuilt DXC for Windows would remove that.
- Engine, game and test rules compile to `.objects/` and link in a second rule, launched through ccache
  when it runs (`NYA_CCACHE` overrides or disables). CI caches `$RUNNER_TEMP/ccache` per OS and job
  (`test`, `build`), 500M each. Tests link one engine object compiled once, unless they define anything
  before including the engine or name a `_nya_`/`_NYA_` identifier: 82 of 163 share it. Measured on the
  8 thread dev machine, other builds running beside it:

  | Command                     | Before | Split, no cache | ccache cold | ccache warm |
  | :-------------------------- | -----: | --------------: | ----------: | ----------: |
  | `./build run test`          |  106 s |            73 s |       123 s |        27 s |
  | `./build build debug-linux` |        |                 |       5.0 s |       1.6 s |
  | `./build build release`     |        |                 |      18.8 s |      12.7 s |

  Warm, the 27 s is about 19 s of running tests one at a time and the links. Release stays slow warm
  because both executables are LTO, and LTO codegen happens in the link.

- Compile commands no longer carry the commit hash, so a new commit gets 170 of 170 direct ccache hits
  and a 27 s warm test run (was preprocessed hits and 42 s).
- The shared engine scan lexes the engine headers and shares the engine when every `_nya_`/`_NYA_` name a
  test uses is declared there outside a `NYA_INTERNAL` line: 139 of 171 tests share it. Cold
  `NYA_CCACHE=off ./build run test` went from 76 s to 45 s. The rest name static internals, define
  something before including the engine, or use generated reflection names.
- `nya_build_parallel` is a pool that starts the next rule as soon as any finishes. `./build run test` on
  the 8 thread dev machine: 111 s with batches, 106 s with the pool. The gain is small because test
  compiles all take 3.5 to 5 s; it grows with uneven rules.
- `[ ]` No clang-format gate: the tree does not satisfy `.clang-format`, and fixing that is a 73k line
  reformat with include regrouping to review.

## `[~]` The verification rule

Six drones in the 2D scene learn to fly to the player: five fly the best NEAT genome, one a DQN. Both train
on one job every 0.25 s and are scored on the same eight test flights; the HUD shows the numbers and the
genome. A nav flow field around terrain and map picks each drone's next point. The best genome is saved to
`robots.nya` and each run is a `GNY_RobotRun` row in `robots.db`, written through the reflection-driven ORM
in `plugins/sqlite/orm.h`, so the struct is the schema. `game.robots` in `engine.nya` toggles and tunes
it live. Release: 3 to 4 µs a tick on the main thread, a 2 to 7 ms job every 0.25 s, about 0.8 MB resident,
nothing when disabled. Walking and all menus work on a gamepad, tested headless through SDL's virtual
joystick.

- Gamepad edges now roll at the end of each update tick like keys; per frame, a frame without a tick lost
  a press. `[ ]` The menus still detect pad presses from held state themselves and could use the edges.
- Render occlusion (`nya_occlusion_*`) and mesh LOD (`nya_render3d_lod_*`) are called now: the 3D demo's ring of
  standing stones rasterizes its far faces as occluders and carries the only detail chain. See the stylized
  renderer section for the numbers.
- curl, Discord and Steam stay unwired: each needs a network, a running client or an app id.

---

# Findings

### An assertion nobody could dismiss

`test_agent` hung about one run in thirty, once for 5h32m, and it was not slow. The agent found a real bug: typing
in the pause menu's colour picker hex field, then clicking the name field above it. The name field is declared
first, so it read the click and started typing before the hex field could let go, and `_nya_ui_typing_start`
asserted. The assertion was wrong; a click hands the keyboard over now. Then the crash reporter opened its window
on the offscreen driver and waited for a click that no test can make. A run with nobody watching now writes the
report to a file, and the test deadline turns any hang left into a failure with a backtrace. `ptrace` is limited
to children here (Yama 1), so the stacks came from running each agent under `gdb` directly and interrupting it.

### A crash window that had never opened

The vendored SDL was configured with `SDL_RENDER=OFF`, so `nya_crash_window_show` failed at
`SDL_CreateWindowAndRenderer` in every build ever shipped, and every crash fell through to the plain message
box. Turning the option on was not enough: vendor rules were built once, keyed on their own archive, so a
changed cmake option rebuilt nothing on a machine that had built before. Now `NYA_VendorRule` carries
`options_file` and `options_stamp`, and a recipe newer than its stamp forces a rebuild, for every vendor
through one mechanism. `hook_invalidate_stale_cmake_cache` compares every `-D` against the cache, which took
three corrections found only by running it over all 32 vendor rules: cmake canonicalises booleans, resolves a
bare compiler name to an absolute path, and keeps only the last `-D` for a name. It must be the last pre-build
hook; `hooks.h` says why.

### A script nothing ran

`assets/scripts/startup.lua` called `nya.log` as a function. `nya.log` is a table — `.info`, `.warn`,
`.error` — so it failed on line 7, and everything below it, including the `gnyame` table the game reads
back, never ran. `nya.time` was wrong the same way; the binding is `nya.app.time`.

The file's own comment says it exists so the Lua binding has a caller outside `tests/` that actually
runs. It had never run, and the comment is exactly why nobody checked: it reads as a guarantee.

It survived because its only caller is `gny_world_script_tick`, which needs a world. A run that sits in
the menu never reaches it, so the failure was invisible unless you got into the game and read the log —
which is how it turned up, while chasing something else entirely. The rule it breaks is the one already
written down under "The verification rule": a caller that only runs in a corner of the game is not a
caller. The test runs the shipped script now.


### A teleported body has no speed to give away

The pinball flippers were driven by `nya_physics3d_teleport`, which sets the transform without
simulating the move: no sweep, no contacts along the way. The solver therefore read a flipper that had
never moved and resolved the ball against it as if against a wall, so holding a flipper lifted the ball
and let it roll off again. The code even claimed "the speed of the sweep is what throws the ball" while
doing the opposite, which is what made it hard to see.

A kinematic body has to be given its velocity and left to the solver, which integrates it and carries
the contact. Measured both ways over the same path at the same speed: a paddle sweeping at 6 m/s into a
ball with 0.35 restitution sends it off at 8.1 m/s, and the same paddle teleported leaves it at exactly 0. That pair is a test now, because the difference is invisible in a screenshot and the wrong one looks
like a physics tuning problem rather than a wrong call.

The general rule: teleport is for putting something somewhere, never for moving it. Anything that has to
push what it meets is driven by `nya_physics3d_velocity_set` and
`nya_physics3d_angular_velocity_set`. Since integration lands near the intended pose rather than on it,
a driven body that has stopped wants one corrective teleport, and only one, or it never sleeps.

### A simulated clock handed back is a clock that ran ahead

Three bugs sat behind the time source seam, all of them latent because nothing had ever run a session.
Restoring the real clock asserted, because a simulated one that advanced a tick per frame is minutes
ahead of the wall clock and the loop read the difference as centuries of debt. A clock swap now rebases
the frame clock and moves the uptime origin with it, which also made the first frame of a session
deterministic: it used to measure against a timestamp the real clock wrote at startup. That rebased
first frame has no elapsed time at all, and `nya_app_run` divided by it for the frame rate.

### A session inherits the application as the last one left it

Two runs of one seed differed in their first tick. The pointer was wherever the previous run dropped it,
and the key releases that run dispatched on its way out were still in the queue, to be delivered inside
the next run's first frame. A run now releases what it holds and pumps it through before it returns, and
places the pointer before tick zero, through `nya_app_events_pump` rather than by writing into the input
system. The harness also counts the fixed steps the application ran separately from the frames the agent
acted on and asserts they match: "a frame is a tick" is what fast forwarding rests on, and trusting it is
how the real time mode shipped with 77 ticks where it claimed 120.

### A NEAT seed genome has nothing to say, and argmax makes it say the same thing forever

The seed topology has no connections, so every output is exactly zero and the largest is the first. The
whole of generation one pressed action zero and scored nothing. Ties are drawn from the session's own
seeded stream instead, so a genome with no structure behaves like the random baseline and every genome
that has grown a connection decides. With that, generation one reaches about 40 screen changes in 400
ticks and generation two found a genome reaching 399.

### The agent found a screen with no way out

With the scenes stubbed down to an id in the agent runner, the 3D scene was a dead end: the real
`gny_layer_cube3d_on_event` maps cancel to the main menu and the stub did not, so every agent that
wandered in spent the rest of its run there. Nothing asserted, nothing crashed, and every kind scored
zero. The stub keeps that one hook now. Worth recording because it is the shape of the bug this whole
facility is for: not a crash, a place a player can get stuck.

### "-0" was parsed by wrapping U128_MAX, and the fix is not to negate at all

`_nya_type_try_parse_s128` built a negative from its magnitude as `~magnitude + 1`. For a magnitude of
zero that is `~0 + 1`, an unsigned overflow back to zero: the right answer by undefined means, and
`-fsanitize=unsigned-integer-overflow` says so. It had never fired because nothing fed the parser a
"-0" until an HTTP body did.

The fix is to stop negating through unsigned arithmetic. `S128_MIN` is the one value with no positive
counterpart, so it is _built_ rather than negated into; everything else is known to fit `s128` by the
limit check above it and negates as a signed value. Found by AFL on `fuzz_http_request` at 1.5 million
executions, reached through serde's JSON numbers, and kept as a regression input.

### An idle timeout that reads the clock before the work measures the wrong interval

The HTTP drain read the monotonic clock once at the top of the tick and compared it against each
connection's last-activity stamp. Receiving stamps the connection with a _fresh_ reading, so on any
connection that had just been read from, the subtraction was "earlier minus later" on two unsigned
times: an enormous number, and an immediate drop of a perfectly healthy peer.

The clock is read after the work now, and the comparison is ordered before it is done. The general
shape: a duration between two timestamps is only a duration if you know which one came first, and
`u64` will not tell you.

### AFL could not run any target, for three reasons at once

`./build run fuzz` had never worked. `tests/fuzz/fuzz.h` defines two replay helpers that `__AFL_INIT`
makes unreachable, so the target failed `-Werror,-Wunused-function` before it linked; and afl-fuzz
refuses to start when `ASAN_OPTIONS` is set without `abort_on_error=1` (it watches for the child dying
on a signal, and asan exiting quietly with a status is a crash it never hears about) or without
`symbolize=0` (resolving a backtrace per crash costs more than the rest of an iteration). Each one is
a hard abort with a different message, so they surfaced one at a time.

All three are fixed, plus `AFL_SKIP_CPUFREQ`, which otherwise stops the fuzzer on any machine whose
governor is not `performance`. The corpus replay under `./build run test` had always worked, which is
why nobody noticed: replaying a corpus is not fuzzing, and the difference was five million executions
and two real defects.

### A content keyed cache costs a hash, and only inlining keeps it near a pointer memo

`NYA_Cache` replaced the asset memo, the glyph atlas table and the mesh registry. `nya_asset_get` over
eight handles went from 4.1 ns (slot by pointer, strcmp) to 4.9 ns. Out of line it was 7.5 ns: the calls
to `nya_cache_get`, the hash and `memcmp` each cost about a nanosecond, and relinking a recency list on
every hit cost more than scanning stamps on the rare evicting insert. FNV-1a would have been pointless
here at 12.8 ns per path; wyhash is 2.5 ns with the `strlen`.

### A collocated projection cannot remove what its own divergence cannot see

The fluid solver's divergence and pressure gradient are both central differences over two cells, while
the pressure Laplacian it inverts spans one. The two do not compose, and what survives is the
checkerboard mode the wide difference is blind to. It reads as an under-converged solve and is not
one: on the 32x48x32 bench grid the residual is 0.93 at 20 sweeps, 0.87 at 40 and 0.89 at 80, a floor
rather than a curve. Vorticity confinement makes it much worse, because its force is a cell-scale
field and most of its divergence lands in exactly that mode: at `vorticity = 1` the floor goes from
1.6% of the fastest speed in the field to 5.5%. It does not accumulate, since advection and any
dissipation at all remove the highest frequency the grid holds within a few steps. The cure is a
staggered MAC grid, which is a different solver.

A residual that falls with the sweep count is a solver that needs more sweeps; one that does not is a
discretisation that does not close. Measure both before adding iterations.

### A pointer compare cannot tell a reload from the same face

The glyph atlas noticed a reloaded font by comparing `TTF_Font*`. A new face allocated at the freed
address can compare equal. `NYA_Asset.generation` is unique across the run, so it is the tag instead.

### The ceiling registry cannot forget a row

A registered counter must outlive the process, which a per-window cache does not. Caches register one
static row per name instead, which reads zero once every cache with that name is destroyed.

### A CI runner ignores SIGPIPE, and children inherit it

GitHub runners start jobs with SIGPIPE ignored, which survives `exec`. `yes | head` in a child then gets
EPIPE instead of dying and prints `yes: standard output: Broken pipe`, the 34 extra bytes
`test_bug_command_pipe_deadlock` saw only on CI. `nya_command_spawn` resets SIGPIPE in the child.

### A union member can overwrite the pointer to its own bytes

`NYA_Asset.as_text`, `as_font` and `as_sound` share storage. A font decoded from `as_text.data` wrote
`as_font.font` over that pointer, so unloading freed null and every font and sound leaked its file bytes
inside an arena. The bytes now live in `NYA_Asset.raw`, outside the union. The mesh loader builds into a
scratch asset for the same reason.

### A seen window indexed by id forgets under reordering

The UDP duplicate filter was a bitmap indexed by `id % 1024` that cleared 64 bits ahead of each mark.
Receiving 5 after 10 cleared 10's mark, so a retransmit of 10 was delivered again, and on the reliable
channel it queued behind a delivery id already past it until the queue filled. The window now slides
behind the newest id.

### A code reload must not unmap what the engine points at

The engine keeps pointers into game DLL data: layer ids, perf timer names, config reflection and
instances, asset handle literals. Reloading dlclosed the old image, so after one reload the main window
handle and layer ids were gone and those pointers dangled. Old images now stay mapped (each load opens
its own copy, `RTLD_LOCAL` on Linux so new code does not bind to an older generation), layer ids and
perf names are copied, and state the game must keep lives in the world rather than DLL globals.

### LuaJIT's Makefile probes the compiler even to clean

It defaults to `gcc`, which MSYS2 CLANG64 does not have, and fails with "Unsupported target
architecture" before doing anything. Pass `CC` on every invocation, including `clean`.

### Render pass state is per pass, and a flush opens a new one

Viewport, scissor and load/store ops belong to an `SDL_GPURenderPass`. A copy pass cannot open inside a
render pass, so any flush that uploads ends the pass and begins another, with per pass state back at
defaults. That caused two shadow bugs: the cascade viewport set once in `nya_render3d_shadow_begin`, and
a `DONT_CARE` depth store that resume then `LOAD`ed. Neither shows in a scene that fits one flush. Set
per pass state in `_nya_render2d_pass_resume`.

### An occlusion buffer belongs to one viewpoint

`nya_occlusion_begin` takes a view-projection. Testing shadow casters against the camera's buffer culls
what the camera cannot see, which is exactly what casts shadows onto visible ground, so shadows blink as
walls pass in front of casters. Frustum planes are rebuilt per pass; anything else viewpoint dependent
in the cull path must be too.

### "Has a shadow pass run" is not "am I in a shadow pass"

`nya_render3d_shadow_active` is true from `shadow_end` to `nya_render3d_end`: false during the first
shadow pass and true for the camera pass. `nya_particles_draw` read it as the second meaning, so
particles went into cascade zero and vanished from the screen. `nya_render3d_shadow_pass_active` answers
the second question.

### A pointer keyed memo cannot memoize a handle built on the stack

`nya_asset_get` memoized on the handle pointer. A handle built in a reused stack buffer has the same
address with different text, so fonts came back at the wrong size and an atlas baked with another
font's SDF flag. The memo now keeps a copy of the handle text. While render_text.c still interned
handles to dodge the bug, two sizes sharing a stack address also evicted each other every frame and put
`nya_siphash` at 5% of the profile; the intern table is gone now that the memo compares text.

### The profile was of the sanitizers

`./build perf` used to read a profile of the `-O0` build with four sanitizers. Profiling release showed
what mattered: `nya_render3d_sphere` tessellating 1152 vertices per marker per pass (3.5%), and menu
text shaping on literals.

### Cascaded shadows are fitted to the frustum

A cascade of constant size a fixed distance ahead only works when the camera is near its subject. Each
piece of the fix is required: slices of the camera frustum so size follows the view; bounding spheres so
texel size does not change as the camera turns and the snap has a fixed grid; and cascade selection by
projecting into each volume, since a cascade is not a sphere around the viewer. Found with three debug
views: shadows off, visibility as greyscale, cascade index as colour.

### What shaping costs

`TTF_CreateText` per string per draw: 43.4 µs for a twenty line HUD, ~71 ns per glyph, 950 ns for one
short line, 700 ns to measure without keeping glyphs. Wrapping costs 6x. See `bench/bench_text.c`.

### Codegen dependencies were missing from half the build

`src/genyarated/strings.h`, `reflection.c` and `assets.h` come from metarules that only the DLL rules
depended on. Builds worked by ordering until a new shader made the executable compile against a stale
`assets.h`. `nya_build_parallel` builds a shared dependency once per epoch, so every rule names its
codegen.

### Clang vector miscompile (`f128x3`)

`ext_vector_type(3)` over x87 `long double`: clang sizes the stack slot from the packed
`<3 x x86_fp80>` (32 bytes) and stores the padded four lane form (40 bytes). Two and four lanes are
fine. `f128x3` is declared with a fourth lane; `sizeof` was already 64. Reproduced on clang 22.1.8.

### Box2D pre-solve traps

- `enablePreSolveEvents` is per shape and applies to the dynamic body; Box2D ignores it on static shapes.
- Box2D recycles contacts that have not moved and skips the callback, which is exactly a body resting on
  a platform. Recycling is suspended while a drop window is open, via
  `b2World_SetContactRecycleDistance`.

### `TTF_CreateText` with a null engine

A null `TTF_TextEngine` still runs full layout; the engine only draws. That makes shaping and
measurement GPU free. Shaping outputs glyph indices and there is no public codepoint to index mapping,
so atlases are keyed by index and baked lazily.

### LuaJIT allocator on x64

LuaJIT needs its heap in the low 2 GB and ships an mmap allocator for that; custom allocators are
unsupported upstream. The one non-arena allocation in the engine.

### Physics solver order

The solver steps before the entity update, so a velocity set during update is consumed next tick.
Zeroing a kinematic move's velocity on the arrival tick dropped its final step.

### Terrain LOD

- Measure distance to a chunk's bounding sphere, not its centre.
- Bound skirt depth by the terrain's relief, not a multiple of the cell.
- Scale bands by chunk width, not a fraction of the world.

### Unsigned overflow in hashes

Builds use `-fno-sanitize-recover=all`, so a hash whose multiply wraps needs
`__attr_no_sanitize("unsigned-integer-overflow")`. Missed twice, in the kerning memo and the glyph
lookup, and both aborted the first frame that drew text.
