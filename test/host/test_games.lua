-- Test harness: stub `la` kernel + smoke-test every game file.
-- Run from the repo root:  lua5.4 <this file>

local ROOT = "games/"
local clock = 0

la = {
  msg = { LIT = 0x10, SCORE_COLLECT = 0x12, POINT_REPORT = 0x14, AREA = 0x18,
          FLAG_EVENT = 0x50, CP_BEACON = 0x52, CP_SCORE = 0x54,
          BASE_BEACON = 0x56, FLAG_BEACON = 0x58,
          BONUS_BEACON = 0x5E, MALUS_BEACON = 0x60 },
  flag_event = { TAKEN = 1, DROPPED = 2, SCORED = 3 },
  hit = { TAKEN = 1, SHONE = 2 },         -- HitReply in src/config.h
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
-- 1 = battery volts, 2/3 = NTC degrees; nil past the end of the list.
function la.sensor(n) return ({ 4.05, 31.0, 29.5 })[n or 1] end
function la.player_short(id) return "P" .. tostring(id) end
function la.team_short(t) return ({ [0] = "O", [1] = "X" })[t] or "?" end
function la.totem_for_role(role, i) return i == 0 and 254 or 0 end
-- The DM's per-totem option (Totems submenu, O key): [totem id] = label.
-- The index is not checked by any game, so the stub returns 1 for a hit.
local totem_opts = {}
function la.totem_option(id)
  local label = totem_opts[id]
  if label then return 1, label end
  return 0, nil
end
function la.trigger_down(n) return false end
-- The same ladder the keypad reports on: "pressed" appears in exactly one
-- poll, which is what a ruleset reads a press EDGE from (TiroBersaglio's
-- reload) instead of holding the trigger down.
function la.trigger_state(n) return "off" end
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

-- The area service is firmware (LightAir_GameRunner, tested in the luagame
-- suite).  Here the two verbs only record what a library declared or a
-- ruleset emitted, for the checks below.
area_policies = {}
function la.area_policy(id, spec) area_policies[id] = spec end
function la.area_emit(id) out.radio[#out.radio+1] = { "area", id }; return true end

-- fake packet proxy
local function mk_pkt(fields)
  local p = { sender = fields.sender or 3, team = fields.team or 1,
              rssi = fields.rssi or -40, msg = fields.msg, area = fields.area,
              len = fields.payload and #fields.payload or 0 }
  function p.byte(self, i) return fields.payload[i] end
  return p
end

-- A var's value at on_begin: its default, or for draw = "player" the
-- player the DM drew at Start (player 1 here; this device is player 2).
drawn_player = 1
function initial(x)
  if x.draw == "player" then return drawn_player end
  return x.default
end

local files = { "freeforall", "teams", "flag", "kingofhill", "outflow", "upkeep",
                "virus",
                -- Not flashed: games/custom/ is uploaded over HTTP (Settings
                -- -> Share games) to the stands that need it.  Still tested.
                "custom/festasportsasso", "custom/tirobersaglio" }
local failures = 0
local totem_sizes = {}

for _, f in ipairs(files) do
  local ok, game = pcall(dofile, ROOT .. f .. ".lua")
  assert(ok, f .. ": load failed: " .. tostring(game))

  -- build the vars "proxy" (plain table with declared defaults)
  vars = {}
  for _, c in ipairs(game.config) do vars[c.id] = c.default end
  for _, v in ipairs(game.vars) do vars[v.id] = initial(v) end

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

  -- Respawn bar: every ruleset with a respawn wait shows a bar filling
  -- over it in the state where the player waits, anchored on the instant
  -- the wait began — and every rule that enters that state starts it.
  local has_respawn = false
  for _, c in ipairs(game.config) do
    if c.id == "respawn_secs" then has_respawn = true end
  end
  if has_respawn then
    local bar
    for _, m in ipairs(game.monitor) do
      if m.bar and m.fill_var == "respawn_ms" then bar = m end
    end
    step("respawn bar", function()
      assert(bar, "no bar row fills over respawn_ms")
      assert(bar.start_var == "respawn_from", "the respawn bar is not anchored on respawn_from")
      local in_bar = {}
      for _, st in ipairs(bar.states) do in_bar[st] = true end
      local entered = 0
      for i, r in ipairs(game.rules) do
        if in_bar[r.to] and not in_bar[r.from] and r.action then
          vars.respawn_ms, vars.respawn_from = 0, 0
          vars.respawn_secs = 25
          clock = clock + 1234
          r.action(vars)
          assert(vars.respawn_ms == 25000 and vars.respawn_from == clock
                 and vars.respawn_zero == 0,
                 "rule " .. i .. " enters the wait without starting its bar (" ..
                 tostring(vars.respawn_ms) .. " ms from " .. tostring(vars.respawn_from) .. ")")
          entered = entered + 1
        end
      end
      assert(entered > 0, "no rule enters the respawn bar's state")
    end)
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

  -- ---- ramp policy: the bar covers the idle, the trickle starts after it
  -- FAST against a 30-energy, 10 s baseline: 30 energy, an idle of
  -- 10000/2 - 500 = 4500 ms, then one unit every 10 ms.  Emptied and
  -- released, it must show a bar for the idle alone, give back nothing
  -- during it, then exactly one unit at its end and one per 10 ms after —
  -- not the lump "earned" since the last beam.
  do
    clock, shine_busy_until = 0, 0
    local P = fresh{ vars = { energy = "energy", spent = "energy_spent",
                              reload = "reload", reload_ms = "reload_ms" } }
    local v = { energy = 0, energy_spent = 0, start_energy = 30, recharge_secs = 10,
                reload = 0, reload_ms = 0 }
    P.reset(v)
    P.grant(v, 2)                                   -- FAST
    la.trigger_down = function() return true end
    for _ = 1, 400 do
      if v.energy == 0 then break end
      P.tick(v); clock = clock + 150
    end
    check(v.energy == 0, "ramp", "could not empty FAST")
    clock = clock + 2000;  P.tick(v)                -- a dead trigger, held
    la.trigger_down = function() return false end
    local release = clock
    P.tick(v)
    check(v.reload == release and v.reload_ms == 4500, "ramp",
          "FAST's bar is " .. tostring(v.reload_ms) .. " ms from " .. tostring(v.reload) ..
          "; expected the 4500 ms idle from the release")
    clock = release + 4499;  P.tick(v)
    check(v.energy == 0, "ramp", "energy came back during the idle: " .. v.energy)
    clock = release + 4500;  P.tick(v)
    check(v.energy == 1, "ramp", "the trickle did not start with one unit: " .. v.energy)
    clock = release + 4510;  P.tick(v)
    check(v.energy == 2, "ramp", "the trickle is not one unit per 10 ms: " .. v.energy)
    clock = release + 4500 + 290;  P.tick(v)
    check(v.energy == 30, "ramp", "the trickle did not refill at 10 ms a unit: " .. v.energy)
    la.trigger_down = function() return false end
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

  -- ---- shared pool: one pool for every projector held ---------------
  -- define{ shared_pool = true } (Outflow): the pool is the baseline's and
  -- the player's life.  A pickup must not fill it, a switch must not move
  -- it, a powered projector's own recharge must not run on it, DIM halves
  -- it, and each beam still costs what the projector in hand costs.
  do
    clock, shine_busy_until = 0, 0
    la.trigger_down = function() return false end
    local P = fresh{ vars = BASE_VARS, shared_pool = true,
                     profiles = { { id = 0, recharge = "none" },
                                  { id = 5, name = "HEAVY", max_energy = 100, cost = 2,
                                    strength = 3, recharge = "ramp",
                                    recharge_delay_ms = 0, recharge_step_ms = 10 } } }
    local v = mk_vars(50)
    P.reset(v)
    v.energy = 20                                   -- hurt
    P.grant(v, 5)
    check(P.active_id() == 5 and v.energy == 20, "shared",
          "a pickup moved the pool to " .. v.energy)
    check(P.max_energy(v) == 50, "shared", "the pool is not the baseline's: " .. P.max_energy(v))
    la.trigger_down = function() return true end
    P.tick(v)
    la.trigger_down = function() return false end
    clock = clock + 200; P.tick(v)                  -- released
    check(v.energy == 18 and v.energy_spent == 2, "shared",
          "a beam cost " .. (20 - v.energy) .. " of the pool, expected the projector's 2")
    check(P.payload(v) == 3, "shared", "the projector in hand lost its strength")
    clock = clock + 5000; P.tick(v)
    check(v.energy == 18, "shared",
          "the projector in hand's own ramp ran on the pool: " .. v.energy)
    P.give(v, 5)
    check(v.energy == 18, "shared", "a re-grant refilled the pool")
    P.select(v, 0); P.select(v, 5)
    check(v.energy == 18, "shared", "switching moved energy: " .. v.energy)
    v.energy = 40
    P.set_dim(v, true)
    check(v.energy == 25, "shared", "DIM clamped the pool to " .. v.energy .. ", expected 25")
    P.strip(v)
    check(P.active_id() == 0 and P.owned_count() == 1 and v.energy == 25, "shared",
          "going out kept the projector or moved the pool")
  end

  -- ---- recharge modes: refill, ramp, none — anything else refuses ---
  -- A mode the projector does not know would otherwise recharge as a ramp.
  do
    for _, prof in ipairs({ { id = 1, name = "X", recharge = "consumed" },
                            { id = 0, recharge = "consumed" },
                            { id = 2, name = "Y", recharge = "refil" },
                            { id = 3, name = "Z", recharge = function() end } }) do
      local ok, err = pcall(fresh, { vars = BASE_VARS, profiles = { prof } })
      check(not ok and tostring(err):find("recharge must be", 1, true), "recharge",
            "recharge = " .. tostring(prof.recharge) .. " was accepted")
    end
    local ok = pcall(fresh, { vars = BASE_VARS,
                              profiles = { { id = 1, name = "A", recharge = "ramp" },
                                           { id = 2, name = "B", recharge = "none" },
                                           { id = 0, recharge = "refill" } } })
    check(ok, "recharge", "a known recharge mode was refused")
  end

  -- ---- range is a label: nothing gates on distance ----------------
  do
    clock, shine_busy_until = 0, 0
    local P = fresh{ vars = BASE_VARS,
                     profiles = { { id = 0, range_m = 10 } } }
    local v = mk_vars(50)
    P.reset(v)

    shine_result = { status = "player", id = 7, metres = 4, r = 0, ang = 0 }
    local id, metres = P.result(v)
    check(id == 7 and metres == 4, "range", "a target inside the label was rejected")

    shine_result.metres = 25
    check(P.result(v) == 7, "range", "range_m gated a hit; it is a label only")

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

  -- ---- area: a profile's area is a policy for the firmware ---------
  -- The area service itself (trigger, bands, credit) is firmware and is
  -- tested through the real runner in the luagame suite.  What the
  -- projector owes it is the declaration, under the projector's own id.
  do
    area_policies = {}
    fresh{ vars = BASE_VARS,
           profiles = { { id = 12, name = "BOOM", max_energy = 5, role_tag = 7,
                          area = { on = "shone", bands = { { -60, 3 } }, friendly = "never",
                                   self = true, credit = false } },
                        { id = 13, name = "QUIET", max_energy = 5 } } }
    local a = area_policies[12]
    check(a and a.projector == 12 and a.on == "shone" and a.bands[1][1] == -60
          and a.bands[1][2] == 3 and a.friendly == "never" and a.self == true
          and a.credit == false and a.role_tag == 7, "area",
          "a profile's area did not reach la.area_policy as declared, under its id")
    check(area_policies[13] == nil, "area", "a profile without an area registered one")
    local s = area_policies[P_STANDARD.SPLASH.id]
    check(s and s.projector == P_STANDARD.SPLASH.id and s.on == "lit", "area",
          "the standard SPLASH is not registered as a projector-triggered area")
    local ok = pcall(function()
      fresh{ vars = BASE_VARS, profiles = { { id = 0, area = { bands = { { -60, 1 } } } } } }
    end)
    check(not ok, "area", "the baseline projector accepted an area")
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
                             "area", "shine_action" }) do
      check(S[field] ~= nil, "SPLASH", "the standard profile declares no " .. field)
    end
    check(S.area.bands and #S.area.bands >= 2, "SPLASH",
          "the SPLASH area has no graded bands")

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
        check(prof.recharge_step_ms ~= nil or (prof.recharge_ms or 0) ~= 0, "standard",
              key .. " ramps but declares neither recharge_step_ms nor recharge_ms")
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
    check(P_STANDARD.SPLASH.area ~= nil, "SPLASH", "SPLASH lost its area")
    local others = 0
    for name, prof in pairs(P_STANDARD) do
      if name ~= "SPLASH" and prof.area then others = others + 1 end
    end
    check(others == 0, "SPLASH", others .. " other standard profiles declare an area")
  end

  -- ---- the standard catalogue is stated relative to the baseline -----
  -- Resolved across the menu, so a strong baseline cannot out-shoot a
  -- "powered" projector built on fixed numbers.  The relations read the
  -- baseline of THEIR module instance, so S must come from the one defined.
  do
    local P = fresh{ vars = BASE_VARS }
    local S = P.standard
    local function r(p, field, v) return p[field](v) end
    for _, c in ipairs({ { 10, 5 }, { 30, 10 }, { 60, 20 }, { 25, 15 } }) do
      local e, secs = c[1], c[2]
      local v  = mk_vars(e, secs)
      local R  = secs * 1000
      local tag = string.format("pool %d / %d s: ", e, secs)
      -- FAST: the baseline's pool, half its cooldown, an idle of half its
      -- recharge less half a second, then 10 ms a unit.
      check(r(S.FAST, "max_energy", v) == e, "relations", tag .. "FAST pool")
      check(r(S.FAST, "cooldown_ms", v) == 25, "relations", tag .. "FAST cooldown")
      check(r(S.FAST, "recharge_delay_ms", v) == R // 2 - 500, "relations", tag .. "FAST idle")
      check(S.FAST.recharge == "ramp" and S.FAST.recharge_step_ms == 10,
            "relations", "FAST does not trickle at 10 ms a unit")
      check(r(S.FAST, "cycles", v) == 10, "relations", tag .. "FAST cycles")
      -- STRONG / SPLASH: half the pool, half the recharge, twice the cooldown.
      for _, k in ipairs({ "STRONG", "SPLASH" }) do
        check(r(S[k], "max_energy", v) == math.max(1, e // 2), "relations", tag .. k .. " pool")
        check(r(S[k], "recharge_delay_ms", v) == R // 2, "relations", tag .. k .. " recharge")
        check(r(S[k], "cooldown_ms", v) == 100, "relations", tag .. k .. " cooldown")
        check(r(S[k], "cycles", v) == 10, "relations", tag .. k .. " cycles")
      end
      check(S.STRONG.strength == 3, "relations", "STRONG does not weigh 3")
      check(r(S.SPLASH, "strength", v) == 1, "relations", tag .. "SPLASH strength")
      -- LONG: half the pool, the same recharge, five times the cycles,
      -- half the cooldown.
      check(r(S.LONG, "max_energy", v) == math.max(1, e // 2), "relations", tag .. "LONG pool")
      check(r(S.LONG, "recharge_delay_ms", v) == R, "relations", tag .. "LONG recharge")
      check(r(S.LONG, "cycles", v) == 50, "relations", tag .. "LONG cycles")
      check(r(S.LONG, "cooldown_ms", v) == 25, "relations", tag .. "LONG cooldown")
      check(r(S.LONG, "strength", v) == 1, "relations", tag .. "LONG strength")
      -- Every projector costs what the baseline costs.
      for k, prof in pairs(S) do
        check(r(prof, "cost", v) == 1, "relations", tag .. k .. " cost")
      end
    end

    -- They follow a game's own baseline, not the library's.
    local Q = fresh{ vars = BASE_VARS,
                     profiles = { { id = 0, cycles = 8, cooldown_ms = 80, max_energy = 7 } } }
    local v = mk_vars(50)
    local QS = Q.standard
    check(QS.LONG.cycles(v) == 40 and QS.FAST.cooldown_ms(v) == 40 and QS.STRONG.max_energy(v) == 3,
          "relations", "a game's own baseline did not reach the standard profiles")

    -- A pool never halves to nothing.
    local O = fresh{ vars = BASE_VARS, profiles = { { id = 0, max_energy = 1 } } }
    check(O.standard.STRONG.max_energy(mk_vars(1)) == 1, "relations", "a pool of 1 halved to 0")
  end

  -- ---- the baseline restores its own beam after a long one ----------
  do
    clock, shine_busy_until = 0, 0
    local P = fresh{ vars = BASE_VARS }
    local reps
    local real = la.shine_config
    la.shine_config = function(t) if t.reps then reps = t.reps end end
    local v = mk_vars(30)
    P.reset(v)
    check(reps == 10, "optics", "the baseline pushed " .. tostring(reps) .. " cycles, expected 10")
    P.grant(v, P.standard.LONG.id)
    check(reps == 50, "optics", "LONG pushed " .. tostring(reps) .. " cycles, expected 50")
    P.select(v, 0)
    check(reps == 10, "optics", "back on the baseline Enlight kept " .. tostring(reps) .. " cycles")
    la.shine_config = real
  end

  la.trigger_down = function(n) return false end
  print("OK   projector     baseline, refill/ramp, bar clock, FIFO, range label, payload, clamps, splash, SPLASH profile, BASE relations")
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

-- ================================================================
--   A+B belongs to the firmware
--
--   Held together, A and B open the in-game tools menu in every game and
--   every state (GameRunner, see src/game/LightAir_GameHold.h), before the
--   ruleset sees the keys.  A rule keyed on the same chord would fire in
--   the same breath the menu opens — FestaSportSasso's staff hand-over did
--   exactly that until it moved to < + >.  Any line that asks for both
--   keys is refused here.
-- ================================================================
do
  local checked = 0
  for _, f in ipairs(files) do
    local fh = assert(io.open(ROOT .. f .. ".lua", "r"))
    local src = fh:read("a"); fh:close()
    checked = checked + 1
    for line in src:gmatch("[^\n]+") do
      if not line:match("^%s*%-%-")
         and line:match('key_[%w_]+%s*%(%s*"A"') and line:match('key_[%w_]+%s*%(%s*"B"') then
        failures = failures + 1
        print(string.format("  FAIL %-12s reads the firmware's A+B chord: %s",
                            f, line:match("^%s*(.-)%s*$")))
      end
    end
  end
  print(string.format("OK   keys          %d game files leave the A+B menu chord alone", checked))
end

-- ================================================================
--   Every var role a projector is given must be DECLARED
--
--   proj.define{ vars = { spent = "energy_spent", ... } } names game vars
--   the projector writes.  On the device those writes go through the vars
--   proxy, and writing a name the ruleset never declared is a Lua error —
--   which aborts the whole update callback for that tick.  Since the
--   projector writes `spent` on the accepted-shine path, a missing
--   declaration reads to a player as a trigger that fires once and then
--   stops: energy drops, and the rest of the tick (recharge included)
--   never runs again.  virus.lua shipped exactly that.
--
--   The harness backs `vars` with a plain table, which accepts anything —
--   so nothing else here can catch it.  Read the declarations instead.
-- ================================================================
do
  local checked = 0
  for _, f in ipairs(files) do
    local fh = assert(io.open(ROOT .. f .. ".lua", "r"))
    local src = fh:read("a"); fh:close()

    -- The ids the ruleset declares, from its config and vars tables.
    local declared = {}
    for id in src:gmatch('{%s*id%s*=%s*"([%w_]+)"') do declared[id] = true end

    -- The var roles handed to proj.define{ vars = { ... } }.
    local block = src:match('proj%.define%s*{.-vars%s*=%s*{(.-)}')
    if block then
      checked = checked + 1
      for role, id in block:gmatch('([%w_]+)%s*=%s*"([%w_]+)"') do
        if not declared[id] then
          failures = failures + 1
          print(string.format("  FAIL %-12s projector var '%s' = \"%s\" is not declared",
                              f, role, id))
        end
      end
    end
  end
  print(string.format("OK   proj vars     %d rulesets declare every var their projector writes",
                      checked))
end

-- ================================================================
--   festasportsasso: the welcome screen is a shooting range
--
--   A visitor waiting for a BASE can pull the trigger to learn to aim.
--   Two things must hold, or the practice interferes with the stand:
--   the shots cost nothing (the queue can take as long as it likes), and
--   nothing goes on the radio (a turn already running must not be
--   touched by somebody practising beside it).
-- ================================================================
do
  local S_PRE, S_ACTIVE = 0, 1
  local function fail(what, msg)
    failures = failures + 1
    print(string.format("  FAIL %-12s %s: %s", "festa trial", what, msg))
  end
  local function check(cond, what, msg) if not cond then fail(what, msg) end end

  libcache = {}                       -- a projector of its own for this copy
  local g = dofile(ROOT .. "custom/festasportsasso.lua")
  local v = {}
  for _, c in ipairs(g.config) do v[c.id] = c.default end
  for _, x in ipairs(g.vars)   do v[x.id] = initial(x) end

  clock, shine_busy_until = 0, 0
  g.on_begin(v)

  -- Aim at a player and hold the trigger down across several bursts.
  la.trigger_down  = function() return true end
  shine_result.status, shine_result.id = "player", 3
  local energy0, spent0 = v.energy, v.energy_spent
  local radio0 = #out.radio
  local ui0    = #out.ui

  for _ = 1, 6 do g.update[S_PRE](v); clock = clock + 200 end

  check(v.energy == energy0, "trial",
        "practice cost energy: " .. tostring(energy0) .. " -> " .. tostring(v.energy))
  check(v.energy_spent == spent0, "trial", "practice moved the spent counter")
  check(#out.radio == radio0, "trial",
        (#out.radio - radio0) .. " radio message(s) escaped the welcome screen")
  check(#out.ui > ui0, "trial", "practice gave the player no feedback at all")
  check(v.batt ~= "--", "battery", "the welcome screen showed no battery reading")

  -- The contrast: once the turn starts, the same trigger costs energy and
  -- the hit does reach the target.  Proves the difference is the mode.
  for _, r in ipairs(g.rules) do
    if r.from == S_PRE and r.to == S_ACTIVE then r.action(v) end
  end
  clock = clock + 1000
  shine_busy_until = 0
  local e1, radio1 = v.energy, #out.radio
  g.update[S_ACTIVE](v)
  check(v.energy == e1 - 1, "in-game", "a turn's beam did not cost its energy")
  check(#out.radio > radio1, "in-game", "a turn's hit sent no MSG.LIT")

  la.trigger_down = function() return false end
  shine_result.status, shine_result.id = "no_hit", 0
  print("OK   festa trial   free shots, radio-silent, battery shown; "
        .. "the turn still costs and still sends")
end

-- ================================================================
--   tirobersaglio: the six panels, in order, on one magazine and a half
--
--   The rules that decide what a child at the stand experiences, none of
--   which the smoke test above can see:
--     * the welcome screen answers CLEAR and GREEN and NOTHING else, so
--       learning to aim cannot burn the run's own targets;
--     * the sequence advances only on the expected colour, and a RED
--       ends the turn keeping the targets but forfeiting the leftovers;
--     * a CLEAR refunds its beam -- including the LAST one, which is the
--       whole point of the grace window: the result comes back several
--       ticks after the pool hit zero;
--     * the reload is a press EDGE on the second trigger and only with
--       the magazine actually empty.
-- ================================================================
do
  local S_PRE, S_ACTIVE, S_END = 0, 1, 2
  local GREEN, YELLOW, BLUE, ORANGE = 2, 3, 4, 5
  local RED, LIME, MAGENTA, CLEAR   = 6, 7, 8, 1

  local function fail(what, msg)
    failures = failures + 1
    print(string.format("  FAIL %-12s %s: %s", "tirobersaglio", what, msg))
  end
  local function check(cond, what, msg) if not cond then fail(what, msg) end end

  local state
  local g, v

  -- One GameRunner tick, in the runner's own order: StateRules first
  -- (step 2d), then the state's update body (step 2e).  A flag an update
  -- raises is therefore acted on by the NEXT tick, exactly as on the
  -- device.
  local function tick(id)
    shine_result.status = id and "player" or "no_hit"
    shine_result.id     = id or 0
    for _, r in ipairs(g.rules) do
      if r.from == state and (not r.when or r.when(v)) then
        state = r.to
        if r.action then r.action(v) end
        break
      end
    end
    if g.update[state] then g.update[state](v) end
    clock = clock + 200
    shine_busy_until = 0            -- the burst finished inside the tick
  end

  local function fresh()
    libcache = {}                   -- a projector of its own for this copy
    g = dofile(ROOT .. "custom/tirobersaglio.lua")
    v = {}
    for _, c in ipairs(g.config) do v[c.id] = c.default end
    for _, x in ipairs(g.vars)   do v[x.id] = initial(x) end
    clock, shine_busy_until = 0, 0
    g.on_begin(v)
    state = g.initial_state
  end

  la.trigger_down = function() return true end

  -- ---- the welcome screen answers two colours, and no others --------
  fresh()
  local radio0 = #out.radio          -- out.radio is the whole file's, not ours
  local e0 = v.energy
  tick(CLEAR)
  check(state == S_PRE and v.energy == e0, "trial",
        "the CLEAR practice shot left the welcome screen or cost energy")
  tick(YELLOW)
  check(state == S_PRE and v.hits == 0, "trial",
        "a run target was live on the welcome screen")
  check(#out.radio == radio0, "trial",
        (#out.radio - radio0) .. " radio message(s) escaped a game that has no radio")

  -- ---- GREEN opens the turn, and IS target 1 ------------------------
  tick(GREEN)                       -- update raises the flag ...
  tick(nil)                         -- ... and this tick's rules act on it
  check(state == S_ACTIVE, "start", "the GREEN target did not start the turn")
  check(v.hits == 1, "start", "the opening GREEN did not count as target 1")
  check(v.next_n == 2, "start", "the turn did not ask for target 2")
  check(v.time_left == v.sub_time, "start", "the turn clock was not loaded")
  check(v.energy_left == v.pool * v.charges - 1, "start",
        "the reserve magazine is missing from the energy the score converts")

  -- ---- the sequence advances only on the expected colour ------------
  tick(YELLOW)
  check(v.hits == 2 and v.next_n == 3, "order", "a correct target did not count")
  local h = v.hits
  tick(MAGENTA)
  check(v.hits == h, "order", "an out-of-order target counted as a hit")

  -- ---- a CLEAR gives its beam back ----------------------------------
  local before = v.energy
  tick(CLEAR)                       -- refunds this tick's own beam
  check(v.energy == before, "clear",
        "the CLEAR panel did not refund its energy: " ..
        tostring(before) .. " -> " .. tostring(v.energy))
  check(v.energy_left == v.energy + v.reloads * v.pool, "clear",
        "the score's energy drifted from magazine + reserve")

  -- ---- the reload: an empty magazine, and a press edge ---------------
  la.trigger_state = function() return "pressed" end
  v.energy = 5
  local reloads = v.reloads
  tick(nil)
  check(v.reloads == reloads, "reload",
        "the second trigger reloaded a magazine that still had energy")
  v.energy = 0
  tick(nil)
  check(v.reloads == reloads - 1 and v.energy > 0, "reload",
        "the second trigger did not reload an empty magazine")
  la.trigger_state = function() return "off" end

  -- ---- the grace window: the last beam's CLEAR still arrives ---------
  v.energy, v.reloads = 1, 0
  tick(nil)                         -- spends the very last energy
  check(v.energy_left == 0, "grace", "the last energy was not spent")
  tick(nil)
  check(state == S_ACTIVE, "grace",
        "the turn ended before the last beam's result could come back")
  la.trigger_down = function() return false end   -- finger off: no new beam
  tick(CLEAR)                       -- the result of that last beam
  check(state == S_ACTIVE and v.energy_left == 1, "grace",
        "the CLEAR refund did not save the last beam")
  la.trigger_down = function() return true end

  -- ---- and once the grace is spent, the turn ends --------------------
  -- Two ticks, not one: `energy_left` is recomputed by the update body, so
  -- the rules only see an emptied pool from the tick after -- the same
  -- one-tick lag the device has, and the reason a rule may never read a
  -- number the update it precedes is about to write.
  v.energy, v.reloads = 0, 0
  tick(nil)
  clock = clock + 2000
  tick(nil)
  check(state == S_END, "energy", "an empty pool did not end the turn")
  check(v.score == v.pt_target * v.hits + v.energy_left + v.time_left, "energy",
        "the score did not pay the leftovers of a clean turn")

  -- ---- a RED keeps the targets and forfeits the leftovers ------------
  fresh()
  tick(GREEN); tick(nil)
  local hits_before = v.hits
  tick(RED)
  tick(nil)
  check(state == S_END, "red", "a RED target did not end the turn")
  check(v.red == 1, "red", "the RED target was not recorded")
  check(v.hits == hits_before, "red", "a RED target moved the hit count")
  check(v.score == v.pt_target * v.hits, "red",
        "a RED turn was still paid its time and energy: " .. tostring(v.score))

  -- ---- all six panels end the turn while the clock still pays --------
  fresh()
  tick(GREEN); tick(nil)
  for _, id in ipairs({ YELLOW, BLUE, ORANGE, LIME, MAGENTA }) do tick(id) end
  check(v.hits == 6, "sweep", "the six panels did not all count: " .. tostring(v.hits))
  tick(nil)
  check(state == S_END, "sweep", "a completed sequence did not end the turn")
  check(v.score == v.pt_target * 6 + v.energy_left + v.time_left, "sweep",
        "a perfect turn was not paid its leftovers")

  local scored = false
  for _, line in ipairs(out.shows) do
    if line:match("^Giocatore #%d+ PUNTI: %d+$") then scored = true end
  end
  check(scored, "tray", "the stats screen never showed the score line")

  -- ---- the staff's key hands over to the next child ------------------
  la.key_down = function(k) return k == "<" or k == ">" end
  local n = v.counter
  tick(nil)
  check(state == S_PRE and v.counter == n + 1, "handover",
        "< + > did not start the next child's turn")
  check(v.hits == 0 and v.red == 0 and v.energy_left == v.pool * v.charges,
        "handover", "the next child inherited the last one's turn")
  la.key_down = function(k, pad) return false end

  la.trigger_down = function() return false end
  shine_result.status, shine_result.id = "no_hit", 0
  print("OK   tirobersaglio order enforced, RED forfeits the leftovers, "
        .. "CLEAR refunds even the last beam, reload needs an empty magazine")
end

-- ================================================================
--   CP conquest/hold replies (festasportsasso, kingofhill, upkeep)
--
--   The totem's own accbit rule has no RSSI guard at all -- proximity
--   is entirely the REPLYING PLAYER's decision (see std.totems.cp()
--   and docs/totem-behavior-handshake.md, "RSSI is readable, not
--   policy").  Each beacon is answered fresh from the CURRENT RSSI
--   alone, with no memory of past presence: a non-owner replies
--   "conquest" only within the tight ring; the recorded owner replies
--   "hold" anywhere within the (looser) hold ring, at any distance
--   inside it -- easy stealing is deliberate, so proximity never
--   upgrades an owner's hold into a conquest.
-- ================================================================
do
  local CP_HOLD = 17
  local cases = {
    { file = "custom/festasportsasso", tight = -45, hold = -66, owner_arg = 1 },
    { file = "kingofhill",      tight = -45, hold = -66, owner_arg = 1 },
    { file = "upkeep",          tight = -45, hold = -66, owner_arg = 0 },
  }

  for _, case in ipairs(cases) do
    local function fail(what, msg)
      failures = failures + 1
      print(string.format("  FAIL %-12s %s: %s", "cp/" .. case.file, what, msg))
    end
    local function check(cond, what, msg) if not cond then fail(what, msg) end end

    libcache = {}                       -- this game's own projector instance
    local g = dofile(ROOT .. case.file .. ".lua")
    local v = {}
    for _, c in ipairs(g.config) do v[c.id] = c.default end
    for _, x in ipairs(g.vars)   do v[x.id] = initial(x) end
    clock = 0
    g.on_begin(v)

    local msg = la.msg.CP_BEACON
    local function pkt(owner, rssi)
      return mk_pkt{ msg = msg, payload = { owner }, sender = 254, rssi = rssi }
    end

    -- Find the two CP_BEACON states by behaviour, not by hardcoding the
    -- ruleset's state enum: a strong signal on an unowned beacon answers
    -- with a conquest reply in the "in play" state and stays silent in
    -- the "down/eliminated" one.
    local presence_h, down_h
    for _, handlers in pairs(g.on_message) do
      local h = handlers[msg]
      if h then
        if h(v, pkt(0xFF, -20)) then presence_h = h else down_h = h end
      end
    end
    check(presence_h ~= nil, "setup", "no state ever answers CP presence")
    check(down_h     ~= nil, "setup", "no track-only (down) state found")

    -- Not the owner (beacon says unassigned): conquest only within the
    -- tight ring, nothing at all just inside the looser one.
    check(presence_h(v, pkt(0xFF, case.tight)) == case.owner_arg + 1,
          "conquest", "the tight gate itself did not answer conquest")
    check(presence_h(v, pkt(0xFF, case.hold + 2)) == nil,
          "conquest", "loose range answered right after a tight-range contact "
                    .. "-- there must be no hysteresis left")

    -- I already own it: hold anywhere in the loose ring, standing right
    -- at the totem included -- proximity never upgrades it to conquest.
    check(presence_h(v, pkt(case.owner_arg, case.tight)) == CP_HOLD,
          "hold", "owner standing at the totem did not reply hold")
    check(presence_h(v, pkt(case.owner_arg, case.hold + 2)) == CP_HOLD,
          "hold", "owner at the edge of the hold ring did not reply hold")
    check(presence_h(v, pkt(case.owner_arg, case.hold - 5)) == nil,
          "hold", "owner outside the hold ring still replied")

    -- Shone: no reply at all, regardless of range or ownership.
    check(down_h(v, pkt(case.owner_arg, case.tight)) == nil,
          "shone", "the down state answered a beacon")
  end

  print("OK   cp conquest/hold  tight-ring conquest, owner-only hold, no RSSI memory")
end

-- ================================================================
--   BONUS / MALUS options and effects
--
--   The DM picks each pickup totem's effect with O; la.totem_option()
--   carries it to the claiming player.  Checks, per game: the option
--   lists the menu shows, and that a claim applies what was picked —
--   LIFE as +S capped at 2*S (S = this player's starting lives, through
--   the game's own resolver), a projector put in hand, MALUS LIFE ending
--   the lives, DIM weakening the projector until the player goes out.
-- ================================================================
do
  local function fail(what, msg)
    failures = failures + 1
    print(string.format("  FAIL %-12s %s", what, msg))
  end
  local function check(cond, what, msg) if not cond then fail(what, msg) end end

  local TOTEM = 250
  local function options_of(g, role)
    for _, t in ipairs(g.totem_slots) do
      if t.role == role then return t.options end
    end
  end
  local function has(list, x)
    for _, v in ipairs(list or {}) do if v == x then return true end end
    return false
  end
  -- The handler that answers a ready pickup beacon in the "in play" state.
  local function claim_handler(g, msg)
    local first = g.on_message[g.initial_state]
    if first and first[msg] then return first[msg] end
    for _, handlers in pairs(g.on_message) do
      if handlers[msg] then return handlers[msg] end
    end
  end
  local function fresh_game(f)
    libcache = {}                       -- this game's own projector instance
    local g = dofile(ROOT .. f .. ".lua")
    local v = {}
    for _, c in ipairs(g.config) do v[c.id] = c.default end
    for _, x in ipairs(g.vars)   do v[x.id] = initial(x) end
    clock = 0
    g.on_begin(v)
    return g, v, libcache.projector
  end
  local function claim(g, v, msg, label)
    totem_opts[TOTEM] = label
    local h = claim_handler(g, msg)
    local r = h(v, mk_pkt{ msg = msg, payload = { 0 }, sender = TOTEM, rssi = -30 })
    totem_opts[TOTEM] = nil
    return r
  end
  local B, M = la.msg.BONUS_BEACON, la.msg.MALUS_BEACON

  -- Option lists: every label fits the 8-char menu, the standard catalogue
  -- is offered in every game with pickups (Outflow's included: its powered
  -- projectors share the life pool), role/practice projectors never are.
  local lists = { "freeforall", "teams", "flag", "kingofhill", "upkeep", "virus",
                  "custom/festasportsasso", "outflow" }
  for _, f in ipairs(lists) do
    local projectors = true
    local g = fresh_game(f)
    local bo, mo = options_of(g, "BONUS"), options_of(g, "MALUS")
    check(bo and bo[1] == "LIFE", f, "BONUS options do not start with LIFE")
    check(has(mo, "LIFE") and has(mo, "DIM") and #mo == 2, f, "MALUS options are not LIFE, DIM")
    for _, name in ipairs({ "SPLASH", "FAST", "LONG", "STRONG" }) do
      check(has(bo, name) == projectors, f,
            "BONUS " .. name .. (projectors and " missing" or " offered where the pool is life"))
    end
    check(not has(bo, "TRIAL") and not has(bo, "VIRUS"), f,
          "a practice / role projector is offered as a BONUS")

    -- Each powered projector's icon must reach the ENERGY cell: the cell
    -- names an icon_var, and holding the projector writes its icon there.
    if projectors then
      local iv
      for _, m in ipairs(g.monitor) do
        if m.var == "energy" and m.icon_var then iv = m.icon_var end
      end
      check(iv ~= nil, f, "the energy cell has no icon_var: projector icons never show")
      for _, name in ipairs({ "SPLASH", "FAST", "LONG", "STRONG" }) do
        local g2, v2, P2 = fresh_game(f)
        claim(g2, v2, B, name)
        local want = la.icons[P2.active_profile().icon]
        check(iv and want and v2[iv] == want, f,
              "holding " .. name .. " left the energy cell's icon at " .. tostring(iv and v2[iv]))
      end
    end
    for _, l in ipairs(bo or {}) do check(#l <= 8, f, "option '" .. l .. "' over 8 chars") end
  end

  -- Lives games: LIFE, projector, MALUS LIFE, DIM — and the resolver.
  for _, f in ipairs({ "freeforall", "teams", "flag", "kingofhill", "upkeep" }) do
    local g, v, P = fresh_game(f)
    local S = v.start_lives

    -- Unhurt: +S lands exactly on the 2*S maximum.
    check(claim(g, v, B, "LIFE") == la.my_id(), f, "BONUS claim did not answer the totem")
    check(v.lives == 2 * S, f, "unhurt LIFE bonus gave " .. v.lives .. ", expected " .. 2 * S)
    -- A second one is capped.
    claim(g, v, B, "LIFE")
    check(v.lives == 2 * S, f, "LIFE bonus went past the 2*S cap: " .. v.lives)
    -- Hurt: a significant bonus, still +S.
    v.lives = 1
    claim(g, v, B, "LIFE")
    check(v.lives == math.min(1 + S, 2 * S), f, "hurt LIFE bonus gave " .. v.lives)

    -- A totem with no option does nothing but still answers.
    local before = v.lives
    check(claim(g, v, B, nil) == la.my_id() and v.lives == before, f,
          "an option-less BONUS changed something")

    -- Projector bonus: in hand at once.
    claim(g, v, B, "FAST")
    check(P.active_id() == 2, f, "FAST bonus did not put FAST in hand")

    -- DIM: half pool, lifted by going out.
    local pool = P.max_energy(v)
    claim(g, v, M, "DIM")
    check(P.dimmed() and P.max_energy(v) == math.max(1, pool // 2), f,
          "DIM did not halve the pool (" .. pool .. " -> " .. P.max_energy(v) .. ")")
    check(v.energy <= P.max_energy(v), f, "DIM left energy above the dimmed pool")

    -- MALUS LIFE: no lives left, and the OUT rule that fires lifts DIM.
    claim(g, v, M, "LIFE")
    check(v.lives == 0, f, "MALUS LIFE left " .. v.lives .. " lives")
    local out_rule
    for _, r in ipairs(g.rules) do
      if r.from == g.initial_state and r.when(v) and r.to ~= g.scoring_state then
        out_rule = r; break
      end
    end
    check(out_rule ~= nil, f, "no rule takes a player with 0 lives out")
    if out_rule then
      out.shows = {}
      out_rule.action(v)
      check(not P.dimmed(), f, "going out did not lift DIM")
      check(P.active_id() == 0 and P.owned_count() == 1, f,
            "going out kept a powered projector (in hand: " .. P.active_id() ..
            ", held: " .. P.owned_count() .. ")")
      local credited = false
      for _, t in ipairs(out.shows) do if t == "LIT by TOTEM" then credited = true end end
      check(credited, f, "a MALUS LIFE out is not credited to TOTEM")
      for _, r in ipairs(g.rules) do
        if r.to == g.initial_state and r.from ~= g.initial_state then
          r.action(v)
          check(P.active_id() == 0 and v.energy == P.max_energy(v), f,
                "respawn wrote " .. v.energy .. " into a pool of " .. P.max_energy(v))
          break
        end
      end
    end
  end

  -- The per-player resolver is the single source for respawn AND bonus:
  -- swap vars.start_lives for a "role" value and both follow.
  do
    local g, v = fresh_game("teams")
    v.start_lives = 4                   -- what a role resolver would return
    v.lives = 0
    for _, r in ipairs(g.rules) do
      if r.to == g.initial_state and r.from ~= g.initial_state then
        r.action(v); break
      end
    end
    check(v.lives == 4, "teams", "respawn did not restore this player's starting lives")
    v.lives = 3
    claim(g, v, B, "LIFE")
    check(v.lives == 7, "teams", "LIFE bonus did not use this player's starting lives")
  end

  -- No game writes starting lives directly: every use goes through the
  -- game's resolver, so a per-role value is a one-line change.
  for _, f in ipairs(files) do
    local fh = assert(io.open(ROOT .. f .. ".lua", "r"))
    local src = fh:read("a"); fh:close()
    for line in src:gmatch("[^\n]+") do
      if not line:match("^%s*%-%-") and line:match("lives%s*=%s*vars%.start_lives") then
        fail(f, "sets lives from vars.start_lives, not the resolver: "
                .. line:match("^%s*(.-)%s*$"))
      end
    end
  end

  -- Outflow: energy is life.  LIFE is +S capped at 2*S; MALUS LIFE drains.
  do
    local g, v = fresh_game("outflow")
    local S = v.start_energy
    claim(g, v, B, "LIFE")
    check(v.energy == 2 * S, "outflow", "LIFE bonus on energy gave " .. v.energy)
    claim(g, v, M, "LIFE")
    check(v.energy == 0, "outflow", "MALUS LIFE left " .. v.energy .. " energy")
  end

  -- Outflow: a powered projector shares the life pool (shared_pool).
  -- Picking one up heals nothing, its beams cost the life pool, its own
  -- recharge never runs on it, and going out drops it.
  do
    local g, v, P = fresh_game("outflow")
    local play = g.update[g.initial_state]
    shine_busy_until = 0
    v.energy = 60
    claim(g, v, B, "STRONG")
    check(P.active_id() == 4 and v.energy == 60, "outflow",
          "BONUS STRONG: in hand " .. P.active_id() .. ", energy " .. v.energy ..
          " (expected 4, 60: a pickup must not heal)")
    check(P.payload(v) == 3, "outflow", "STRONG does not weigh 3 here")
    clock = 500                                     -- past STRONG's ready, before any drain
    la.trigger_down = function() return true end
    play(v)
    la.trigger_down = function() return false end
    clock = 510; play(v)                            -- the release starts any recharge wait
    check(v.energy == 59, "outflow",
          "a STRONG beam cost " .. (60 - v.energy) .. " of the life pool, expected 1")
    claim(g, v, B, "FAST")
    check(P.active_id() == 2 and v.energy == 59, "outflow", "BONUS FAST healed or missed")
    clock = 30510; play(v)                          -- FAST alone would have ramped by now
    check(v.energy == 58, "outflow",
          "energy " .. v.energy .. " after 30 s, expected 58 (one drain step, no recharge)")
    for _, r in ipairs(g.rules) do
      if r.from == g.initial_state and r.to ~= g.scoring_state and r.action then
        r.action(v); break                          -- an out transition
      end
    end
    check(P.active_id() == 0 and P.owned_count() == 1 and v.energy == 58, "outflow",
          "going out kept a projector or moved the pool")
  end

  -- Virus: a virus keeps the VIRUS projector — a projector bonus becomes LIFE.
  do
    local g, v, P = fresh_game("virus")
    claim(g, v, B, "FAST")
    check(P.active_id() == 2, "virus", "a clean player did not get FAST")
    -- Become the virus through the game's own rule.
    local infect = claim_handler(g, la.msg.LIT)
    infect(v, mk_pkt{ msg = la.msg.LIT, payload = { 1, 11, 1 }, sender = 5 })
    for _, r in ipairs(g.rules) do
      if r.from == g.initial_state and r.to ~= g.scoring_state and r.when(v) then
        r.action(v); break
      end
    end
    check(P.active_id() == 11, "virus", "infection did not put VIRUS in hand")
    v.energy = 0
    claim(g, v, B, "FAST")
    check(P.active_id() == 11, "virus", "a projector bonus took VIRUS out of a virus's hand")
    check(v.energy == v.energy_max, "virus",
          "the virus's projector bonus did not become LIFE (energy " .. v.energy .. ")")
  end

  -- Virus: patient zero is the player the DM drew at Start.  This device
  -- (player 2) starts infected exactly when the draw named it, and every
  -- device counts the drawn player as infected either way.
  do
    for _, case in ipairs({ { 2, true }, { 1, false }, { 7, false } }) do
      drawn_player = case[1]
      local g, v = fresh_game("virus")
      local fired = false
      for _, r in ipairs(g.rules) do
        if r.from == g.initial_state and r.to ~= g.scoring_state and r.when(v) then
          fired = true
        end
      end
      check(fired == case[2], "virus",
            "drawn player " .. case[1] .. ": this player (2) " ..
            (fired and "became" or "did not become") .. " the virus")
      check(v.clean_left == la.player_count() - 1, "virus",
            "the drawn player was not counted out of the clean ones")
    end
    drawn_player = 1
    local g = dofile(ROOT .. "virus.lua")
    for _, c in ipairs(g.config) do
      check(c.id ~= "virus_id", "virus", "the first virus is in the config menu")
    end
  end

  -- DIM on the projector itself: recharge and cooldown doubled, a
  -- profile with no cooldown gets the floor, and lifting it restores both.
  do
    package.loaded_projector = nil
    local P = dofile(ROOT .. "lib/projector.lua")
    P.define{ vars = { energy = "energy", spent = "energy_spent",
                       reload = "reload", reload_ms = "reload_ms" },
              profiles = { { id = 20, name = "NOCOOL", cooldown_ms = 0, max_energy = 5 } } }
    local pushed
    local real = la.shine_config
    la.shine_config = function(t) if t.cooldown_ms then pushed = t.cooldown_ms end end
    local v = { energy = 20, energy_spent = 0, start_energy = 20, recharge_secs = 4,
                reload = 0, reload_ms = 0 }
    clock = 0
    P.reset(v)
    check(pushed == 50, "dim", "baseline did not push its 50 ms cooldown: " .. tostring(pushed))
    P.set_dim(v, true)
    check(pushed == 100, "dim", "dimmed baseline cooldown " .. tostring(pushed) .. ", expected 100")
    check(v.reload_ms == 8000, "dim", "dimmed recharge is " .. v.reload_ms .. " ms, expected 8000")
    check(v.energy == 10, "dim", "dimmed pool left energy " .. v.energy)
    P.set_dim(v, false)
    check(pushed == 50 and v.reload_ms == 4000, "dim", "lifting DIM did not restore the optics / recharge")
    P.grant(v, 4)                       -- STRONG: twice the baseline's 50 ms
    P.set_dim(v, true)
    check(pushed == 200, "dim", "STRONG dimmed cooldown " .. tostring(pushed) .. ", expected 200")
    P.grant(v, 20)                      -- no cooldown at all: the floor
    check(pushed == 250, "dim", "a dimmed 0 cooldown got " .. tostring(pushed) .. ", expected the 250 floor")
    P.reset(v)
    check(not P.dimmed(), "dim", "reset did not lift DIM")
    la.shine_config = real
  end

  -- Feedback: the LCD names the effect — every projector by its own name —
  -- and each kind has its own cue, with no ProjectorChange on top.
  do
    local function last(t) return t[#t] end
    local function saw(t, x, from)
      for i = from, #t do if t[i] == x then return true end end
      return false
    end
    local g, v, P = fresh_game("teams")
    local cases = {
      { B, "LIFE",   "BONUS LIFE",   "Bonus" },
      { B, "SPLASH", "BONUS SPLASH", "BonusProjector" },
      { B, "FAST",   "BONUS FAST",   "BonusProjector" },
      { B, "LONG",   "BONUS LONG",   "BonusProjector" },
      { B, "STRONG", "BONUS STRONG", "BonusProjector" },
      { M, "DIM",    "MALUS DIM",    "MalusDim" },
      { M, "LIFE",   "MALUS LIFE",   "Malus" },
    }
    for _, c in ipairs(cases) do
      local ui0 = #out.ui + 1
      claim(g, v, c[1], c[2])
      check(last(out.shows) == c[3], "feedback",
            c[2] .. ": LCD read '" .. tostring(last(out.shows)) .. "', expected '" .. c[3] .. "'")
      check(saw(out.ui, c[4], ui0), "feedback", c[2] .. ": no '" .. c[4] .. "' cue")
      check(not saw(out.ui, "ProjectorChange", ui0), "feedback",
            c[2] .. ": ProjectorChange played on top of the pickup cue")
    end

    -- A game's own projector shows its own name, cut to the menu's 8 chars.
    package.loaded_projector = nil
    local CP = dofile(ROOT .. "lib/projector.lua")
    CP.define{ vars = { energy = "energy", spent = "energy_spent" },
               profiles = { { id = 10, name = "LIGHTNING", max_energy = 5 } } }
    local opts = CP.bonus_options()
    check(opts[#opts] == "LIGHTNIN", "feedback", "custom option label is '" .. tostring(opts[#opts]) .. "'")
    local S = dofile(ROOT .. "lib/std.lua")
    local eff = S.pickup_effect{ proj = CP, lives = "lives" }
    local cv = { energy = 5, energy_spent = 0, start_energy = 5, recharge_secs = 1,
                 lives = 3, start_lives = 3 }
    CP.reset(cv)
    totem_opts[TOTEM] = "LIGHTNIN"
    local ui0 = #out.ui + 1
    eff(cv, mk_pkt{ msg = B, payload = { 0 }, sender = TOTEM })
    totem_opts[TOTEM] = nil
    check(last(out.shows) == "BONUS LIGHTNIN", "feedback",
          "custom projector LCD read '" .. tostring(last(out.shows)) .. "'")
    check(CP.active_id() == 10 and saw(out.ui, "BonusProjector", ui0), "feedback",
          "custom projector bonus not granted with its cue")
  end

  print("OK   pickups       option lists, LIFE +S capped 2*S, projector, MALUS LIFE, DIM")
end

-- ================================================================
--   Area hits: the ruleset's own rule, minus immunity
-- ================================================================
-- The firmware hands an area hit to the state's LIT handler with pkt.area
-- set.  Immunity is a rule about beams: an area hit neither checks the
-- window nor opens one, while friendly fire — judged against the
-- originator, who is the hit's sender — still applies.
do
  local function fail(what, msg)
    failures = failures + 1
    print(string.format("  FAIL %-12s %s: %s", "area hits", what, msg))
  end
  local function check(cond, what, msg) if not cond then fail(what, msg) end end

  local S   = dofile(ROOT .. "lib/std.lua")
  local imm = S.immunity(3000)
  local REPLY = { taken = 1, shone = 2, friend = 4, immune = 5 }
  local take = S.lit_target{
    lives = "lives", immunity = imm, reply = REPLY,
    friendly = function(pkt) return pkt.team == la.my_team() end,
  }
  local function hit(sender, strength, area)
    return mk_pkt{ sender = sender, team = la.team_of(sender), area = area,
                   payload = { strength, 1, 0, 0, area and 1 or nil } }
  end
  local v = { lives = 20 }
  clock = 0
  imm.reset()

  check(take(v, hit(3, 1)) == REPLY.taken and v.lives == 19, "direct",
        "a direct hit did not land")
  check(take(v, hit(3, 1)) == REPLY.immune and v.lives == 19, "direct",
        "a second direct hit inside the window was not refused")
  check(take(v, hit(3, 2, true)) == REPLY.taken and v.lives == 17, "window",
        "an area hit was refused by the shooter's immunity window")
  check(take(v, hit(5, 1, true)) == REPLY.taken and v.lives == 16, "window",
        "an area hit from a fresh shooter did not land")
  check(take(v, hit(5, 1)) == REPLY.taken and v.lives == 15, "window",
        "an area hit opened an immunity window: the shooter's beam right after was refused")
  check(take(v, hit(4, 2, true)) == REPLY.friend and v.lives == 15, "friendly",
        "friendly fire let a teammate's area hit through")
  check(take(v, hit(la.my_id(), 1, true)) == REPLY.taken and v.lives == 14, "self",
        "an own area hit (its policy said self = true) was refused as friendly fire")
  check(take(v, hit(la.my_id(), 1)) == REPLY.friend and v.lives == 14, "self",
        "the self exemption leaked to a direct hit")
  v.lives = 2
  check(take(v, hit(7, 2, true)) == REPLY.shone and v.lives == 0, "knock-out",
        "an area hit that empties the lives is not a SHONE")

  print("OK   area hits     immunity is the beam's: area hits neither check nor open it; friendly fire and knock-outs still apply; an own area hit is its policy's call")
end

-- ================================================================
--   Every game weighs a hit by its strength
-- ================================================================
-- Strength and area hits are shared mechanics: a game that takes hits
-- must not re-implement them, or it drifts (freeforall once took every
-- hit as one life).  Per game, in its play state: a strength-3 LIT
-- costs 3 units (lives, or Outflow's lit_cost energy), the same shooter
-- inside the immunity window is refused where the game has one, and an
-- area hit costs its band's strength and ignores that window.
do
  local function fail(what, msg)
    failures = failures + 1
    print(string.format("  FAIL %-12s %s: %s", "strength", what, msg))
  end
  local function check(cond, what, msg) if not cond then fail(what, msg) end end
  local function fresh(f)
    libcache = {}
    local g = dofile(ROOT .. f .. ".lua")
    local v = {}
    for _, c in ipairs(g.config) do v[c.id] = c.default end
    for _, x in ipairs(g.vars)   do v[x.id] = initial(x) end
    clock = 0
    g.on_begin(v)
    return g, v
  end
  local function hit(strength, area)
    return mk_pkt{ sender = 3, team = la.team_of(3), area = area,
                   payload = { strength, 0, 0, 0, area and 1 or nil } }
  end
  -- { file, play state, var, unit, immune reply or nil }
  local games = {
    { "freeforall",             0, "lives", 1, 4 },
    { "teams",                  0, "lives", 1, 5 },
    { "flag",                   0, "lives", 1 },
    { "kingofhill",             0, "lives", 1 },
    { "upkeep",                 0, "lives", 1 },
    { "custom/festasportsasso", 1, "lives", 1 },
    { "outflow",                0, "energy" },
  }
  for _, c in ipairs(games) do
    local f, st, var = c[1], c[2], c[3]
    local g, v = fresh(f)
    local unit = c[4] or v.lit_cost
    local h = g.on_message[st] and g.on_message[st][la.msg.LIT]
    check(h ~= nil, f, "no LIT handler in the play state")
    if h then
      v[var] = 20 * unit
      clock = 1000
      local r = h(v, hit(3))
      check(r == la.hit.TAKEN and v[var] == 17 * unit, f,
            "a strength-3 hit cost " .. (20 * unit - v[var]) // unit .. " units, reply " .. tostring(r))
      if c[5] then
        clock = 1100
        r = h(v, hit(1))
        check(r == c[5] and v[var] == 17 * unit, f, "the same shooter inside the window was not refused")
      end
      clock = 1200
      r = h(v, hit(2, true))
      check(r == la.hit.TAKEN and v[var] == 15 * unit, f,
            "an area hit of 2 cost " .. (17 * unit - v[var]) // unit .. " units, reply " .. tostring(r))
    end
  end
  print("OK   strength      every game that takes hits weighs them by strength, area hits by band, past immunity")
end

print("\nTotemVM encoded program sizes (bytes, single-packet budget = 225):")
local keys = {}
for k in pairs(totem_sizes) do keys[#keys+1] = k end
table.sort(keys)
for _, k in ipairs(keys) do print(string.format("  %-24s %3d", k, totem_sizes[k])) end

print(failures == 0 and "\nALL GAMES PASS" or ("\n" .. failures .. " FAILURES"))
os.exit(failures == 0 and 0 or 1)
