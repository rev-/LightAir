-- Test harness: stub `la` kernel + smoke-test every game file.
-- Run from the repo root:  lua5.4 <this file>

local ROOT = "games/"
local clock = 0

la = {
  msg = { LIT = 0x10, SCORE_COLLECT = 0x12, POINT_REPORT = 0x14, SPLASH = 0x18,
          FLAG_EVENT = 0x50, CP_BEACON = 0x52, CP_SCORE = 0x54,
          BASE_BEACON = 0x56, FLAG_BEACON = 0x58,
          BONUS_BEACON = 0x5E, MALUS_BEACON = 0x60 },
  flag_event = { TAKEN = 1, DROPPED = 2, SCORED = 3 },
  -- Mirrors IconType in src/ui/player/display/LightAir_Display_Icons.h.
  -- A profile naming an icon that is not here fails the standard-catalogue
  -- check below, which is what catches this stub drifting from the enum.
  icons = { LIGHT = 0, LIFE = 1, FLAG = 2, HOURGLASS = 3, SCORE = 4,
            ROLE = 5, ENERGY = 6, DOWN = 7, SPLASH = 8,
            FAST = 9, LONG = 10, STRONG = 11, TIME = 3 },
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
function la.key_down(k, pad) return false end
function la.key_state(k, pad) return "off" end
function la.key_at(i) return nil end
-- Enlight stub.  run() refuses while a burst is still in flight, exactly as
-- the real Enlight::run() does — that refusal is the whole reason energy is
-- spent inside the `and la.shine()` short-circuit rather than beside it, so a
-- stub that always accepted would let that bug back in unnoticed.
local shine_busy_until = 0
local shine_burst_ms   = 100
local shine_result     = { status = "no_hit", id = 0, metres = 0, r = 0.5, ang = 0.5 }
function la.shine()
  if clock < shine_busy_until then return false end
  shine_busy_until = clock + shine_burst_ms
  return true
end
function la.shine_lit() return nil end
function la.shine_ms() return shine_burst_ms end
function la.shine_config(t) end
function la.shine_action(spec) end
function la.shine_result()
  return shine_result.status, shine_result.id, shine_result.metres,
         shine_result.r, shine_result.ang
end
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

local files = { "freeforall", "teams", "flag", "kingofhill", "outflow", "upkeep",
                "virus", "festasportsasso" }
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
  assert(game.initial_state ~= nil, f .. ": no initial_state")
  -- scoring_state is optional: a game with no ending (festasportsasso)
  -- omits it and the binding then never enters score collection.
  assert(game.scoring_state == nil or type(game.scoring_state) == "number",
         f .. ": bad scoring_state")
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
  -- A hit weighs what the shooter's projector says, in standard hits, and
  -- an empty payload still counts as one so a game that sends none keeps
  -- working against one that does.
  local strong = std.lit_target{
    lives = "lives", immunity = std.immunity(0),
    reply = { taken = 1, shone = 2, friend = 4, immune = 5 },
  }
  local sv = { lives = 5 }
  strong(sv, mk_pkt{ sender = 7, payload = { 3 } })
  if sv.lives ~= 2 then
    failures = failures + 1
    print("  FAIL std          lit_target: strength 3 took " .. (5 - sv.lives) .. " lives")
  end
  local ev = { lives = 5 }
  strong(ev, mk_pkt{ sender = 7 })
  if ev.lives ~= 4 then
    failures = failures + 1
    print("  FAIL std          lit_target: an empty payload did not count as one hit")
  end
  -- Lives never go negative, whatever the shooter claims.
  local ov = { lives = 1 }
  if strong(ov, mk_pkt{ sender = 7, payload = { 9 } }) ~= 2 or ov.lives ~= 0 then
    failures = failures + 1
    print("  FAIL std          lit_target: an overkill hit did not settle at zero/shone")
  end

  -- The RSSI gate is only honoured by a ruleset that also says how to
  -- report the refusal: a silent decline reads as broken hardware.
  local gated = std.lit_target{
    lives = "lives", immunity = std.immunity(0),
    reply = { taken = 1, shone = 2, friend = 4, immune = 5, far = 6 },
  }
  local gv = { lives = 5 }
  if gated(gv, mk_pkt{ sender = 7, rssi = -80, payload = { 1, 0, 0, 55 } }) ~= 6
     or gv.lives ~= 5 then
    failures = failures + 1
    print("  FAIL std          lit_target: a hit beyond the shooter's gate was absorbed")
  end
  if gated(gv, mk_pkt{ sender = 7, rssi = -40, payload = { 1, 0, 0, 55 } }) ~= 1 then
    failures = failures + 1
    print("  FAIL std          lit_target: a hit inside the gate was refused")
  end
  local ungated = { lives = 5 }
  if strong(ungated, mk_pkt{ sender = 7, rssi = -80, payload = { 1, 0, 0, 55 } }) ~= 1 then
    failures = failures + 1
    print("  FAIL std          lit_target: gated without a `far` reply to report it")
  end

  print("OK   std           proximity gates, pickup claim, on_shone, absorption, rssi gate")
end

-- ================================================================
--   projector.lua
-- ================================================================
do
  local function fail(what, msg)
    failures = failures + 1
    print(string.format("  FAIL %-12s %s: %s", "projector", what, msg))
  end
  local function check(cond, what, msg) if not cond then fail(what, msg) end end

  local function fresh(decl)
    -- Each case gets its own module instance: the projector holds the
    -- inventory in upvalues, so a shared one would leak state between cases.
    package.loaded_projector = nil
    local P = dofile(ROOT .. "lib/projector.lua")
    P.define(decl)
    return P
  end

  local BASE_VARS = { energy = "energy", spent = "energy_spent" }
  -- The standard catalogue, read once from a throwaway instance.
  local P_STANDARD = dofile(ROOT .. "lib/projector.lua").standard
  local function mk_vars(e, recharge)
    return { energy = e, energy_spent = 0, start_energy = e,
             recharge_secs = recharge or 10 }
  end

  -- ---- the baseline reproduces std.shiner ------------------------
  do
    clock, shine_busy_until = 0, 0
    local P = fresh{ vars = BASE_VARS }
    local v = mk_vars(50)
    P.reset(v)
    check(v.energy == 50, "baseline", "reset did not fill the pool from start_energy")

    -- Trigger held down across many ticks: one beam, one energy.  Ten ticks
    -- inside one 100 ms burst is exactly the shape of the old 8-energy bug.
    la.trigger_down = function() return true end
    for _ = 1, 10 do P.tick(v); clock = clock + 10 end
    check(v.energy == 49, "baseline",
          "held trigger cost " .. (50 - v.energy) .. " energy, expected 1")
    check(v.energy_spent == 1, "baseline",
          "spent counter = " .. v.energy_spent .. ", expected 1")

    -- Past the burst, the next tick is allowed to fire again.
    clock = clock + 200
    P.tick(v)
    check(v.energy == 48, "baseline", "a second beam was refused after the burst")
  end

  -- ---- refill waits for the release, not for the pool hitting 0 ---
  do
    clock, shine_busy_until = 0, 0
    local P = fresh{ vars = BASE_VARS }
    local v = mk_vars(1, 10)          -- one shot, 10 s recharge
    P.reset(v)

    la.trigger_down = function() return true end
    P.tick(v)
    check(v.energy == 0, "refill", "the only beam did not empty the pool")

    -- Still holding, well past the recharge time: nothing comes back,
    -- because the clock has not started.
    clock = clock + 30000
    P.tick(v)
    check(v.energy == 0, "refill", "refilled while the trigger was still down")

    -- Release, then wait it out.
    la.trigger_down = function() return false end
    P.tick(v)
    clock = clock + 9000;  P.tick(v)
    check(v.energy == 0, "refill", "refilled before the delay elapsed")
    clock = clock + 2000;  P.tick(v)
    check(v.energy == 1, "refill", "did not refill after the delay")
  end

  -- ---- pressing an empty trigger neither restarts nor blocks the wait
  do
    clock, shine_busy_until = 0, 0
    local P = fresh{ vars = { energy = "energy", spent = "energy_spent",
                              reload = "reload", reload_ms = "reload_ms" } }
    local v = mk_vars(1, 10)
    v.reload, v.reload_ms = 0, 0
    P.reset(v)

    la.trigger_down = function() return true end
    P.tick(v)                                   -- the only beam empties the pool
    la.trigger_down = function() return false end
    clock = clock + 100;  P.tick(v)             -- release: the wait starts here
    local anchor = v.reload
    check(anchor == clock, "empty-press", "the wait did not anchor at the release")

    -- Now lean on a dead trigger for most of the wait: presses and releases
    -- that spend nothing must not push the refill further out...
    for _ = 1, 4 do
      la.trigger_down = function() return true end
      clock = clock + 1000;  P.tick(v)
      la.trigger_down = function() return false end
      clock = clock + 1000;  P.tick(v)
    end
    check(v.reload == anchor, "empty-press",
          "an empty press re-anchored the wait: " .. tostring(v.reload) ..
          " instead of " .. tostring(anchor))
    check(v.energy == 0, "empty-press", "refilled early")

    -- ...and must not hold it back either: the energy arrives on time even
    -- with the trigger held down across the moment it is due.
    la.trigger_down = function() return true end
    clock = anchor + 10000;  P.tick(v)
    check(v.energy == 1, "empty-press",
          "a held trigger blocked the refill that was due")
  end

  -- ---- ramp climbs one unit at a time ----------------------------
  do
    clock, shine_busy_until = 0, 0
    local P = fresh{ vars = BASE_VARS,
                     profiles = { { id = 0, max_energy = 4, recharge = "ramp",
                                    recharge_delay_ms = 1000, recharge_ms = 4000 } } }
    local v = { energy = 0, energy_spent = 0 }
    P.reset(v)
    v.energy = 0
    la.trigger_down = function() return false end
    clock = clock + 1000;  P.tick(v)      -- delay elapsed, ramp starts
    clock = clock + 1000;  P.tick(v)
    check(v.energy > 0 and v.energy < 4, "ramp",
          "expected a partial pool mid-ramp, got " .. v.energy)
    clock = clock + 5000;  P.tick(v)
    check(v.energy == 4, "ramp", "ramp did not reach full, got " .. v.energy)
  end

  -- ---- the reload bar's clock is the release, not the zero-crossing
  do
    clock, shine_busy_until = 0, 0
    local P = fresh{ vars = { energy = "energy", spent = "energy_spent",
                              reload = "reload", reload_ms = "reload_ms" } }
    local v = mk_vars(1, 10)
    v.reload, v.reload_ms = 0, 0
    P.reset(v)

    la.trigger_down = function() return true end
    P.tick(v)                              -- empties the pool
    local zero_at = clock
    clock = clock + 5000;  P.tick(v)       -- still held
    check(v.reload == 0, "bar",
          "the bar clock started at the zero-crossing, not the release")

    la.trigger_down = function() return false end
    local release = clock
    P.tick(v)
    check(v.reload == release, "bar",
          "the bar clock is " .. tostring(v.reload) .. ", expected the release " ..
          tostring(release))
    check(v.reload_ms == 10000, "bar",
          "fill duration = " .. tostring(v.reload_ms) .. " ms, expected 10000")
    check(zero_at ~= release, "bar", "test is degenerate: no hold before release")

    -- The clock is an ANCHOR, not a running value: it must keep reading the
    -- release instant as time passes, or the bar would never appear to fill.
    clock = clock + 3000;  P.tick(v)
    check(v.reload == release, "bar",
          "the bar clock moved to " .. tostring(v.reload) ..
          "; it must stay at the release instant " .. tostring(release))

    -- Pressing again does NOT abandon the reload.  An empty trigger cannot
    -- fire, so it has nothing to restart the wait with, and the bar must
    -- keep showing the wait that is genuinely still running.
    la.trigger_down = function() return true end
    clock = clock + 100;  P.tick(v)
    check(v.reload == release, "bar",
          "a re-press moved the reload bar to " .. tostring(v.reload) ..
          "; the wait is still the one anchored at " .. tostring(release))
  end

  -- ---- inventory: FIFO eviction, and the baseline is structural ---
  do
    clock, shine_busy_until = 0, 0
    local P = fresh{ vars = BASE_VARS, max_owned = 2,
                     profiles = { { id = 1, name = "A", max_energy = 5 },
                                  { id = 2, name = "B", max_energy = 5 },
                                  { id = 3, name = "C", max_energy = 5 } } }
    local v = mk_vars(50)
    P.reset(v)

    la.trigger_down = function() return false end
    P.give(v, 1); clock = clock + 10
    P.give(v, 2); clock = clock + 10
    check(P.owned_count() == 3, "inventory",
          "expected baseline + 2 powered, got " .. P.owned_count())
    P.give(v, 3)
    check(P.owns(1) == false, "inventory", "FIFO kept the oldest projector")
    check(P.owns(2) and P.owns(3), "inventory", "FIFO evicted the wrong slot")
    check(P.consume_evicted() == "A", "inventory", "eviction did not name what it dropped")
    check(P.owns(0), "inventory", "the baseline was evicted")
    check(P.drop(v, 0) == false, "inventory", "the baseline was droppable")

    -- Dropping the projector in hand falls back to the baseline.
    P.select(v, 3)
    P.drop(v, 3)
    check(P.active_id() == 0, "inventory", "dropping the active one did not fall back")
  end

  -- ---- range policy gates, and fails open when uncalibrated -------
  do
    clock, shine_busy_until = 0, 0
    local P = fresh{ vars = BASE_VARS,
                     profiles = { { id = 0, range_m = 10 } } }
    local v = mk_vars(50)
    P.reset(v)

    shine_result = { status = "player", id = 7, metres = 4, r = 0, ang = 0 }
    check(P.result(v) == 7, "range", "a target inside range was rejected")

    shine_result.metres = 25
    local id, why = P.result(v)
    check(id == nil and why == "far", "range", "a target beyond range was accepted")

    -- No reference calibration: metres is 0 and the gate must not fire.
    shine_result.metres = 0
    check(P.result(v) == 7, "range", "an uncalibrated device gated on distance")

    shine_result = { status = "no_hit", id = 0, metres = 0, r = 0, ang = 0 }
    check(P.result(v) == nil, "range", "a miss returned a target")
  end

  -- ---- payload carries strength, id, role and the rssi gate -------
  do
    local P = fresh{ vars = BASE_VARS,
                     profiles = { { id = 1, name = "S", strength = 3, role_tag = 2,
                                    rssi_min = -55, max_energy = 5 } } }
    local v = mk_vars(50)
    P.reset(v)
    la.trigger_down = function() return false end
    P.grant(v, 1)
    local strength, id, role, gate = P.payload(v)
    check(strength == 3 and id == 1 and role == 2 and gate == 55, "payload",
          string.format("got %s/%s/%s/%s, expected 3/1/2/55",
                        tostring(strength), tostring(id), tostring(role), tostring(gate)))
  end

  -- ---- clamps bite at load ---------------------------------------
  do
    local P = fresh{ vars = BASE_VARS,
                     profiles = { { id = 1, cycles = 9999, strength = 99,
                                    cooldown_ms = -5 } } }
    local v = mk_vars(50)
    P.reset(v)
    la.trigger_down = function() return false end
    P.grant(v, 1)
    local p = P.active_profile()
    check(p.cycles == 100, "clamp", "cycles = " .. tostring(p.cycles) .. ", expected 100")
    check(p.strength == 10, "clamp", "strength = " .. tostring(p.strength) .. ", expected 10")
    check(p.cooldown_ms == 0, "clamp", "cooldown_ms = " .. tostring(p.cooldown_ms))
  end

  -- ---- splash: the attacker's reach, the victim's beacon -----------
  do
    clock, shine_busy_until = 0, 0
    local P = fresh{ vars = BASE_VARS,
                     profiles = { { id = 1, name = "BOOM", max_energy = 5,
                                    splash = { on = "lit", strength = 2, rssi = -60,
                                               bands = { { -55, 2 }, { -70, 1 } } } },
                                  { id = 2, name = "QUIET", max_energy = 5 } } }
    local v = mk_vars(50)
    P.reset(v)
    la.trigger_down = function() return false end

    -- Victim side: a LIT from projector 1 relays that projector's reach.
    local n0 = #out.radio
    local lit = mk_pkt{ sender = 9, payload = { 1, 1, 0, 0 } }   -- [strength, proj, role, gate]
    check(P.emit_splash(v, lit, "lit"), "splash", "a splash profile did not emit")
    local sent = out.radio[#out.radio]
    check(sent and sent[2] == la.msg.SPLASH, "splash", "the beacon was not MSG.SPLASH")
    check(sent and sent[1] == "bcast", "splash",
          "the beacon was relayed; splash must be single-hop")
    -- The gate on the wire is the OUTERMOST band (-70), not the flat rssi
    -- (-60): bands are the reach when a profile declares them, and the byte
    -- is what a bystander without the profile falls back to, so it has to
    -- admit everyone the bands would.
    check(sent and sent[3] == 1 and sent[4] == 2 and sent[5] == 70
          and sent[6] == 1 and sent[7] == 9, "splash",
          "beacon payload wrong: " .. table.concat({ tostring(sent[3]), tostring(sent[4]),
              tostring(sent[5]), tostring(sent[6]), tostring(sent[7]) }, "/"))

    -- A projector with no splash stays silent.
    clock = clock + 1000
    local n1 = #out.radio
    P.emit_splash(v, mk_pkt{ sender = 9, payload = { 1, 2, 0, 0 } }, "lit")
    check(#out.radio == n1, "splash", "a projector with no splash still emitted")

    -- splash.on gates the event: this profile answers "lit", not "shone".
    clock = clock + 1000
    local n2 = #out.radio
    P.emit_splash(v, lit, "shone")
    check(#out.radio == n2, "splash", "emitted on an event the profile does not answer")

    -- Rate limit: two calls for one event produce one beacon.
    clock = clock + 1000
    local n3 = #out.radio
    P.emit_splash(v, lit, "lit")
    P.emit_splash(v, lit, "lit")
    check(#out.radio == n3 + 1, "splash", "the rate limit let a second beacon through")

    -- Bystander side, graded by distance.
    local near  = mk_pkt{ sender = 9, rssi = -50, payload = { 1, 2, 60, 1, 4 } }
    local mid   = mk_pkt{ sender = 9, rssi = -65, payload = { 1, 2, 60, 1, 4 } }
    local far   = mk_pkt{ sender = 9, rssi = -90, payload = { 1, 2, 60, 1, 4 } }
    check(P.on_splash(v, near) == 2, "splash", "a close bystander took the wrong band")
    check(P.on_splash(v, mid)  == 1, "splash", "a mid-range bystander took the wrong band")
    check(P.on_splash(v, far)  == nil, "splash", "a distant bystander was hit")

    -- THE cascade guard: a beacon that is not a direct optical hit is
    -- dropped, so one beam cannot chain across a field.
    local relayed = mk_pkt{ sender = 9, rssi = -50, payload = { 1, 2, 60, 0, 4 } }
    local absorbed, why = P.on_splash(v, relayed)
    check(absorbed == nil and why == "cascade", "splash",
          "a non-direct beacon was absorbed: splash can cascade")

    -- And a player never splashes themselves.
    local mine = mk_pkt{ sender = la.my_id(), rssi = -40, payload = { 1, 2, 60, 1, 4 } }
    check(P.on_splash(v, mine) == nil, "splash", "the emitter splashed itself")
  end

  -- ---- a retired field name is named, not silently ignored --------
  do
    local ok = pcall(function()
      fresh{ vars = BASE_VARS,
             profiles = { { id = 1, name = "OLD", recharge_delay_secs = 5 } } }
    end)
    check(not ok, "retired",
          "a profile using the old seconds field loaded quietly; it would " ..
          "have got a zero delay and never waited")
  end

  -- ---- the standard SPLASH profile -------------------------------
  do
    clock, shine_busy_until = 0, 0
    local P = fresh{ vars = { energy = "energy", spent = "energy_spent",
                              icon = "proj_icon", name = "proj_name" },
                     profiles = { P_STANDARD.SPLASH } }
    local v = mk_vars(50)
    v.proj_icon, v.proj_name = 0, ""
    P.reset(v)
    la.trigger_down = function() return false end

    local S = P_STANDARD.SPLASH
    -- Everything a projector needs to be playable, not just a name.
    for _, field in ipairs({ "id", "name", "icon", "cycles", "cooldown_ms",
                             "range_m", "cost", "max_energy", "recharge",
                             "recharge_delay_ms", "ready_ms", "strength",
                             "target_immunity_ms", "splash", "shine_action" }) do
      check(S[field] ~= nil, "SPLASH", "the standard profile declares no " .. field)
    end
    check(S.splash.bands and #S.splash.bands >= 2, "SPLASH",
          "the splash profile has no graded bands")

    -- The whole standard catalogue has to be playable and self-consistent.
    local seen_id, seen_name = {}, {}
    for key, prof in pairs(P_STANDARD) do
      for _, field in ipairs({ "id", "name", "icon", "cycles", "cooldown_ms",
                               "range_m", "cost", "max_energy", "recharge",
                               "ready_ms", "strength", "shine_action" }) do
        check(prof[field] ~= nil, "standard", key .. " declares no " .. field)
      end
      check(prof.id ~= 0, "standard", key .. " claims the baseline id")
      check(not seen_id[prof.id], "standard",
            key .. " reuses standard id " .. tostring(prof.id) ..
            " — ids travel on the wire and must be unique")
      seen_id[prof.id] = key
      check(not seen_name[prof.name], "standard", key .. " reuses a name")
      seen_name[prof.name] = true
      check(la.icons[prof.icon] ~= nil, "standard",
            key .. " names an icon the firmware does not carry: " .. tostring(prof.icon))
      check(#prof.name <= 8, "standard", key .. "'s name will not fit the cell")

      -- Shine feedback: the burst governs the action's TOTAL length and the
      -- declared ms are a shape, so several notes are fine and are in fact
      -- how a player tells two projectors apart.  What is not fine is a
      -- shape of all zeros, which throws the ratio away.
      local steps = prof.shine_action.steps
      check(steps and #steps >= 1 and #steps <= 4, "standard",
            key .. "'s shine_action needs 1..4 steps, has " ..
            tostring(steps and #steps))
      local shape = 0
      for _, st in ipairs(steps or {}) do shape = shape + (st.ms or 0) end
      check(shape > 0, "standard", key .. "'s shine_action declares no shape at all")

      -- A ramp needs a duration to ramp over, or it would never fill.
      if prof.recharge == "ramp" then
        check((prof.recharge_ms or 0) > 0, "standard",
              key .. " ramps but declares no recharge_ms")
      end
    end

    -- Holding it puts its identity on the LCD.
    P.grant(v, S.id)
    check(v.proj_name == "SPLASH", "SPLASH",
          "the name did not reach the display var: " .. tostring(v.proj_name))
    check(v.proj_icon == la.icons.SPLASH, "SPLASH",
          "the icon did not reach the display var: " .. tostring(v.proj_icon))

    -- Switching back to the baseline restores the baseline's identity, so
    -- the cell never shows the icon of a projector no longer in hand.
    P.select(v, 0)
    check(v.proj_icon == la.icons.ENERGY, "SPLASH",
          "the icon stayed on SPLASH after switching away")

    -- It is the ONLY profile that splashes: a field where everything
    -- splashed would be chaos rather than tactics.
    check(P_STANDARD.SPLASH.splash ~= nil, "SPLASH", "SPLASH lost its splash")
    local others = 0
    for name, prof in pairs(P_STANDARD) do
      if name ~= "SPLASH" and prof.splash then others = others + 1 end
    end
    check(others == 0, "SPLASH", others .. " other standard profiles declare a splash")
  end

  la.trigger_down = function(n) return false end
  print("OK   projector     baseline=shiner, refill/ramp, bar clock, FIFO, range, payload, clamps, splash, SPLASH profile")
end

-- ================================================================
--   The projector is the ONLY route to Enlight.
--
--   A ruleset that starts or polls a burst itself bypasses everything the
--   projector exists to own: the energy a beam costs, the reach, what the
--   hit weighs on the wire, and the splash.  Worse, la.shine_result() and
--   la.shine_lit() both poll, and the poll is read-and-clear — a game
--   calling one while the projector calls the other would eat measurements
--   at random.  So the raw optics verbs belong to games/lib/projector.lua
--   and to nothing else.
-- ================================================================
do
  local RAW = { "la%.shine%s*%(", "la%.shine_lit%s*%(", "la%.shine_result%s*%(",
                "la%.shine_config%s*%(", "la%.shine_ms%s*%(" }
  local checked = 0
  for _, f in ipairs(files) do
    local fh = assert(io.open(ROOT .. f .. ".lua", "r"))
    local src = fh:read("a"); fh:close()
    checked = checked + 1
    for _, pat in ipairs(RAW) do
      -- Ignore comment lines: the ban is on calling, not on explaining.
      for line in src:gmatch("[^\n]+") do
        if not line:match("^%s*%-%-") and line:match(pat) then
          failures = failures + 1
          print(string.format("  FAIL %-12s reaches Enlight directly: %s",
                              f, line:match("^%s*(.-)%s*$")))
        end
      end
    end
  end
  print(string.format("OK   optics        %d game files go through the projector, none direct",
                      checked))
end

print("\nTotemVM encoded program sizes (bytes, single-packet budget = 225):")
local keys = {}
for k in pairs(totem_sizes) do keys[#keys+1] = k end
table.sort(keys)
for _, k in ipairs(keys) do print(string.format("  %-24s %3d", k, totem_sizes[k])) end

print(failures == 0 and "\nALL GAMES PASS" or ("\n" .. failures .. " FAILURES"))
os.exit(failures == 0 and 0 or 1)
