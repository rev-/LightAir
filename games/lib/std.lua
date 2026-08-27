-- ================================================================
-- LightAir standard game library — pure Lua, no firmware code.
--
-- Load with:  local std = la.lib("std")
--
-- This is layer 2 of the API (see docs/lua-games-design.md §"API
-- layering"): recurring game patterns built ONLY out of kernel
-- verbs.  It ships as a file next to the games, so it can grow or
-- be fixed without reflashing firmware, and a game that doesn't
-- like a helper simply doesn't call it — nothing here is required.
--
-- Everything allocates at load time (closures, config tables) and
-- runs allocation-free in the 10 ms tick path.
-- ================================================================

local std = { totems = {} }

-- ----------------------------------------------------------------
-- Per-sender immunity window.
--   local imm = std.immunity(3000)
--   imm.active(id) -> bool     imm.mark(id)     imm.reset()
-- ----------------------------------------------------------------
function std.immunity(window_ms)
  local marks = {}
  return {
    active = function(id)
      local t = marks[id]
      return t ~= nil and la.now() - t < window_ms
    end,
    mark  = function(id) marks[id] = la.now() end,
    reset = function() for k in pairs(marks) do marks[k] = nil end end,
  }
end

-- ----------------------------------------------------------------
-- The trigger/energy/recharge shine idiom shared by every ruleset:
-- shine while the trigger is down and energy remains; on release
-- start a cooldown; after cfg.recharge seconds restore full energy.
--
--   local shiner = std.shiner{ energy = "energy", spent = "energy_spent",
--                              max = "start_energy", recharge = "recharge_secs",
--                              can = function() return true end }  -- optional gate
--   shiner.reset()            -- call from on_begin
--   shiner.tick(vars)         -- call from update; returns trigger state
--
-- All cfg entries are *var ids* resolved through the vars proxy, so
-- config-menu changes apply without re-wiring.
-- ----------------------------------------------------------------
function std.shiner(cfg)
  local was_active, release_at = false, 0
  return {
    reset = function()
      was_active, release_at = false, 0
    end,
    tick = function(vars)
      local max    = vars[cfg.max]
      local active = la.trigger_down(1)
      if active and vars[cfg.energy] > 0
         and (cfg.can == nil or cfg.can())
         and la.shine() then
        vars[cfg.energy] = vars[cfg.energy] - 1
        vars[cfg.spent]  = vars[cfg.spent] + 1
        la.ui_enlight(la.shine_ms())
      end
      if was_active and not active then release_at = la.now() end
      was_active = active
      if not active and vars[cfg.energy] < max
         and la.now() - release_at >= vars[cfg.recharge] * 1000 then
        vars[cfg.energy] = max
      end
      return active
    end,
  }
end

-- ----------------------------------------------------------------
-- Standard target-side MSG.LIT handler: the friendly-fire /
-- immunity / lives ladder used by FFA, Teams, Flag, KoH and Upkeep.
--
--   on_message = { [S.IN_GAME] = { [MSG.LIT] = std.lit_target{
--       lives = "lives", immunity = imm,
--       reply = { taken = R.TAKEN, shone = R.SHONE,
--                 friend = R.FRIEND, immune = R.IMMUNE },
--       friendly = function(pkt) ... end,   -- optional; true = reject
--       on_shone = function(vars, pkt) ... end,  -- optional; last life lost
--   } } }
--
-- on_shone runs on the hit that empties the lives counter, while the
-- shooter's packet is still in hand — the only moment a game can learn
-- who put it down (the state rule that follows sees no packet).
--
-- The returned reply sub-type drives the sender-side on_reply table.
-- ----------------------------------------------------------------
function std.lit_target(cfg)
  return function(vars, pkt)
    if cfg.friendly and cfg.friendly(pkt) then return cfg.reply.friend end
    if cfg.immunity.active(pkt.sender)   then return cfg.reply.immune end
    vars[cfg.lives] = vars[cfg.lives] - 1
    cfg.immunity.mark(pkt.sender)
    if vars[cfg.lives] > 0 then
      la.ui("GotLit")
      return cfg.reply.taken
    end
    if cfg.on_shone then cfg.on_shone(vars, pkt) end
    return cfg.reply.shone            -- a state rule handles the transition
  end
end

-- ----------------------------------------------------------------
-- BASE-beacon respawn handler for OUT_GAME (Teams/Flag/Upkeep/KoH).
-- Gates on a minimum-wait predicate, team match and RSSI proximity,
-- then calls cfg.on_ready and replies so the BASE animates.
--
--   [MSG.BASE_BEACON] = std.base_respawn{
--       when     = function(vars) return la.now() >= respawn_at end,
--       team     = function() return my_team end,  -- accepted team; nil = teamless only
--       teamless = true,                           -- also accept 0xFF bases
--       rssi     = -57,                            -- REQUIRED, see below
--       on_ready = function(vars) can_respawn = true end,
--   }
--
-- cfg.rssi has no default on purpose.  How close "at the base" means is a
-- ruleset decision — the same BASE role is a 2 m respawn pad in Teams and
-- also the capture point in Flag — so the library refuses to pick a number
-- on a game's behalf and errors if one is missing.
-- ----------------------------------------------------------------
function std.base_respawn(cfg)
  assert(cfg.rssi, "std.base_respawn: cfg.rssi is required")
  return function(vars, pkt)
    if cfg.when and not cfg.when(vars) then return end
    if pkt.len < 1 then return end
    local base_team = pkt:byte(1)
    local mine      = cfg.team and cfg.team() or nil
    local ok        = (mine ~= nil and base_team == mine)
                   or (cfg.teamless and base_team == 0xFF)
    if not ok then return end
    if pkt.rssi < cfg.rssi then return end
    cfg.on_ready(vars)
    -- Answering IS the signal: the sub-type (slot+1, always >= 1) tells the
    -- BASE to play its respawn animation.  A handler that returns nothing
    -- sends nothing, so a base hears only the players actually respawning.
    return (mine or (la.my_id() - 1)) + 1
  end
end

-- ----------------------------------------------------------------
-- BONUS / MALUS claim handler.
--
--   [MSG.BONUS_BEACON] = std.pickup_claim{ rssi = -57 },
--
-- The pickup totem hands itself to whoever answers its beacon, so the
-- answer has to mean "I am standing at it": this gates on proximity and
-- returns the claiming player's id, which the totem's `reply` rule turns
-- into the claim animation and its cooldown.  cfg.on_claim(vars, pkt) is
-- optional and is where a ruleset attaches the actual reward.
--
-- cfg.rssi is required, for the same reason as base_respawn.
-- ----------------------------------------------------------------
function std.pickup_claim(cfg)
  assert(cfg.rssi, "std.pickup_claim: cfg.rssi is required")
  return function(vars, pkt)
    if pkt.len < 1 or pkt:byte(1) ~= 0 then return end   -- 0 = totem is ready
    if pkt.rssi < cfg.rssi then return end
    if cfg.on_claim then cfg.on_claim(vars, pkt) end
    return la.my_id()
  end
end

-- ----------------------------------------------------------------
-- Two-team aggregate winner announcement (Teams, Flag, Upkeep).
-- scores is the array handed to on_score_announce:
--   { { id = pid, team = 0|1|.., vals = { v1, v2 } }, ... }
-- vals[1] = primary (higher wins), vals[2] = tie-break (lower wins).
-- cfg.primary: "sum" (add per team) or "max" (take best per team —
-- Upkeep style, tolerant of missed CP-score packets).
-- ----------------------------------------------------------------
function std.team_announce(cfg)
  return function(scores)
    local pts = { [0] = 0, [1] = 0 }
    local tie = { [0] = 0, [1] = 0 }
    for _, s in ipairs(scores) do
      local t = (s.team == 1) and 1 or 0
      if cfg.primary == "max" then
        if s.vals[1] > pts[t] then pts[t] = s.vals[1] end
      else
        pts[t] = pts[t] + s.vals[1]
      end
      tie[t] = tie[t] + s.vals[2]
    end
    local w = -1
    if     pts[1] > pts[0] then w = 1
    elseif pts[0] > pts[1] then w = 0
    elseif tie[1] < tie[0] then w = 1
    elseif tie[0] < tie[1] then w = 0 end

    -- Personalised line first (ends up on the bottom tray row).
    if w < 0 then                 la.show("Your team tied!", 0)
    elseif w == la.my_team() then la.show("Your team won!", 0)
    else                          la.show("Your team lost!", 0) end
    if w < 0 then la.show("TIE!", 0)
    else          la.show("TEAM " .. la.team_short(w) .. " WINS!", 0) end
  end
end

-- ================================================================
-- Standard totem roles — TotemVM v1 programs (pure data).
--
-- Totems hold no game files.  Each factory below returns a
-- declarative state-machine table that the projector validates and
-- serializes into the 0xF1 activation reply — the whole behaviour
-- travels in that single packet.  No code from this file ever runs
-- on a totem; the interpreter lives in the totem firmware.
-- Full model and wire format: docs/totem-behavior-handshake.md.
--
-- Program shape:
--   { vm = 1, cfg_default = secs?, states = { {rule, ...}, ... } }
--   state 1 is the initial state.
--   rule = { enter = true | every = ms | msg = type | reply = type,
--            when = { guard, ... },     -- optional, all must hold
--            run  = { action, ... },
--            cont = true }              -- optional: don't consume event
--
-- Value specs usable as operands:
--   number          literal byte / u16
--   {"r", n}        register R0..R7
--   {"p", i}        payload byte (1-based, as pkt:byte(i))
--   {"low"}         lowest set bit index of ACC
--   {"sender"}      sender player id
--   {"team"}        sender team
--   {"cfg"}         this role's config seconds (resolved by the
--                   projector at serialization; cfg_default if the
--                   game declares no config_var for the role)
-- ================================================================

-- ---- BASE: respawn base.  team = 0, 1 or "any" (teamless). --------
-- Beacons its team byte every second; an *intentional* respawn reply
-- (sub-type >= 1 — empty auto-replies carry no proximity info) plays
-- the respawn animation in the respawning player's colour.
function std.totems.base(team)
  local tv = (team == "any") and 0xFF or team
  return { vm = 1, states = { {
    { enter = true,
      run = { {"anim", "BaseIdle", {"team", tv}, {"rhythm", tv}} } },
    { every = 1000,
      run = { {"bcast", la.msg.BASE_BEACON, tv} } },
    { reply = la.msg.BASE_BEACON,
      when = { {"len", ">=", 1}, {"p", 1, ">=", 1} },
      -- Always the *respawning player's* colour, on a team base too: the
      -- base's own team is already on its idle ring, so what the animation
      -- has to say is who just came back.
      run = { {"anim", "Respawn", {"sender_player"}} } },
  } } }
end

-- ---- BONUS / MALUS: claimable pickup with cooldown. ---------------
-- State 1 (READY): beacon every 2 s; any reply claims the pickup.
-- State 2 (COOLDOWN): silent until the configured seconds elapse.
local function pickup(cfg)
  return { vm = 1, cfg_default = 30, states = {
    { -- state 1: READY
      { enter = true,
        run = { {"anim", cfg.idle, {"rgb", cfg.rgb[1], cfg.rgb[2], cfg.rgb[3]}} } },
      { every = 2000,
        run = { {"bcast", cfg.beacon, 0} } },       -- payload byte 0 = ready
      { reply = cfg.beacon,
        run = { {"start", 0}, {"anim", cfg.claim}, {"goto", 2} } },
    },
    { -- state 2: COOLDOWN
      { every = 250,
        when = { {"elapsed", 0, ">=", {"cfg"}} },
        run = { {"goto", 1} } },                    -- READY's enter restores idle
    },
  } }
end

function std.totems.bonus()
  return pickup{ beacon = la.msg.BONUS_BEACON, idle = "BonusIdle",
                 claim = "Bonus", rgb = { 0, 180, 0 } }
end

function std.totems.malus()
  return pickup{ beacon = la.msg.MALUS_BEACON, idle = "MalusIdle",
                 claim = "Malus", rgb = { 200, 0, 0 } }
end

-- ---- FLAG: home/away flag stand for team 0 or 1. ------------------
-- Driven entirely by player MSG.FLAG_EVENT broadcasts, which players
-- emit only inside their own RSSI proximity gate — the totem inherits
-- that gate and needs no distance logic of its own.
function std.totems.flag(team)
  local fr = (team == 0) and 255 or 0      -- warm for O, cold for X
  local fb = (team == 0) and 0 or 255
  local FE = la.flag_event
  return { vm = 1, states = {
    { -- state 1: HOME
      { enter = true,
        run = { {"anim", "FlagIdle", {"rgb", fr, 80, fb}, {"rhythm", team}} } },
      { every = 500,
        run = { {"bcast", la.msg.FLAG_BEACON, 0, team} } },   -- 0 = FLAG_IN
      { msg = la.msg.FLAG_EVENT,
        when = { {"len", ">=", 2}, {"p", 2, "==", team},
                 {"p", 1, "==", FE.TAKEN} },
        run = { {"anim", "FlagMissing", {"rgb", fr, 80, fb}},
                {"anim", "FlagTaken", {"sender_player"}},
                {"goto", 2} } },
    },
    { -- state 2: AWAY (silent; DROPPED or SCORED returns the flag)
      { msg = la.msg.FLAG_EVENT,
        when = { {"len", ">=", 2}, {"p", 2, "==", team},
                 {"p", 1, ">=", FE.DROPPED}, {"p", 1, "<=", FE.SCORED} },
        -- goto runs HOME's enter (idle background) first, then the
        -- one-shot return flash plays over it.
        run = { {"goto", 1}, {"anim", "FlagReturn", {"rgb", fr, 80, fb}} } },
    },
  } }
end

-- ---- CP: control point (Upkeep: teams 0/1; KoH: slots 0-15). ------
-- R0 = owner slot (0xFF = neutral).  ACC accumulates presence bits
-- from reply sub-types 1..16 during each 2 s window; the window
-- rules then run in order (cont) and the epilogue clears ACC and
-- beacons the owner.  T0 times unchallenged control (10 s = point).
function std.totems.cp()
  local MSG = la.msg
  local POINT_MS = 10000        -- one point per emission period
  return { vm = 1, states = { {
    { enter = true,
      run = { {"set", 0, 0xFF}, {"start", 0},
              {"anim", "CPIdle", {"rgb", 80, 80, 80}} } },
    -- collect presence replies: sub-type 1..16 -> ACC bit 0..15
    { reply = MSG.CP_BEACON, cont = true,
      when = { {"len", ">=", 1}, {"p", 1, ">=", 1}, {"p", 1, "<=", 16} },
      run = { {"accbit", {"p", 1}} } },
    -- single occupant, different from owner: attach, pay for the
    -- capture at once and start the emission period from this moment.
    -- Taking a hill is the achievement; making the new owner wait a
    -- whole period before anything happens reads as "nothing happened".
    -- Actions run in order, so {"r",0} here is the owner just set.
    { every = 2000, cont = true,
      when = { {"acc", "single"}, {"low", "~=", {"r", 0}} },
      run = { {"set", 0, {"low"}}, {"start", 0},
              {"bcast", MSG.CP_SCORE, {"r", 0}},
              {"anim", "Control", {"args", 0xFE, {"r", 0}}} } },
    -- still the same lone owner one period later: another point, and
    -- the next period starts here.
    { every = 2000, cont = true,
      when = { {"acc", "single"}, {"low", "==", {"r", 0}},
               {"r", 0, "~=", 0xFF}, {"elapsed", 0, ">=", POINT_MS} },
      run = { {"bcast", MSG.CP_SCORE, {"r", 0}}, {"start", 0},
              {"anim", "Bonus"} } },
    -- contested: hold the current owner, show the contest
    { every = 2000, cont = true,
      when = { {"acc", "many"} },
      run = { {"anim", "ControlContest"} } },
    -- empty and never owned: stay visibly unclaimed
    { every = 2000, cont = true,
      when = { {"acc", "empty"}, {"r", 0, "==", 0xFF} },
      run = { {"anim", "CPIdle", {"rgb", 80, 80, 80}} } },
    -- window epilogue: open the next window, beacon the owner
    { every = 2000,
      run = { {"accclr"}, {"bcast", MSG.CP_BEACON, {"r", 0}} } },
  } } }
end

return std
