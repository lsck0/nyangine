-- Runs once when the world is created. See gny_world_create in layers.c.
--
-- Deliberately small. The point is not that the demo needs scripting — it does not — but that the
-- Lua binding has a caller outside tests/, so the marshalling, the engine table and the hot-reload
-- rules are exercised by something that actually runs.

nya.log("startup.lua: hello from Lua " .. _VERSION)

-- Values cross the boundary as NYA_Object, so a table here is a keyed object on the C side and can
-- go through serde without a conversion step. gny_world_create reads this back.
gnyame = {
  greeting = "nyangine",
  spawned_at = nya.time(),
  ledges = { "left", "moving", "right" },
}

--- Called by the game once per second, with the number of crates in the world.
--
-- An optional hook: the C side asks nya_lua_has_function first, so deleting this function is a
-- supported thing to do rather than an error.
function gnyame_tick(crate_count)
  if crate_count > 0 and crate_count % 64 == 0 then
    nya.log("startup.lua: " .. crate_count .. " crates")
  end

  return crate_count
end
