-- Fixture: a runtime error raised INSIDE a library, by a call from here.
--
-- la.lib strips std.lua and projector.lua of their debug information
-- (line tables, local names) to save RAM on boards without PSRAM.  The
-- trade is that an error inside a library names the file but no line.
-- projector's clamp compares the value it is given with a number, so a
-- string max_owned fails in projector.lua, not here.
local proj = la.lib("projector")
proj.define{ max_owned = "3" }

return { api = 1, type_id = 0x7F0A, name = "LibDebugLib" }
