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
--   } } }
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
--       rssi     = -57,
--       on_ready = function(vars) can_respawn = true end,
--   }
-- ----------------------------------------------------------------
function std.base_respawn(cfg)
  return function(vars, pkt)
    if cfg.when and not cfg.when(vars) then return end
    if pkt.len < 1 then return end
    local base_team = pkt:byte(1)
    local mine      = cfg.team and cfg.team() or nil
    local ok        = (mine ~= nil and base_team == mine)
                   or (cfg.teamless and base_team == 0xFF)
    if not ok then return end
    if pkt.rssi < (cfg.rssi or -57) then return end
    cfg.on_ready(vars)
    -- Intentional reply: sub-type = slot+1 so the BASE shows a
    -- respawn animation (empty auto-replies are ignored by bases).
    return (mine or (la.my_id() - 1)) + 1
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
    if w < 0 then      la.show("TIE!", 0)
    elseif w == 0 then la.show("TEAM O WINS!", 0)
    else               la.show("TEAM X WINS!", 0) end
  end
end

-- ================================================================
-- Standard totem roles — ports of src/totem-rulesets/*.cpp.
-- Use in a game file:
--   totems = { BASE_O = std.totems.base(0), BONUS = std.totems.bonus(), ... }
-- ================================================================

local function team_rgb(team)
  if team == 0xFF then return 255, 255, 255 end   -- white = teamless
  local c = la.colors.team[team] or la.colors.team[0]
  return c[1], c[2], c[3]
end

local function player_rgb(id)
  local c = la.colors.player[id] or la.colors.player[0]
  return c[1], c[2], c[3]
end

-- ---- BASE: respawn base.  team = 0, 1 or "any" (teamless). --------
function std.totems.base(team)
  local tv = (team == "any") and 0xFF or team
  return {
    on_activate = function(t)
      t.last_beacon = 0
      local r, g, b = team_rgb(tv)
      local rh = la.rhythm[tv] or la.rhythm[0]
      la.totem_ui("BaseIdle", r, g, b, rh.period, rh.pulses)
    end,
    on_message = function(t, pkt)
      -- Accept only *intentional* respawn replies (sub-type >= 1);
      -- the runner's empty auto-replies carry no proximity info.
      if pkt.msg ~= la.msg.BASE_BEACON + 1 then return end
      if pkt.len == 0 or pkt:byte(1) == 0 then return end
      local r, g, b
      if tv == 0xFF then r, g, b = player_rgb(pkt.sender)
      else               r, g, b = team_rgb(pkt.team < 2 and pkt.team or 0) end
      la.totem_ui("Respawn", r, g, b)
    end,
    update = function(t)
      if la.now() - t.last_beacon >= 1000 then
        t.last_beacon = la.now()
        la.broadcast(la.msg.BASE_BEACON, tv)
      end
    end,
  }
end

-- ---- BONUS / MALUS: claimable pickup with cooldown. ---------------
local function pickup(cfg)
  return {
    on_activate = function(t)
      t.ready, t.cooldown_end, t.last_beacon = true, 0, 0
      la.totem_ui(cfg.idle, cfg.rgb[1], cfg.rgb[2], cfg.rgb[3])
    end,
    on_message = function(t, pkt)
      if t.ready and pkt.msg == cfg.beacon + 1 then
        t.ready        = false
        t.cooldown_end = la.now() + (t.config_secs or 30) * 1000
        la.totem_ui(cfg.claim)
      end
    end,
    update = function(t)
      if not t.ready then
        if la.now() >= t.cooldown_end then
          t.ready = true
          la.totem_ui(cfg.idle, cfg.rgb[1], cfg.rgb[2], cfg.rgb[3])
        end
        return
      end
      if la.now() - t.last_beacon >= 2000 then
        t.last_beacon = la.now()
        la.broadcast(cfg.beacon, 0)     -- payload byte 0 = ready
      end
    end,
  }
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
function std.totems.flag(team)
  -- Flag colours match FlagTotem.cpp: warm for O, cold for X.
  local fr = (team == 0) and 255 or 0
  local fg = 80
  local fb = (team == 0) and 0 or 255

  local function show_idle()
    local rh = la.rhythm[team] or la.rhythm[0]
    la.totem_ui("FlagIdle", fr, fg, fb, rh.period, rh.pulses)
  end

  return {
    on_activate = function(t)
      t.home, t.last_beacon = true, 0
      show_idle()
    end,
    on_message = function(t, pkt)
      -- Driven entirely by player MSG.FLAG_EVENT broadcasts, which the
      -- players emit only inside their own RSSI proximity gate.
      if pkt.msg ~= la.msg.FLAG_EVENT then return end
      if pkt.len < 2 or pkt:byte(2) ~= team then return end
      local sub = pkt:byte(1)
      if t.home then
        if sub == la.flag_event.TAKEN then
          t.home = false
          la.totem_ui("FlagMissing", fr, fg, fb)
          local pr, pg, pb = player_rgb(pkt.sender)
          la.totem_ui("FlagTaken", pr, pg, pb)
        end
      elseif sub == la.flag_event.DROPPED or sub == la.flag_event.SCORED then
        t.home = true
        show_idle()
        la.totem_ui("FlagReturn", fr, fg, fb)
      end
    end,
    update = function(t)
      if not t.home then return end
      if la.now() - t.last_beacon >= 500 then
        t.last_beacon = la.now()
        la.broadcast(la.msg.FLAG_BEACON, 0, team)   -- 0 = FLAG_IN
      end
    end,
  }
end

-- ---- CP: control point (Upkeep: teams 0/1; KoH: slots 0-15). ------
function std.totems.cp()
  return {
    on_activate = function(t)
      t.owner        = 0xFF                -- 0xFF = neutral
      t.presence     = 0                   -- bit i = slot i replied this window
      t.window_start = la.now()
      t.attach_start = la.now()
      la.totem_ui("CPIdle", 80, 80, 80)
    end,
    on_message = function(t, pkt)
      -- Presence replies: sub-type 1..16 -> slot 0..15.
      if pkt.msg ~= la.msg.CP_BEACON + 1 then return end
      if pkt.len == 0 then return end
      local sub = pkt:byte(1)
      if sub >= 1 and sub <= 16 then
        t.presence = t.presence | (1 << (sub - 1))
      end
    end,
    update = function(t)
      local now = la.now()
      if now - t.window_start < 2000 then return end

      -- Evaluate the 2 s window that just closed.
      local any    = t.presence ~= 0
      local single = any and (t.presence & (t.presence - 1)) == 0
      if any then
        local new_owner = t.owner
        if single then
          new_owner = 0
          while (t.presence >> new_owner) & 1 == 0 do
            new_owner = new_owner + 1
          end
        end
        if new_owner ~= t.owner then
          t.owner        = new_owner       -- switch: no point, restart countdown
          t.attach_start = now
          if t.owner < 2 then la.totem_ui("Control", t.owner)
          else                la.totem_ui("Control", 0xFF, t.owner + 1) end
        elseif single and t.owner ~= 0xFF
               and now - t.attach_start >= 10000 then
          la.broadcast(la.msg.CP_SCORE, t.owner)
          la.totem_ui("Bonus")
          t.attach_start = now
        end
        if not single then la.totem_ui("ControlContest") end
      elseif t.owner == 0xFF then
        la.totem_ui("CPIdle", 80, 80, 80)
      end

      -- Open the next window.
      t.presence     = 0
      t.window_start = now
      la.broadcast(la.msg.CP_BEACON, t.owner)
    end,
  }
end

return std
