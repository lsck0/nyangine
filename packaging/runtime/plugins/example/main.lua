-- The entry point named by manifest.nya. The host loads this file and nothing else; everything below
-- src/ arrives through require().
--
-- Three optional functions are called if they exist, and skipped without complaint if they do not:
-- on_load once after the plugin is loaded, on_frame every frame, on_unload once on the way out.

local drift = require("example.src.drift")

local subject = nil

function on_load()
    nya.log("example: loaded")

    -- Handles, not indices: a stale one is rejected instead of addressing whatever now occupies the slot.
    subject = nya.spawn("crate", 0.0, 4.0, 0.0)
end

function on_frame()
    if subject == nil then return end

    local x, y, z = nya.position(subject)
    nya.move_to(subject, drift.step(x, nya.time()), y, z)
end

function on_unload()
    if subject ~= nil then nya.despawn(subject) end
    nya.log("example: unloaded")
end
