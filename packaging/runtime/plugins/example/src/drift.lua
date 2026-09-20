-- Everything a plugin is that is not its entry point. require()d by module path, rooted at plugins/,
-- so this file is "example.src.drift" and two plugins can both have a src/drift.lua.

local drift = {}

-- Metres either side of the origin, and seconds for a full sweep. Named because a literal in the
-- expression below would be two numbers nobody can account for.
local AMPLITUDE_METRES = 3.0
local PERIOD_SECONDS = 8.0

--- Where something at `x` should be at time `seconds`.
function drift.step(x, seconds)
    return x * 0.0 + AMPLITUDE_METRES * math.sin(seconds * (2.0 * math.pi / PERIOD_SECONDS))
end

return drift
