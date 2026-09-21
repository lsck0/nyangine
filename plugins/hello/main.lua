-- The entry point, run after everything in src/. What it may call is decided by manifest.nya and by
-- the permission profile the game compiled: `nya.log` and `nya.plugin` are always there, `nya.entity`
-- and `nya.input` are there because the manifest asks for them and gnyame grants them, and
-- `nya.file.read` is not there at all, because nothing granted the filesystem. Calling it is not a
-- refusal, it is an attempt to call a nil value: the name was never put in this VM.
--
-- Every function under `nya` is described in docs/lua/nya.lua, which is generated from the engine
-- headers on every build. An editor pointed at it (.luarc.json already is) completes these and knows
-- which permission each one needs.
--
-- Every hook is optional. A plugin defining none of them loads, does nothing, and costs one registry
-- entry.

local state = {
    ticks         = 0,
    bursts        = 0,
    action        = nil,
    next_report_s = 0.0,
    marker        = nil,
}

-- Called once, after src/ and this file have run and after the plugin's registry entry exists.
-- Returning is the only thing that counts as success; an error here refuses the whole plugin.
function on_load()
    nya.log.info(greeting("gnyame"))

    -- By name, never by number. An action number is the game's own enum and a plugin that hard codes
    -- one breaks the day the game adds an action in the middle; the name survives that and survives a
    -- rebind in the pause menu too.
    state.action = nya.input.action_from_name("spawn_burst")

    -- A handle, not a pointer: after a despawn every call taking it answers nil, exactly as in C.
    state.marker = nya.entity.spawn({ name = "hello_plugin_marker", x = 0.0, y = 3.0, z = 0.0 })
end

-- The partner. Called before the VM is closed, so the bindings are all still there.
function on_unload()
    if state.marker ~= nil then nya.entity.despawn(state.marker) end

    nya.log.info(farewell() .. " after " .. state.ticks .. " ticks and " .. state.bursts .. " bursts")
end

-- The fixed timestep, the same one the engine's own systems tick on. This runs as one entry in the
-- system registry owned by "hello", so the debug overlay's owner table shows what it costs and what
-- its VM is holding.
function on_tick(delta_time_s)
    state.ticks = state.ticks + 1

    if nya.input.action_just_pressed(state.action) then state.bursts = state.bursts + 1 end

    local now = nya.app.time()
    if now < state.next_report_s then return end

    state.next_report_s = now + REPORT_INTERVAL_S

    nya.log.info(nya.plugin.name() .. ": " .. state.ticks .. " ticks, " .. state.bursts .. " bursts, " ..
                 nya.entity.count() .. " entities, uptime " .. string.format("%.1f", now) .. "s")
end
