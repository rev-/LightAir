-- Test harness: stub `la` kernel + smoke-test every game file.
-- Run from the repo root:  lua5.4 <this file>

local ROOT = "games/"
local clock = 0

la = {
  msg = { LIT = 0x10, SCORE_COLLECT = 0x12, POINT_REPORT = 0x14,
          FLAG_EVENT = 0x50, CP_BEACON = 0x52, CP_SCORE = 0x54,
          BASE_BEACON = 0x56, FLAG_BEACON = 0x58,
          BONUS_BEACON = 0x5E, MALUS_BEACON = 0x60 },
  flag_event = { TAKEN = 1, DROPPED = 2, SCORED = 3 },
  colors = { team = {}, player = {} },
  rhythm = {},
}
for t = 0, 7 do
  la.colors.team[t] = { 10 * t, 255 - 10 * t, 128 }
  la.rhythm[t] = { period = 1000 + t, pulses = 1 + t % 3 }
end
for p = 0, 16 do la.colors.player[p] = { p, p, p } end

local out = { radio = {}, ui = {}, shows = {} }
function la.now() return clock end
function la.my_id() return 2 end
function la.my_team() return 0 end
function la.team_of(id) return id % 2 end
function la.player_count() return 4 end
function la.player_short(id) return "P" .. tostring(id) end
function la.team_short(t) return ({ [0] = "O", [1] = "X" })[t] or "?" end
function la.totem_for_role(role, i) return i == 0 and 254 or 0 end
function la.trigger_down(n) return false end
function la.shine() return true end
function la.shine_lit() return nil end
function la.shine_ms() return 100 end
function la.shine_config(t) end
function la.send(target, msg, ...) out.radio[#out.radio+1] = { "send", target, msg, ... } end
function la.broadcast(msg, ...) out.radio[#out.radio+1] = { "bcast", msg, ... } end
function la.broadcast_relay(msg, ...) out.radio[#out.radio+1] = { "relay", msg, ... } end
function la.ui(ev) out.ui[#out.ui+1] = ev end
function la.ui_enlight(ms) out.ui[#out.ui+1] = "Enlight" end
function la.show(txt, ms) out.shows[#out.shows+1] = txt end
function la.background(bg) end
function la.clear_tray() end
function la.totem_ui(ev, ...) end

local libcache = {}
function la.lib(name)
  if not libcache[name] then libcache[name] = dofile(ROOT .. "lib/" .. name .. ".lua") end
  return libcache[name]
end

-- fake packet proxy
local function mk_pkt(fields)
  local p = { sender = fields.sender or 3, team = fields.team or 1,
              rssi = fields.rssi or -40, msg = fields.msg,
              len = fields.payload and #fields.payload or 0 }
  function p.byte(self, i) return fields.payload[i] end
  return p
end

local files = { "freeforall", "teams", "flag", "kingofhill", "outflow", "upkeep", "virus" }
local failures = 0
local totem_sizes = {}

for _, f in ipairs(files) do
  local ok, game = pcall(dofile, ROOT .. f .. ".lua")
  assert(ok, f .. ": load failed: " .. tostring(game))

  -- build the vars "proxy" (plain table with declared defaults)
  vars = {}
  for _, c in ipairs(game.config) do vars[c.id] = c.default end
  for _, v in ipairs(game.vars) do vars[v.id] = v.default end

  local steps = {}
  local function step(what, fn, ...)
    local okk, err = pcall(fn, ...)
    if not okk then
      failures = failures + 1
      print(string.format("  FAIL %-12s %-18s %s", f, what, err))
    else
      steps[#steps+1] = what
    end
  end

  -- structural checks
  assert(type(game.name) == "string" and #game.name <= 15, f .. ": bad name")
  assert(type(game.type_id) == "number", f .. ": bad type_id")
  assert(game.scoring_state ~= nil and game.initial_state ~= nil, f .. ": states")
  for _, m in ipairs(game.monitor) do
    local found, is_text = false, false
    for _, v in ipairs(game.vars) do
      if v.id == m.var then found = true; is_text = v.text or false end
    end
    for _, c in ipairs(game.config) do if c.id == m.var then found = true end end
    assert(found, f .. ": monitor var '" .. m.var .. "' not declared")
    -- A `bar` row is drawn as a filled 0-100 gauge, so it needs a number.
    if m.bar then
      assert(not is_text, f .. ": monitor bar '" .. m.var .. "' is a text var")
      assert(m.width == nil or (m.width > 0 and m.width <= 54),
             f .. ": monitor bar '" .. m.var .. "' width out of the 64px cell")
    end
  end
  -- A totem that hands itself to whoever answers its beacon needs the
  -- ruleset to answer deliberately: nothing replies on a game's behalf any
  -- more, so a declared BONUS/MALUS slot with no handler is unclaimable.
  do
    local answered = {}
    for _, handlers in pairs(game.on_message or {}) do
      for msg in pairs(handlers) do answered[msg] = true end
    end
    for _, slot in ipairs(game.totem_slots or {}) do
      local beacon = ({ BONUS = la.msg.BONUS_BEACON,
                        MALUS = la.msg.MALUS_BEACON })[slot.role]
      assert(beacon == nil or answered[beacon],
             f .. ": declares a " .. slot.role .. " totem but never answers its beacon")
    end
  end

  for _, w in ipairs(game.winners) do
    local found = false
    for _, v in ipairs(game.vars) do if v.id == w.var then found = true end end
    assert(found, f .. ": winner var '" .. w.var .. "' not declared")
  end

  -- lifecycle smoke test
  step("on_begin", game.on_begin, vars)

  clock = 5000
  for state, fn in pairs(game.update or {}) do
    step("update[" .. state .. "]", fn, vars)
  end

  -- feed a MSG.LIT into every state that handles it
  for state, handlers in pairs(game.on_message or {}) do
    for msg, h in pairs(handlers) do
      local payload = (msg == la.msg.LIT) and { 1 } or { 0, 1 }
      step(string.format("msg[%d][0x%02X]", state, msg), h, vars,
           mk_pkt{ msg = msg, payload = payload })
    end
  end

  -- replies
  for msg, subs in pairs(game.on_reply or {}) do
    for sub, h in pairs(subs) do
      step(string.format("reply[0x%02X][%s]", msg, tostring(sub)), h, vars,
           mk_pkt{ msg = msg + 1, payload = { sub } }, mk_pkt{ msg = msg, payload = {} })
    end
  end

  -- rules: run all conditions and actions
  for i, r in ipairs(game.rules) do
    if r.when   then step("rule" .. i .. ".when", r.when, vars) end
    if r.action then step("rule" .. i .. ".action", r.action, vars) end
  end

  -- score announce
  if game.on_score_announce then
    step("score_announce", game.on_score_announce,
         { { id = 1, team = 0, vals = { 5, 2 } },
           { id = 2, team = 1, vals = { 3, 1 } } })
  end

  -- totem sections: validate + encode every TotemVM program
  local vm = dofile(arg[0]:match("(.*/)") .. "totemvm.lua")
  for role, prog in pairs(game.totems or {}) do
    local ok2, size = pcall(vm.encode, prog, 30)
    if not ok2 then
      failures = failures + 1
      print(string.format("  FAIL %-12s totem %-8s %s", f, role, size))
    else
      steps[#steps+1] = "totem." .. role
      totem_sizes[f .. "/" .. role] = size
    end
  end

  print(string.format("OK   %-12s  %2d config, %2d vars, %2d monitor, %2d rules, %2d checks",
        f, #game.config, #game.vars, #game.monitor, #game.rules, #steps))
end

-- ================================================================
-- std library: the two helpers whose gates decide whether a player
-- respawns at all, checked directly rather than through a game file.
-- ================================================================
do
  local std = la.lib("std")

  local ready
  local respawn = std.base_respawn{
    when     = function() return true end,
    team     = function() return 0 end,
    teamless = true,
    rssi     = -57,
    on_ready = function() ready = true end,
  }

  local function try(fields)
    ready = false
    local sub = respawn({}, mk_pkt(fields))
    return ready, sub
  end

  local near_ok, sub = try{ payload = { 0 }, rssi = -40 }
  if not (near_ok and sub == 1) then
    failures = failures + 1
    print("  FAIL std          base_respawn: own base in range must arm respawn")
  end

  -- The gate that was inert while pkt.rssi always read 0 dBm.
  if try{ payload = { 0 }, rssi = -80 } then
    failures = failures + 1
    print("  FAIL std          base_respawn: out-of-range base must be ignored")
  end

  local teamless_ok = try{ payload = { 0xFF }, rssi = -40 }
  if not teamless_ok then
    failures = failures + 1
    print("  FAIL std          base_respawn: teamless base must be accepted")
  end

  if try{ payload = { 1 }, rssi = -40 } then
    failures = failures + 1
    print("  FAIL std          base_respawn: enemy base must be ignored")
  end

  -- The gate belongs to the ruleset: the library refuses to invent a range.
  if pcall(std.base_respawn, { team = function() return 0 end,
                               on_ready = function() end }) then
    failures = failures + 1
    print("  FAIL std          base_respawn accepted a missing rssi gate")
  end
  if pcall(std.pickup_claim, {}) then
    failures = failures + 1
    print("  FAIL std          pickup_claim accepted a missing rssi gate")
  end

  -- pickup_claim answers only from inside its gate, and only a ready totem.
  local claim = std.pickup_claim{ rssi = -57 }
  if claim({}, mk_pkt{ payload = { 0 }, rssi = -80 }) ~= nil then
    failures = failures + 1
    print("  FAIL std          pickup_claim answered an out-of-range totem")
  end
  if claim({}, mk_pkt{ payload = { 1 }, rssi = -40 }) ~= nil then
    failures = failures + 1
    print("  FAIL std          pickup_claim answered a totem on cooldown")
  end
  if claim({}, mk_pkt{ payload = { 0 }, rssi = -40 }) ~= la.my_id() then
    failures = failures + 1
    print("  FAIL std          pickup_claim did not claim in range")
  end

  -- lit_target's on_shone fires only on the hit that empties the lives.
  local shot_by = nil
  local ladder = std.lit_target{
    lives = "lives", immunity = std.immunity(0),
    reply = { taken = 1, shone = 2, friend = 4, immune = 5 },
    on_shone = function(_, pkt) shot_by = pkt.sender end,
  }
  local v = { lives = 2 }
  ladder(v, mk_pkt{ sender = 7, payload = { 1 } })
  if shot_by ~= nil then
    failures = failures + 1
    print("  FAIL std          lit_target: on_shone fired while lives remained")
  end
  ladder(v, mk_pkt{ sender = 7, payload = { 1 } })
  if shot_by ~= 7 then
    failures = failures + 1
    print("  FAIL std          lit_target: on_shone did not name the shooter")
  end
  print("OK   std           proximity gates, pickup claim, lit_target on_shone")
end

print("\nTotemVM encoded program sizes (bytes, single-packet budget = 225):")
local keys = {}
for k in pairs(totem_sizes) do keys[#keys+1] = k end
table.sort(keys)
for _, k in ipairs(keys) do print(string.format("  %-24s %3d", k, totem_sizes[k])) end

print(failures == 0 and "\nALL GAMES PASS" or ("\n" .. failures .. " FAILURES"))
os.exit(failures == 0 and 0 or 1)
