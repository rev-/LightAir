-- ================================================================
-- LightAir game: Virus — infection tag.  Last clean player wins.
--
-- One player starts as the VIRUS (chosen by the host with the
-- virus_id config var — pick a player who is actually in the
-- session).  The role is stated on the LCD ("VIRUS" / "CLEAN"),
-- and a red pulsing background alert marks the infected device.
--
-- The virus pays for its power:
--   * a cooldown between one shine and the next (virus_cooldown ms),
--   * an energy pool of only a FIFTH of the clean players' maximum.
--
-- Whoever is lit by a virus becomes a virus too (the lit carries a
-- "viral" payload byte; a clean player's shine has no effect).
-- New infections are announced with a flooded broadcast so every
-- device tracks how many clean players remain.
--
-- The game ends when at most one clean player is left, or when the
-- time runs out.  Winner: whoever stayed clean longest
-- (clean_secs, max); tie-break: most infections caused.
-- ================================================================

local std  = la.lib("std")
local proj = la.lib("projector")

local S   = { CLEAN = 0, VIRUS = 1, GAME_END = 2 }
local MSG = la.msg

-- Game-private message: infection announcement (flooded broadcast,
-- no payload; the sender IS the newly infected player).
-- Custom even msgType from the 0x10 player block; safe to pick here
-- because typeId + sessionToken isolate games on the wire.  Never
-- reuse the 0xA0 infrastructure or 0xF0 totem-protocol blocks.
local MSG_INFECTED = 0x16

-- Reply sub-types for MSG.LIT.
local R = { NOEFFECT = 1, INFECTED = 2, VIRUS = 3 }

local PICKUP_RSSI = -57         -- ~2 m: BONUS/MALUS claim gate

-- Continuous red pulse + soft vibration on the infected device.
local virus_bg = {
  priority = 1,
  steps = {
    { ms = 250, freq = 3500, vib = 20, rgb = { 255, 0, 0 } },
    { ms = 400,                        rgb = {  40, 0, 0 } },
  },
}

-- ---- Private state ------------------------------------------------
local virus_set        = {}      -- [playerId] = true once infected
local virus_count      = 0
local pending_infected = false   -- a viral lit reached us this cycle
-- The two roles are two projectors.  They differ in exactly two things:
-- how long Enlight must cool between beams, and the role tag the beam
-- carries — so the cooldown stops being hand-timed here and becomes the
-- optics' own, and "is this beam viral" rides the payload field meant for
-- it instead of colliding with the projector's strength byte.
--
-- Both draw on energy_max, which become_virus() lowers for the virus, so
-- the pool rule the game already had is untouched.
-- CLEAN is the baseline, so everyone starts holding it; VIRUS is granted
-- on infection and never given back, which is the shape of the game.
local P_VIRUS = 11
proj.define{
  vars     = { energy = "energy", spent = "energy_spent",
               reload = "reload", reload_ms = "reload_ms" },
  profiles = {
    { id = 0, name = "CLEAN", cooldown_ms = 0,
      max_energy = "energy_max",
      recharge_delay_ms = function(v) return (v.recharge_secs or 0) * 1000 end,
      role_tag = 0 },
    { id = P_VIRUS, name = "VIRUS", cooldown_ms = "virus_cooldown",
      max_energy = "energy_max",
      recharge_delay_ms = function(v) return (v.recharge_secs or 0) * 1000 end,
      role_tag = 1 },
  },
}
local was_active       = false   -- trigger release edge for recharge
local release_at       = 0

local function is_virus() return virus_set[la.my_id()] == true end

local function note_infected(vars, id)
  if id and not virus_set[id] then
    virus_set[id] = true
    virus_count   = virus_count + 1
    vars.clean_left = la.player_count() - virus_count
  end
end

local function become_virus(vars)
  if not virus_set[la.my_id()] then
    virus_set[la.my_id()] = true
    virus_count = virus_count + 1
    la.broadcast_relay(MSG_INFECTED)
  end
  vars.clean_left = la.player_count() - virus_count
  vars.clean_secs = vars.game_time - vars.time_left
  vars.role       = "VIRUS"
  -- The virus recharges to only a fifth of the others' energy max.
  vars.energy_max = math.max(1, vars.start_energy // 5)
  if vars.energy > vars.energy_max then vars.energy = vars.energy_max end
  proj.grant(vars, P_VIRUS)       -- longer cooldown, and a viral role tag
  la.background(virus_bg)
  la.show("YOU ARE THE VIRUS!", 0)
  la.ui("RoleChange")
end

local function last_clean(vars) return vars.clean_left <= 1 end

local function game_over(vars)
  la.background()
  la.clear_tray()
  la.show("Game over!", 3000)
  la.ui("EndGame")
end

return {
  api     = 1,
  type_id = 0x0007,               -- next free GameTypeId
  name    = "Virus",

  initial_state = S.CLEAN,
  scoring_state = S.GAME_END,
  score_msg     = MSG.SCORE_COLLECT,

  config = {
    -- Host must pick a player id that is present in the session.
    { id = "virus_id",       name = "Virus",    min = 1,   max = 16,   step = 1,   default = 1    },
    { id = "start_energy",   name = "Energy",   min = 10,  max = 100,  step = 10,  default = 50   },
    { id = "recharge_secs",  name = "Recharge", min = 0,   max = 20,   step = 5,   default = 10   },
    { id = "virus_cooldown", name = "CoolMs",   min = 250, max = 3000, step = 250, default = 1000 },
    { id = "game_time",      name = "Time",     min = 60,  max = 900,  step = 60,  default = 600  },
  },

  vars = {
    { id = "energy",     default = 50 },
    { id = "energy_spent", default = 0 },
    -- The projector's reload clock, read by the energy cell's bar:
    -- reload = millis the wait began (0 = not waiting), reload_ms =
    -- how long it takes.  Both written by projector.lua.
    { id = "reload",       default = 0 },
    { id = "reload_ms",    default = 0 },
    { id = "energy_max", default = 50 },
    { id = "time_left",  default = 600, countdown_in = { S.CLEAN, S.VIRUS } },
    { id = "clean_left", default = 0  },   -- clean players remaining
    { id = "infections", default = 0  },   -- players this device infected
    { id = "clean_secs", default = 0  },   -- how long we stayed clean
    -- The role, clearly stated on the LCD in both playing states.
    { id = "role", text = true, len = 8, default = "CLEAN" },
  },

  monitor = {
    -- CLEAN screen
    { var = "role",       icon = "ROLE",   col = 0, row = 0, states = { S.CLEAN } },
    { var = "clean_left", icon = "LIFE",   col = 1, row = 0, states = { S.CLEAN } },
    { var = "time_left",  icon = "TIME",   col = 0, row = 1, states = { S.CLEAN, S.VIRUS } },
    -- Energy, and — while the pool is empty — a bar filling over the
    -- recharge.  The projector owns both the duration and the instant
    -- the wait began, because a refill starts at the trigger's RELEASE,
    -- not when the pool hit zero.
    { var = "energy",     icon = "ENERGY", col = 1, row = 1, states = { S.CLEAN, S.VIRUS },
      bar = true, bar_at = 0, fill_var = "reload_ms", start_var = "reload" },
    -- VIRUS screen
    { var = "role",       icon = "ROLE",   col = 0, row = 0, states = { S.VIRUS } },
    { var = "infections", icon = "SCORE",  col = 1, row = 0, states = { S.VIRUS } },
    -- GAME_END screen
    { var = "clean_secs", icon = "TIME",   col = 0, row = 0, states = { S.GAME_END } },
    { var = "infections", icon = "SCORE",  col = 1, row = 0, states = { S.GAME_END } },
    { var = "clean_left", icon = "LIFE",   col = 0, row = 1, states = { S.GAME_END } },
    { var = "role",       icon = "ROLE",   col = 1, row = 1, states = { S.GAME_END } },
  },

  winners = {
    { var = "clean_secs", dir = "max" },   -- last clean player wins
    { var = "infections", dir = "max" },   -- tie-break: most infections caused
  },

  totem_slots = {
    { role = "BONUS", min = 0, max = 16 },
    { role = "MALUS", min = 0, max = 16 },
  },
  teams = 0,
  time_left_var = "time_left",

  on_begin = function(vars)
    vars.energy_max = vars.start_energy
    vars.time_left  = vars.game_time
    vars.infections = 0
    vars.clean_secs = 0
    vars.role       = "CLEAN"
    virus_set        = {}
    virus_count      = 0
    pending_infected = false
    proj.reset(vars)                -- CLEAN in hand, pool full, optics pushed
    last_shine       = 0
    was_active       = false
    release_at       = 0

    -- Everybody knows the patient zero from the config blob.
    virus_set[vars.virus_id] = true
    virus_count     = 1
    vars.clean_left = la.player_count() - 1
    if la.my_id() == vars.virus_id then
      pending_infected = true     -- the CLEAN->VIRUS rule fires on tick 1
    end
    la.ui("GameStart")
  end,

  on_message = {
    [S.CLEAN] = {
      -- A pickup totem gives itself to whoever answers, so only answer
      -- from arm's length: the claim has to mean "I am standing at it".
      [MSG.BONUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI },
      [MSG.MALUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI },
      [MSG.LIT] = function(vars, pkt)
        -- Only a viral lit infects; a clean player's lit has no effect.
        -- Byte 3 is the projector's role tag — byte 1 is its strength.
        if pkt.len >= 3 and pkt:byte(3) == 1 then
          pending_infected = true
          note_infected(vars, pkt.sender)   -- sender is certainly a virus
          return R.INFECTED
        end
        return R.NOEFFECT
      end,
      [MSG_INFECTED] = function(vars, pkt)
        note_infected(vars, pkt.sender)
        la.show(la.player_short(pkt.sender) .. " is INFECTED", 3000)
      end,
    },
    [S.VIRUS] = {
      -- A pickup totem gives itself to whoever answers, so only answer
      -- from arm's length: the claim has to mean "I am standing at it".
      [MSG.BONUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI },
      [MSG.MALUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI },
      [MSG.LIT] = function() return R.VIRUS end,   -- already infected
      [MSG_INFECTED] = function(vars, pkt)
        note_infected(vars, pkt.sender)
        la.show(la.player_short(pkt.sender) .. " is INFECTED", 3000)
      end,
    },
  },

  on_reply = {
    [MSG.LIT] = {
      [R.INFECTED] = function(vars, reply)
        vars.infections = vars.infections + 1
        la.show(la.player_short(reply.sender) .. " INFECTED!", 3000)
        la.ui("Lit")
      end,
      [R.NOEFFECT] = function() la.ui("Taken")  end,
      [R.VIRUS]    = function() la.ui("Immune") end,
    },
  },

  rules = {
    { from = S.CLEAN, to = S.GAME_END,
      when   = function(vars) return vars.time_left <= 0 or last_clean(vars) end,
      action = function(vars)
        -- Still clean at the end: full survival time (possibly the win).
        vars.clean_secs = vars.game_time - vars.time_left
        game_over(vars)
      end },
    { from = S.CLEAN, to = S.VIRUS,
      when   = function() return pending_infected end,
      action = function(vars)
        pending_infected = false
        become_virus(vars)
      end },
    { from = S.VIRUS, to = S.GAME_END,
      when   = function(vars) return vars.time_left <= 0 or last_clean(vars) end,
      action = game_over },
  },

  update = {
    -- Both roles run the same body: which projector is in hand already
    -- carries the difference, in the cooldown and in the role tag.
    [S.CLEAN] = function(vars)
      local target = proj.result(vars)
      if target then la.send(target, MSG.LIT, proj.payload(vars)) end
      proj.tick(vars)
    end,
    [S.VIRUS] = function(vars)
      local target = proj.result(vars)
      if target then la.send(target, MSG.LIT, proj.payload(vars)) end
      proj.tick(vars)
    end,
  },

  totems = {
    BONUS = std.totems.bonus(),
    MALUS = std.totems.malus(),
  },
}
