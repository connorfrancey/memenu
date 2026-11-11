-- A module of the mod, loaded with require("greeting"): files of a mod can only come from its own folder
return function(greeting, ticks)
    return string.format("%s (%d ticks)", greeting, ticks)
end
