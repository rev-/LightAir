-- Manifest fixture: a game whose file scope leans on a library that does
-- not exist.
--
-- Every real ruleset pulls its libraries in at file scope, and a manifest
-- peek RUNS file scope — so a peek that loaded them for real would compile
-- tens of kilobytes of Lua per file, into a state it throws away
-- immediately.  On a device that is what ran the interpreter out of memory
-- partway through the scan and emptied the game menu.
--
-- So during a peek la.lib() hands back an inert stand-in, and the proof is
-- this file: the library it asks for is not on disk, and the indexing and
-- calling below would all fail against anything real.  Peeking it must
-- still report Api/type_id/name.  Loading it for real must fail.
local nope = la.lib("this_library_does_not_exist")

-- Indexed, called, called with a table, and chained — the shapes a real
-- ruleset uses at file scope (proj.define{...}, std.immunity(3000),
-- std.totems.cp()).
nope.define{ vars = { energy = "energy" } }
local imm   = nope.immunity(3000)
local totem = nope.totems.cp()
local deep  = nope.a.b.c(1, 2, 3).d

return {
  api = 1, type_id = 0x7F06, name = "LibScope",
  initial_state = 0,
  scoring_state = 1,
  config = {},
  vars = { { id = "energy", default = 1 } },
  monitor = { { var = "energy", icon = "ENERGY", col = 0, row = 0, states = { 0 } } },
  winners = { { var = "energy", dir = "max" } },
  totem_slots = {}, teams = 0,
  on_begin = function(vars) vars.energy = 1 end,
  rules = {}, update = {},
}
