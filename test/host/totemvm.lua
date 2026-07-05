-- Reference TotemVM v1 validator + encoder (mirrors the normative
-- encoding in docs/totem-behavior-handshake.md).  Returns byte size.

local M = {}

local CMP  = { ["=="]=0, ["~="]=1, ["<"]=2, [">="]=3, ["<="]=4, [">"]=5 }
local ACLS = { empty=0, single=1, many=2 }
local ANIM = { Respawn=0, FlagTaken=1, FlagReturn=2, Bonus=3, Malus=4,
               Roster=5, Idle=6, BaseIdle=7, CPIdle=8, FlagIdle=9,
               BonusIdle=10, MalusIdle=11, FlagMissing=12, Control=13,
               ControlContest=14,
               Custom1=15, Custom2=16, Custom3=17, Custom4=18 }
local LIMITS = { states=8, regs=8, timers=4, prog=225 }

local function u16(b, v) b[#b+1]=v%256; b[#b+1]=v//256 end

local function val(b, v, cfg_secs)
  if type(v) == "number" then
    if v <= 255 then b[#b+1]=0; b[#b+1]=v else b[#b+1]=1; u16(b, v) end
  elseif v[1] == "r" then assert(v[2]>=0 and v[2]<LIMITS.regs, "bad reg"); b[#b+1]=2; b[#b+1]=v[2]
  elseif v[1] == "p" then assert(v[2]>=1, "payload idx 1-based"); b[#b+1]=3; b[#b+1]=v[2]
  elseif v[1] == "low" then b[#b+1]=4
  elseif v[1] == "sender" then b[#b+1]=5
  elseif v[1] == "team" then b[#b+1]=6
  elseif v[1] == "cfg" then b[#b+1]=1; u16(b, (cfg_secs or 30)*10)  -- resolved: imm16 deciseconds
  else error("bad value spec: "..tostring(v[1])) end
end

local function guard(b, g, cfg)
  local k = g[1]
  if k == "p" then b[#b+1]=1; b[#b+1]=assert(CMP[g[3]]); b[#b+1]=g[2]; val(b, g[4], cfg)
  elseif k == "len" then b[#b+1]=2; b[#b+1]=assert(CMP[g[2]]); b[#b+1]=g[3]
  elseif k == "r" then b[#b+1]=3; b[#b+1]=assert(CMP[g[3]]); b[#b+1]=g[2]; val(b, g[4], cfg)
  elseif k == "acc" then b[#b+1]=4; b[#b+1]=assert(ACLS[g[2]], "bad acc class")
  elseif k == "low" then b[#b+1]=5; b[#b+1]=assert(CMP[g[2]]); val(b, g[3], cfg)
  elseif k == "elapsed" then
    b[#b+1]=6; b[#b+1]=assert(CMP[g[3]]); b[#b+1]=g[2]
    assert(g[2]>=0 and g[2]<LIMITS.timers, "bad timer")
    if type(g[4]) == "number" then b[#b+1]=1; u16(b, g[4]//100)  -- deciseconds
    else val(b, g[4], cfg) end
  elseif k == "rssi" then b[#b+1]=7; b[#b+1]=assert(CMP[g[2]]); b[#b+1]=g[3]%256
  else error("bad guard: "..tostring(k)) end
end

local function colorspec(b, c)
  if c == nil then b[#b+1]=0
  elseif c[1] == "rgb" then b[#b+1]=1; b[#b+1]=c[2]; b[#b+1]=c[3]; b[#b+1]=c[4]
  elseif c[1] == "team" then b[#b+1]=2; val(b, c[2])
  elseif c[1] == "sender_player" then b[#b+1]=3
  elseif c[1] == "sender_team" then b[#b+1]=4
  elseif c[1] == "args" then b[#b+1]=5; b[#b+1]=#c-1; for i=2,#c do val(b, c[i]) end
  else error("bad color spec: "..tostring(c[1])) end
end

local function action(b, a, nstates, cfg)
  local k = a[1]
  if k == "goto" then assert(a[2]>=1 and a[2]<=nstates, "goto out of range"); b[#b+1]=1; b[#b+1]=a[2]
  elseif k == "set" then assert(a[2]<LIMITS.regs); b[#b+1]=2; b[#b+1]=a[2]; val(b, a[3], cfg)
  elseif k == "accbit" then b[#b+1]=3; val(b, a[2], cfg)
  elseif k == "accclr" then b[#b+1]=4
  elseif k == "start" then assert(a[2]<LIMITS.timers); b[#b+1]=5; b[#b+1]=a[2]
  elseif k == "bcast" then
    b[#b+1]=6; b[#b+1]=a[2]; b[#b+1]=#a-2
    for i=3,#a do val(b, a[i], cfg) end
  elseif k == "reply" then b[#b+1]=7; b[#b+1]=a[2]
  elseif k == "anim" then
    b[#b+1]=8; b[#b+1]=assert(ANIM[a[2]], "unknown anim "..tostring(a[2]))
    local c, rh = a[3], a[4]
    if c and c[1] == "rhythm" then c, rh = nil, a[3] end
    colorspec(b, c)
    b[#b+1] = rh and (rh[2] % 256) or 0xFF
  else error("bad action: "..tostring(k)) end
end

-- Validate + encode; returns byte count.
function M.encode(prog, cfg_secs)
  assert(prog.vm == 1, "vm version")
  assert(type(prog.states) == "table" and #prog.states >= 1, "states")
  assert(#prog.states <= LIMITS.states, "too many states")
  cfg_secs = cfg_secs or prog.cfg_default
  local b = { 1, #prog.states }
  for _, state in ipairs(prog.states) do
    b[#b+1] = #state
    for _, r in ipairs(state) do
      -- exactly one trigger
      local trig = (r.enter and 1 or 0) + (r.every and 1 or 0)
                 + (r.msg and 1 or 0) + (r.reply and 1 or 0)
      assert(trig == 1, "rule needs exactly one trigger")
      if r.enter then b[#b+1]=0
      elseif r.every then b[#b+1]=1; u16(b, r.every//100)  -- deciseconds
      elseif r.msg then b[#b+1]=2; b[#b+1]=r.msg
      else b[#b+1]=3; b[#b+1]=r.reply end
      b[#b+1] = r.cont and 1 or 0
      local when = r.when or {}
      b[#b+1] = #when
      for _, g in ipairs(when) do guard(b, g, cfg_secs) end
      assert(r.run and #r.run >= 1, "rule needs actions")
      b[#b+1] = #r.run
      for _, a in ipairs(r.run) do action(b, a, #prog.states, cfg_secs) end
    end
  end
  assert(#b <= LIMITS.prog, "program exceeds single-packet budget: "..#b)
  return #b, b
end

return M

