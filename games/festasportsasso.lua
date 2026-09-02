-- ================================================================
-- LightAir game: FestaSportSasso — the never-ending King of Hill of
-- a festival stand.
--
-- The projectors are switched on in the morning and handed from one
-- visitor to the next all day, so THE MATCH ITSELF NEVER ENDS: this
-- ruleset declares no scoring state, so there is no score
-- collection, no winner announcement and no end screen.  The game
-- stops when the stand is powered off, not before.
--
-- What ends and restarts is the *sub-game*: one visitor's turn,
-- sub_time seconds long (500 by default), played with King of Hill
-- rules — shine the others, hold the CP totems, respawn at a BASE.
-- A turn walks through four phases:
--
--   PRE_START  the welcome screen — "Welcome player <counter>" — while
--              the projector is handed over.  Nothing else works until
--              a BASE totem respawns its holder, which is both the
--              visitor's way in and how the staff starts the turn: the
--              BASE plays its respawn animation, the King of Hill
--              screen comes up and the turn clock starts at sub_time.
--   ACTIVE     the King of Hill sub-game, turn clock running.
--   DOWN       shone: "SHONE by <player>" / "GO TO BASE" on the tray
--              while the clock keeps running; a BASE brings you back
--              after respawn_secs — exactly as in King of Hill.
--   SUB_END    the clock ran out: a frozen stats screen leading with
--              "#<counter> POINTS: <score>" (score = 10 per CP totem
--              point plus 1 per player lit this turn).  The A+B chord
--              (deliberately NOT written on the screen — it is the
--              staff's key, not the visitor's) starts the next turn.
--
-- The player counter is the one number that survives a restart. It
-- reads as "<visitors before this one><projector digit>": 2 = the
-- first visitor on projector 2, 12 = the second one, 122 = the
-- thirteenth.  The A+B restart bumps the first part by one; the last
-- digit is this device's player id and never changes, because the
-- projector doesn't.  played_before seeds the first part, so a
-- battery swap mid-festival can resume the count instead of
-- restarting it.
--
-- Two consequences of "never ends" worth knowing before editing:
--   * no `scoring_state`  -> the runner never collects scores, never
--     floods MSG_END_GAME and never arms its own A+B reboot, which
--     is what leaves the chord free for the turn restart below;
--   * no `time_left_var`  -> the 0xF1 activation reply reports
--     0xFFFF instead of the turn clock, so a totem activated at any
--     point of the day never arms its self-revert watchdog.  Wiring
--     the turn clock there would strand every totem 10 s after the
--     first turn ended.
-- ================================================================

local std  = la.lib("std")
local proj = la.lib("projector")

-- The baseline profile reproduces what std.shiner did here: one
-- energy per beam, a full refill after the configured idle, both
-- read from this game's own config vars.
-- TRIAL is the welcome screen's projector: free shots so a first-time
-- visitor can learn to aim while the queue moves, and no accounting at
-- all.  cost 0 means the pool is never touched and never runs out, and
-- recharge "none" means there is nothing to wait for.  It is a separate
-- profile rather than a flag because the projector already banks a pool
-- per slot: switching to it leaves the turn's energy exactly where it
-- was, and switching back restores it.
local P_TRIAL = 20

proj.define{
  vars     = { energy = "energy", spent = "energy_spent",
               reload = "reload", reload_ms = "reload_ms" },
  profiles = {
    { id = P_TRIAL, name = "TRIAL", cost = 0, max_energy = "start_energy",
      recharge = "none", strength = 1, ready_ms = 0 },
  },
}

-- Hand the welcome screen its practice projector.  proj.reset() rebuilds
-- the inventory down to the baseline, so this runs after every reset that
-- lands us on the welcome screen.
local function arm_trial(vars)
  proj.give(vars, P_TRIAL)
  proj.select(vars, P_TRIAL)
end

local S   = { PRE_START = 0, ACTIVE = 1, DOWN = 2, SUB_END = 3 }
local MSG = la.msg
local R   = { TAKEN = 1, SHONE = 2, DOWN = 3, IMMUNE = 4 }

local NEAR_CP_RSSI      = -60   -- ~2.5 m: CP presence gate, to join or capture
-- Once counted as present, a CP is held (or contested) at this looser
-- reach instead — earned, not given: reaching it the first time still
-- needs NEAR_CP_RSSI.  Applies to anyone who was just present, not only
-- the recorded owner, so a multi-way contest does not flicker apart on
-- signal noise near the tight gate while it is still being fought over.
local NEAR_CP_RSSI_HOLD = -70
local NEAR_BASE_RSSI    = -57   -- ~2 m: BASE respawn gate
local PICKUP_RSSI       = -57   -- ~2 m: BONUS/MALUS claim gate
local CP_NONE           = 0xFF

-- ---- Private state ------------------------------------------------
local my_slot     = 0           -- player id - 1; set in on_begin
local cp_ids      = {}          -- [i] = device id of the i-th CP totem
local cp_owner    = {}          -- [i] = last announced owner slot or CP_NONE
local cp_engaged  = {}          -- [i] = was our presence counted last window?
local respawn_at  = 0
local can_respawn = false
local shone_by    = nil         -- short name of whoever put us down
local imm         = std.immunity(3000)

local function cp_index(sender)
  for i, id in ipairs(cp_ids) do
    if id == sender then return i end
  end
  return nil
end

-- cp_engaged persists on this physical projector across visitors (the
-- device, not the person, is what the CP totems know) and across a
-- respawn, so it has to be cleared explicitly wherever a turn or a life
-- restarts — an un-reset flag would hand a brand-new visitor the loose
-- gate on a CP the PREVIOUS visitor was holding.
local function reset_cp_engagement()
  for i = 1, #cp_engaged do cp_engaged[i] = false end
end

-- CP beacon: track ownership changes (both playing states); declare
-- presence by returning slot+1 only when send_presence and close enough.
--
-- The totem's own accbit rule has no RSSI guard at all — it counts
-- whoever replies, full stop (see docs/totem-behavior-handshake.md,
-- "RSSI is readable, not policy").  Proximity is entirely this
-- decision: whether to reply, and at what reach.  cp_engaged[idx] is
-- the hysteresis: fail to answer for ANY reason — too far, or shone
-- and therefore not sending presence at all — and it drops, so the
-- next attempt needs the tight gate again.
local function cp_beacon_handler(send_presence)
  return function(vars, pkt)
    local idx = cp_index(pkt.sender)
    if not idx or pkt.len < 1 then return end

    local owner = pkt:byte(1)
    if owner ~= cp_owner[idx] then
      local prev = cp_owner[idx]
      cp_owner[idx] = owner
      -- Only the two players actually affected by the change learn
      -- about it — the loser and the gainer — not the whole field.
      if owner == my_slot then
        la.show("Totem acquired", 3000)
        la.ui("FlagReturn")
      elseif prev == my_slot then
        la.show("Totem lost", 3000)
        la.ui("FlagTaken")
      end
    end

    if not send_presence then
      cp_engaged[idx] = false
      return
    end

    local gate = cp_engaged[idx] and NEAR_CP_RSSI_HOLD or NEAR_CP_RSSI
    cp_engaged[idx] = pkt.rssi >= gate
    if cp_engaged[idx] then return my_slot + 1 end
  end
end

local function cp_score_handler(vars, pkt)
  local idx = cp_index(pkt.sender)
  if not idx or pkt.len < 1 then return end
  if pkt:byte(1) == my_slot then
    vars.points = vars.points + 1
    -- A point is what the visitor is here for: cue it and name the hill
    -- that paid it, instead of letting the score cell tick by unnoticed.
    la.show(string.format("CP %d +1", idx), 2000)
    la.ui("ControlGain")
  end
end

-- Sender side: a confirmed lit is one more player lit, whether the
-- target survived it or went down.  `announce` is the tray line for the
-- one a visitor wants to see — the beam is invisible and the buzzer
-- cannot name anybody, so this is where they learn who they put down.
local function counted_lit(ui_event, announce)
  return function(vars, reply)
    vars.players_lit = vars.players_lit + 1
    if announce then
      la.show(la.player_short(reply.sender) .. announce, 3000)
    end
    la.ui(ui_event)
  end
end

-- Shone: out until a BASE picks us up again.  The turn clock keeps
-- running, so a long wait costs the visitor playing time.  Two
-- persistent lines say who put them down and what to do about it; the
-- "Down" cue is the moment feedback, so no transient line competes.
local function go_down(vars)
  vars.shone_times = vars.shone_times + 1
  respawn_at  = la.now() + vars.respawn_secs * 1000
  can_respawn = false
  la.show("GO TO BASE", 0)
  la.show("SHONE by " .. (shone_by or "?"), 0)
  la.ui("Down")
end

-- Turn over: freeze the numbers and read them out.  The tray leads with
-- the number that matters to the visitor: their player number and the
-- score for this turn — 10 points per CP totem point, 1 per player lit.
local function sub_end(vars)
  vars.tally = string.format("%d/%d", vars.players_lit, vars.shone_times)
  la.clear_tray()
  la.show(string.format("#%d POINTS: %d",
                        vars.counter, 10 * vars.points + vars.players_lit), 0)
  la.show("Time up!", 3000)
  la.ui("EndGame")
end

-- Hand-over: every turn stat back to zero and the welcome screen up.
-- The player counter is NOT reset here — it belongs to the stand, not
-- to the turn, and it is what the welcome line names the visitor by.
local function welcome(vars)
  -- The playing numbers are loaded here as well, so the welcome screen
  -- shows a fresh turn instead of the last visitor's leftovers; the
  -- ones that matter are loaded again, for real, in start_turn.
  vars.lives        = vars.start_lives
  vars.energy       = vars.start_energy
  vars.time_left    = vars.sub_time
  vars.points       = 0
  vars.energy_spent = 0
  vars.shone_times  = 0
  vars.players_lit  = 0
  vars.tally        = "0/0"
  respawn_at  = 0
  can_respawn = false
  shone_by    = nil
  imm.reset()
  proj.reset(vars)
  arm_trial(vars)
  reset_cp_engagement()
  la.clear_tray()
  la.show("Go to a BASE!", 0)
  la.show(string.format("Welcome player %d", vars.counter), 0)
end

-- A BASE let the visitor in: this is where the turn actually begins.
-- The clock is loaded here rather than at hand-over, so a projector
-- can wait on the welcome screen as long as the queue needs and every
-- visitor still gets the full sub_time.  The BASE totem answers for
-- itself: std.base_respawn replied with slot+1, which is what makes it
-- play its respawn animation in this player's colour.
local function start_turn(vars)
  vars.lives     = vars.start_lives
  vars.energy    = vars.start_energy
  vars.time_left = vars.sub_time
  can_respawn = false
  shone_by    = nil
  imm.reset()
  proj.reset(vars)
  reset_cp_engagement()
  la.clear_tray()
  la.show("Play!", 2000)
  la.ui("Up")
end

return {
  api     = 1,
  type_id = 0x0008,               -- next free GameTypeId
  name    = "FestaSportSasso",

  initial_state = S.PRE_START,
  -- No scoring_state / score_msg on purpose: this game has no end.

  config = {
    { id = "start_lives",   name = "Lives",     min = 1,  max = 5,   step = 1,  default = 3   },
    { id = "respawn_secs",  name = "Respawn",   min = 5,  max = 120, step = 5,  default = 30  },
    { id = "start_energy",  name = "Energy",    min = 10, max = 100, step = 10, default = 50  },
    { id = "recharge_secs", name = "Recharge",  min = 0,  max = 20,  step = 5,  default = 10  },
    -- One visitor's turn, not the match: the match never ends.
    { id = "sub_time",      name = "SubTime",   min = 60, max = 900, step = 10, default = 500 },
    -- Visitors already served before this device booted (0 on the
    -- first boot of the day; set it after a battery swap to resume
    -- the count where it stopped).
    { id = "played_before", name = "Played",    min = 0,  max = 99,  step = 1,  default = 0   },
  },

  vars = {
    { id = "lives",        default = 3   },
    { id = "energy",       default = 50  },
    { id = "time_left",    default = 500, countdown_in = { S.ACTIVE, S.DOWN } },
    { id = "points",       default = 0   },
    { id = "energy_spent", default = 0   },
    -- The projector's reload clock, read by the energy cell's bar:
    -- reload = millis the wait began (0 = not waiting), reload_ms =
    -- how long it takes.  Both written by projector.lua.
    { id = "reload",       default = 0 },
    { id = "reload_ms",    default = 0 },
    -- Battery, read on the welcome screen: between turns is the only
    -- moment anyone looks at a projector without playing it, so it is
    -- where a flat one has to be caught.  Text, because "4.05V" reads and
    -- "405" does not.
    { id = "batt", text = true, len = 8, default = "--" },
    { id = "shone_times",  default = 0   },
    { id = "players_lit",  default = 0   },
    -- <visitors before this one><projector digit>, see the header.
    { id = "counter",      default = 0   },
    -- Two stats, one cell: "<players lit>/<times shone>".  The stats
    -- screen has four cells and five numbers to show.
    { id = "tally",        text = true, len = 8, default = "0/0" },
  },

  monitor = {
    { var = "counter",      icon = "ROLE",   col = 0, row = 0,
      states = { S.PRE_START, S.DOWN, S.SUB_END } },
    { var = "batt",         icon = "LIGHT",  col = 1, row = 0,
      states = { S.PRE_START } },
    { var = "time_left",    icon = "TIME",   col = 0, row = 1,
      states = { S.PRE_START, S.ACTIVE, S.DOWN } },
    { var = "lives",        icon = "LIFE",   col = 0, row = 0, states = { S.ACTIVE } },
    -- Energy, and — while the pool is empty — a bar filling over the
    -- recharge.  The projector owns both the duration and the instant
    -- the wait began, because a refill starts at the trigger's RELEASE,
    -- not when the pool hit zero.
    { var = "energy",       icon = "ENERGY", col = 1, row = 0, states = { S.ACTIVE },
      bar = true, bar_at = 0, fill_var = "reload_ms", start_var = "reload" },
    { var = "points",       icon = "SCORE",  col = 1, row = 1,
      states = { S.ACTIVE, S.DOWN, S.SUB_END } },
    -- Stats screen: counter + lit/shone above, energy spent + points below.
    { var = "tally",        icon = "LIFE",   col = 1, row = 0, states = { S.SUB_END } },
    { var = "energy_spent", icon = "ENERGY", col = 0, row = 1, states = { S.SUB_END } },
  },

  -- Never elected (no scoring state), but declared so the descriptor
  -- stays complete and a fused score would still mean something if a
  -- variant of this file ever grows an ending.
  winners = {
    { var = "points",      dir = "max" },
    { var = "shone_times", dir = "min" },
  },

  totem_slots = {
    { role = "CP",    min = 1, max = 6 },      -- at least one hill
    { role = "BASE",  min = 1, max = 4 },      -- teamless: starts and respawns
    { role = "BONUS", min = 0, max = 16 },
    { role = "MALUS", min = 0, max = 16 },
  },
  teams = 0,
  -- time_left_var deliberately absent — see the header.

  on_begin = function(vars)
    my_slot = la.my_id() - 1
    -- Concatenation, done in arithmetic: the projector digit is the
    -- units, the visitor count everything above it.  Ids 10-16 fold
    -- onto their last digit, so give a stand's projectors ids 1-9.
    vars.counter = vars.played_before * 10 + (la.my_id() % 10)
    welcome(vars)
    cp_ids, cp_owner, cp_engaged = {}, {}, {}
    for i = 0, 5 do
      local id = la.totem_for_role("CP", i)
      if id == 0 then break end
      cp_ids[#cp_ids + 1]     = id
      cp_owner[#cp_owner + 1] = CP_NONE
      cp_engaged[#cp_engaged + 1] = false
    end
    la.ui("GameStart")
  end,

  on_message = {
    -- On the welcome screen: no lives to lose, no CP to hold.  The one
    -- thing that reaches us is the BASE that starts the turn, and the
    -- slot+1 answer std.base_respawn returns is what lights it up.
    [S.PRE_START] = {
      [MSG.LIT]         = function() return R.DOWN end,
      [MSG.BASE_BEACON] = std.base_respawn{
        teamless = true,                            -- teamless bases only
        rssi     = NEAR_BASE_RSSI,
        on_ready = function() can_respawn = true end,
      },
    },
    [S.ACTIVE] = {
      -- A pickup totem gives itself to whoever answers, so only answer
      -- from arm's length: the claim has to mean "I am standing at it".
      [MSG.BONUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI },
      [MSG.MALUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI },
      [MSG.LIT] = std.lit_target{
        lives = "lives", immunity = imm,
        reply = { taken = R.TAKEN, shone = R.SHONE, immune = R.IMMUNE },
        on_shone = function(_, pkt) shone_by = la.player_short(pkt.sender) end,
      },
      [MSG.CP_BEACON] = cp_beacon_handler(true),
      [MSG.CP_SCORE]  = cp_score_handler,
    },
    [S.DOWN] = {
      [MSG.LIT]       = function() return R.DOWN end,
      [MSG.CP_BEACON] = cp_beacon_handler(false),   -- track only; can't capture
      [MSG.CP_SCORE]  = cp_score_handler,
      [MSG.BASE_BEACON] = std.base_respawn{
        when     = function() return la.now() >= respawn_at end,
        teamless = true,
        rssi     = NEAR_BASE_RSSI,
        on_ready = function() can_respawn = true end,
      },
    },
    -- Stats screen: frozen.  No CP handlers, so no presence is
    -- declared and no point can land on a number already read out.
    [S.SUB_END] = {
      [MSG.LIT] = function() return R.DOWN end,
    },
  },

  on_reply = {
    [MSG.LIT] = {
      [R.TAKEN]  = counted_lit("Taken"),
      [R.SHONE]  = counted_lit("Lit", " SHONE!"),
      [R.DOWN]   = function() la.ui("AlreadyDown") end,
      [R.IMMUNE] = function() la.ui("Immune")      end,
    },
  },

  rules = {
    { from = S.PRE_START, to = S.ACTIVE,
      when   = function() return can_respawn end,
      action = start_turn },

    { from = S.ACTIVE, to = S.SUB_END,
      when   = function(vars) return vars.time_left <= 0 end,
      action = sub_end },
    { from = S.ACTIVE, to = S.DOWN,
      when   = function(vars) return vars.lives <= 0 end,
      action = go_down },

    { from = S.DOWN, to = S.SUB_END,
      when   = function(vars) return vars.time_left <= 0 end,
      action = sub_end },
    { from = S.DOWN, to = S.ACTIVE,
      when   = function() return can_respawn end,
      action = function(vars)
        vars.lives  = vars.start_lives
        vars.energy = vars.start_energy
        can_respawn = false
        shone_by    = nil
        imm.reset()
        la.clear_tray()             -- drop the credit and the instruction
        la.show("Back in game!", 1000)
        la.ui("Up")
      end },

    -- The staff's key: A+B together hands the projector to the next
    -- visitor.  Not shown on the stats screen on purpose.
    { from = S.SUB_END, to = S.PRE_START,
      when   = function() return la.key_down("A") and la.key_down("B") end,
      action = function(vars)
        -- One more visitor served: bump the counter's first part and
        -- leave its last digit (this projector) alone.
        vars.counter = vars.counter + 10
        welcome(vars)
        la.ui("GameStart")
      end },
  },

  update = {
    -- Welcome screen: aim practice, and the battery.
    --
    -- Nothing here reaches the radio.  A visitor learning to aim must not
    -- be able to touch a turn already in progress, so a hit tells this
    -- projector and nobody else: no MSG.LIT goes out, and the target
    -- never learns it was lit.  The feedback is the beam's own UI burst
    -- (proj.tick plays it on every accepted shot) plus the standard hit
    -- cue when the beam actually finds a player-coloured target.
    [S.PRE_START] = function(vars)
      if proj.result(vars) then la.ui("Taken") end
      proj.tick(vars)

      local v = la.sensor(1)                      -- 1 = battery divider
      vars.batt = v and string.format("%.2fV", v) or "--"
    end,

    [S.ACTIVE] = function(vars)
      -- Everyone is a valid target: no team check needed.
      local target = proj.result(vars)
      if target then la.send(target, MSG.LIT, proj.payload(vars)) end
      proj.tick(vars)
    end,
  },

  totems = {
    CP    = std.totems.cp(),
    BASE  = std.totems.base("any"),
    BONUS = std.totems.bonus(),
    MALUS = std.totems.malus(),
  },
}
