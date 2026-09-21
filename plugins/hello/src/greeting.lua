-- Everything in plugins/hello/src/ runs before main.lua, in name order, into the same VM. There is no
-- `require`: `package` is one of the libraries a plugin's VM refuses, and a module loader is a path
-- resolver, which is a way out of the plugin's own directory. Ordering by file name is the whole
-- module system.
--
-- These are globals, and that is safe here in a way it would not be in one shared VM: this plugin has
-- a VM to itself, so `greeting` is `hello`'s `greeting` and nothing another plugin defines can see it
-- or be shadowed by it.

-- How often the tick hook says anything, in seconds. A heartbeat, not a log line per tick.
REPORT_INTERVAL_S = 10.0

function greeting(who)
    return "hello, " .. who .. ", from " .. nya.plugin.name() .. " " .. nya.plugin.version()
end

function farewell()
    return nya.plugin.name() .. " is going away"
end
