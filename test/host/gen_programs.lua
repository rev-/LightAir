-- Encode the standard totem roles with the reference encoder and dump
-- the wire bytes so the C++ TotemVM host test can load them.
-- arg[1] = directory of this script (totemvm.lua lives there)
-- arg[2] = output directory for the .bin program dumps
local vm = dofile(arg[1] .. "/totemvm.lua")

la = {
  msg = { LIT=0x10, SCORE_COLLECT=0x12, POINT_REPORT=0x14, FLAG_EVENT=0x50,
          CP_BEACON=0x52, CP_SCORE=0x54, BASE_BEACON=0x56, FLAG_BEACON=0x58,
          BONUS_BEACON=0x5E, MALUS_BEACON=0x60 },
  flag_event = { TAKEN=1, DROPPED=2, SCORED=3 },
}
local libcache = {}
function la.lib(n)
  if not libcache[n] then libcache[n] = dofile("games/lib/" .. n .. ".lua") end
  return libcache[n]
end

-- patch the encoder to also return bytes: re-implement encode capture
-- (the reference encoder builds an array `b`; expose it)
local std = la.lib("std")
local out = {
  base0  = std.totems.base(0),
  baseX  = std.totems.base(1),
  baseA  = std.totems.base("any"),
  bonus  = std.totems.bonus(),
  flag0  = std.totems.flag(0),
  cp     = std.totems.cp(),
}
for name, prog in pairs(out) do
  local _, bytes = vm.encode(prog, 30)
  local f = assert(io.open(arg[2] .. "/prog_" .. name .. ".bin", "wb"))
  f:write(string.char(table.unpack(bytes)))
  f:close()
  print(name, #bytes)
end
