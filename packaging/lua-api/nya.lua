---@meta
--- The whole API a plugin gets. One global table, `nya`, and nothing else.
---
--- This file is a stub: it declares the surface and documents it, and contains no implementation. Point
--- a Lua language server at it (`.luarc.json`: `"workspace.library": ["path/to/nya.lua"]`) and you get
--- completion, signatures and type checking against the real API while you write a plugin.
---
--- It is written by hand against the binding table in src/nyangine/plugins/lua/lua_engine.c, which is
--- the only place the set of functions is decided. Adding a binding there means adding it here in the
--- same commit.

---@class nya
nya = {}

---@alias nya.Entity integer An entity handle. Carries a generation, so a stale one is rejected rather
--- than addressing whatever now occupies its slot. Never do arithmetic on it.

---@alias nya.Action integer An input action, from the action table the game registers by name.

--- Writes a line to the engine log at INFO.
---@param message string
function nya.log(message) end

--- Writes a line to the engine log at WARN.
---@param message string
function nya.warn(message) end

--- Writes a line to the engine log at ERROR. Does not stop the plugin; return from `on_frame` for that.
---@param message string
function nya.error(message) end

--- Seconds since the game started, as the simulation counts them. Not wall clock: it is the value that
--- makes a replay deterministic, and it does not advance while paused.
---@return number seconds
function nya.time() end

--- Spawns an entity of `kind` at a world position, in metres.
---@param kind string One of the entity kinds the game registered. An unknown one logs and returns nil.
---@param x number
---@param y number
---@param z number
---@return nya.Entity|nil entity
function nya.spawn(kind, x, y, z) end

--- Despawns an entity. A handle that is already gone is a no-op, not an error.
---@param entity nya.Entity
function nya.despawn(entity) end

--- Where an entity is, in metres. Returns nothing for a stale handle.
---@param entity nya.Entity
---@return number|nil x
---@return number|nil y
---@return number|nil z
function nya.position(entity) end

--- Moves an entity to a world position, in metres. A stale handle is a no-op.
---@param entity nya.Entity
---@param x number
---@param y number
---@param z number
function nya.move_to(entity, x, y, z) end

--- Whether an action is held this frame.
---@param action nya.Action
---@return boolean held
function nya.action(action) end

--- Whether an action went down this frame. False on every frame after the first, however long it is held.
---@param action nya.Action
---@return boolean pressed
function nya.action_pressed(action) end

return nya
