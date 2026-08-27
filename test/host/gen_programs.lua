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

-- Synthetic role: not a game role, just the smallest program that
-- exercises the {"rssi"} value operand and signed 16-bit registers.
-- payload[1] selects what to do: 1 = latch this packet's RSSI into R0,
-- 2 = latch it into R1, 3 = animate iff R1 is the stronger of the two
-- (i.e. that sender was closer).  Storing a negative dBm and comparing
-- two of them is only possible because registers are int16_t.
local rssi_probe = { vm = 1, states = { {
  { enter = true, run = { {"set", 0, 0}, {"set", 1, 0} } },
  { msg = la.msg.BASE_BEACON, cont = true,
    when = { {"len", ">=", 1}, {"p", 1, "==", 1} },
    run  = { {"set", 0, {"rssi"}} } },
  { msg = la.msg.BASE_BEACON, cont = true,
    when = { {"len", ">=", 1}, {"p", 1, "==", 2} },
    run  = { {"set", 1, {"rssi"}} } },
  { msg = la.msg.BASE_BEACON,
    when = { {"len", ">=", 1}, {"p", 1, "==", 3}, {"r", 1, ">", {"r", 0}} },
    run  = { {"anim", "Bonus"} } },
  -- payload 4: is the stored reading still negative?  This is the rule that
  -- an 8-bit register cannot satisfy — truncating -80 gives 176, which
  -- compares greater than 0, so the sign is what the width buys us.
  { msg = la.msg.BASE_BEACON,
    when = { {"len", ">=", 1}, {"p", 1, "==", 4}, {"r", 0, "<", 0} },
    run  = { {"anim", "Malus"} } },
} } }

local out = {
  base0  = std.totems.base(0),
  baseX  = std.totems.base(1),
  baseA  = std.totems.base("any"),
  bonus  = std.totems.bonus(),
  flag0  = std.totems.flag(0),
  cp     = std.totems.cp(),
  rssi   = rssi_probe,
}
for name, prog in pairs(out) do
  local _, bytes = vm.encode(prog, 30)
  local f = assert(io.open(arg[2] .. "/prog_" .. name .. ".bin", "wb"))
  f:write(string.char(table.unpack(bytes)))
  f:close()
  print(name, #bytes)
end
