-- ================================================================
-- LightAir game: TiroBersaglio — the shooting gallery of a festival
-- stand, for the under-12s.
--
-- Like FestaSportSasso this is a stand ruleset, not a match: the
-- projectors are switched on in the morning, handed from one child to
-- the next all day, and THE MATCH ITSELF NEVER ENDS.  No
-- `scoring_state`, so there is no score collection, no winner
-- announcement and no end screen — and the A+B chord stays free for the
-- staff's hand-over key (see the header of festasportsasso.lua for the
-- full reasoning; it applies here word for word).
--
-- What ends and restarts is one child's turn.  A turn walks three
-- phases:
--
--   PRE_START  the welcome screen, while the projector is handed over.
--              The trial projector costs nothing, so the queue can take
--              as long as it likes, and only two colours exist: CLEAR is
--              free aiming practice, GREEN starts the turn.  Every other
--              target is ignored — a child learning to aim must not be
--              able to burn the first targets of their own run.
--   ACTIVE     the run itself: sub_time seconds and `charges` magazines
--              of `pool` energy to light six targets IN ORDER.
--   SUB_END    the frozen stats screen, led by
--              "Giocatore #<n> PUNTI: <score>".  The A+B chord — NOT
--              written on the screen, it is the staff's key — hands the
--              projector to the next child.
--
-- ---- The targets ------------------------------------------------
--
-- They are retroreflective panels, not players and not totems.  Enlight
-- classifies each one by colour and reports the matching player id
-- (PlayerDefs in src/config.h), which is the whole input of this game:
-- nothing here touches the radio, declares a totem, or can be reached
-- from another projector.  Two stands can run side by side.
--
--   1..6, in order   GREEN YELLOW BLUE ORANGE LIME MAGENTA   -> hits
--   RED              never, ends the turn on the spot
--   CLEAR            aim adjustment: no order, no effect, energy back
--
-- ---- The energy ------------------------------------------------
--
-- `charges` magazines of `pool` each — 2 x 50 by default.  The projector
-- itself owns ONE magazine (`energy`), so the reserve is counted here in
-- `reloads`, and what the child actually has left is the sum of the two:
--
--     energy_left = energy + reloads * pool
--
-- That sum is the number the score converts, so it is recomputed rather
-- than accounted: a CLEAR refund lands in the magazine and the sum
-- follows on its own, with nothing to keep in step.  There is no
-- automatic recharge at all (recharge = "none"): the second trigger is
-- the only way energy comes back, and only once the magazine is empty.
--
-- One consequence worth knowing before editing: the beam's result comes
-- back SEVERAL TICKS after the shot was paid for, so a turn that ended
-- the instant the pool hit zero would end before the CLEAR that refunds
-- that last shot could be seen.  Hence GRACE_MS — the empty-pool ending
-- waits it out, which also gives the last beam's feedback time to play.
-- ================================================================

local proj = la.lib("projector")

-- Enlight's colour ids, from PlayerDefs::playerNames in src/config.h.
local C = { CLEAR = 1, GREEN = 2, YELLOW = 3, BLUE = 4,
            ORANGE = 5, RED = 6, LIME = 7, MAGENTA = 8 }

-- The six numbered panels of the stand, in the order they must be lit,
-- and the names the child reads on the tray.  Kept short on purpose: the
-- LCD cell is 64 px wide minus the icon gutter.
local SEQ   = { C.GREEN, C.YELLOW, C.BLUE, C.ORANGE, C.LIME, C.MAGENTA }
local NAMES = { "VERDE", "GIALLO", "BLU", "ARANCIO", "LIME", "MAGENTA" }

local COST     = 1      -- one energy a beam, and what a CLEAR gives back
local GRACE_MS = 1000   -- see the header: the last beam's result is owed

-- The welcome screen's projector: free shots, so learning to aim costs
-- the child nothing and the stand nothing.  A separate profile rather
-- than a flag because the projector banks a pool per slot — switching to
-- the paying baseline at VIA! leaves the turn's magazine untouched.
local P_TRIAL = 20

proj.define{
  vars     = { energy = "energy", spent = "energy_spent" },
  profiles = {
    -- The baseline, retuned in place: one energy a beam out of a
    -- magazine the config menu sizes, and NO recharge — the reload is a
    -- deliberate press, never a wait.
    { id = 0, cost = COST, max_energy = "pool", recharge = "none" },
    { id = P_TRIAL, name = "TRIAL", cost = 0, max_energy = "pool",
      recharge = "none", strength = 1, ready_ms = 0 },
  },
}

local S = { PRE_START = 0, ACTIVE = 1, SUB_END = 2 }

-- ---- Private state ------------------------------------------------
local start_now   = false   -- the GREEN that opens the turn was lit
local over        = nil     -- the turn's ending, as the line that names it
local grace_until = 0       -- no empty-pool ending before this instant
local spent_seen  = 0       -- last `energy_spent` observed, to spot a beam
local reload_hint = false   -- the "press the second trigger" line, once

local function arm_trial(vars)
  proj.give(vars, P_TRIAL)
  proj.select(vars, P_TRIAL)
end

-- What the child has in hand and in reserve, as one number: this is what
-- the score converts, and 100 minus it is what they actually spent.
local function recount_energy(vars)
  vars.energy_left = vars.energy + vars.reloads * vars.pool
end

-- The standing tray line, rewritten on every target taken.  clear_tray
-- is the only way to replace a persistent line, so transients go up
-- AFTER this, never before.
local function show_next(vars)
  local n = vars.hits + 1
  la.clear_tray()
  if n <= #SEQ then
    vars.next = NAMES[n]
    la.show(string.format("Ora il %d: %s", n, NAMES[n]), 0)
  end
end

-- Hand-over: every turn number back to its start and the welcome screen
-- up.  `counter` is NOT reset — it belongs to the stand, not to the turn.
local function welcome(vars)
  vars.time_left = vars.sub_time
  vars.hits      = 0
  vars.red       = 0
  vars.reloads   = vars.charges - 1        -- one charge is in the magazine
  vars.score     = 0
  vars.next      = NAMES[1]
  start_now, over, spent_seen, reload_hint = false, nil, 0, false
  grace_until = 0
  proj.reset(vars)
  arm_trial(vars)
  recount_energy(vars)
  la.clear_tray()
  la.show("Il VERDE fa partire", 0)
  la.show("Mira sul BIANCO", 0)
  la.show(string.format("Giocatore %d", vars.counter), 0)
end

-- The GREEN was lit: the turn starts here, and that green IS target 1.
-- The clock is loaded now rather than at hand-over, so a projector can
-- wait on the welcome screen as long as the queue needs and every child
-- still gets the full sub_time.
local function start_turn(vars)
  vars.time_left = vars.sub_time
  vars.hits      = 1                       -- the green that opened the turn
  vars.reloads   = vars.charges - 1
  proj.reset(vars)                         -- back to the paying baseline
  start_now   = false
  spent_seen  = 0
  grace_until = 0
  reload_hint = false
  recount_energy(vars)
  show_next(vars)
  la.show("VIA!", 1500)
  la.ui("Up")
end

-- One classified target, in a running turn.
local function on_target(vars, id)
  -- CLEAR: the aiming panel.  No order, no effect, and the beam is given
  -- back — only the seconds it took are gone.  Its cue is the short
  -- version of a hit's, which is exactly what "Taken" is.
  if id == C.CLEAR then
    local room = vars.pool - vars.energy
    if room > 0 then
      vars.energy = vars.energy + ((COST < room) and COST or room)
    end
    la.ui("Taken")
    return
  end

  -- RED: never.  The turn ends here, and the score keeps only the
  -- targets — no time, no energy.
  if id == C.RED then
    vars.red = 1
    over = "Bersaglio ROSSO!"
    la.ui("Down")
    return
  end

  if id == SEQ[vars.hits + 1] then
    vars.hits = vars.hits + 1
    la.ui("Lit")
    if vars.hits >= #SEQ then
      vars.next = "--"
      over = "TUTTI I BERSAGLI!"
    else
      show_next(vars)
    end
    return
  end

  -- Any other panel: the right game, the wrong turn.  Costs a beam and a
  -- few seconds, nothing else.
  la.ui("Immune")
  la.show("Ordine sbagliato!", 1500)
end

-- Turn over: freeze the numbers and read them out.  The tray leads with
-- what the child came for — their number and their score.
local function sub_end(vars)
  recount_energy(vars)
  vars.score = vars.pt_target * vars.hits
  -- A red forfeits the leftovers, and only them: what was already lit is
  -- kept.  Without this, ending a turn early would PAY — the untouched
  -- energy and the unspent seconds would out-score a careful run.
  if vars.red == 0 then
    vars.score = vars.score + vars.energy_left + vars.time_left
  end
  la.clear_tray()
  la.show(string.format("Giocatore #%d PUNTI: %d", vars.counter, vars.score), 0)
  la.show(over or "Fine!", 3000)
  la.ui("EndGame")
end

return {
  api     = 1,
  type_id = 0x0009,               -- next free GameTypeId
  name    = "TiroBersaglio",

  initial_state = S.PRE_START,
  -- No scoring_state / score_msg on purpose: this game has no end, and
  -- that is what leaves the A+B chord to the hand-over rule below.

  config = {
    -- One child's turn, not the match: the match never ends.
    { id = "sub_time",      name = "Tempo",     min = 30, max = 300, step = 10, default = 100 },
    -- One magazine.  `charges` of these make the pool the score converts.
    { id = "pool",          name = "Energia",   min = 10, max = 100, step = 5,  default = 50  },
    { id = "charges",       name = "Ricariche", min = 1,  max = 4,   step = 1,  default = 2   },
    { id = "pt_target",     name = "PtBersagl", min = 0,  max = 100, step = 5,  default = 50  },
    -- Children already served before this device booted (0 on the first
    -- boot of the day; set it after a battery swap to resume the count).
    { id = "played_before", name = "Giocati",   min = 0,  max = 99,  step = 1,  default = 0   },
  },

  vars = {
    -- The magazine in hand, written by the projector on every beam.
    { id = "energy",       default = 50  },
    { id = "energy_spent", default = 0   },
    -- Magazine + reserve: the number the score converts.  Recomputed,
    -- never accounted — see the header.
    { id = "energy_left",  default = 100 },
    { id = "reloads",      default = 1   },
    { id = "time_left",    default = 100, countdown_in = { S.ACTIVE } },
    { id = "hits",         default = 0   },
    -- 0 or 1: a red ends the turn, so it can never be lit twice.  Kept as
    -- a cell because it is what says WHY the turn ended.
    { id = "red",          default = 0   },
    { id = "score",        default = 0   },
    { id = "counter",      default = 1   },
    -- The colour still to light, spelled out for a child who cannot be
    -- expected to remember the order.
    { id = "next", text = true, len = 8, default = "VERDE" },
    -- Battery, read on the welcome screen: between turns is the only
    -- moment anyone looks at a projector without playing it, so it is
    -- where a flat one has to be caught.
    { id = "batt", text = true, len = 8, default = "--" },
  },

  monitor = {
    { var = "counter",     icon = "ROLE",   col = 0, row = 0, states = { S.PRE_START } },
    { var = "batt",        icon = "LIGHT",  col = 1, row = 0, states = { S.PRE_START } },
    { var = "time_left",   icon = "TIME",   col = 0, row = 1, states = { S.PRE_START } },
    { var = "energy_left", icon = "ENERGY", col = 1, row = 1, states = { S.PRE_START } },

    { var = "time_left",   icon = "TIME",   col = 0, row = 0, states = { S.ACTIVE } },
    { var = "energy",      icon = "ENERGY", col = 1, row = 0, states = { S.ACTIVE } },
    { var = "next",        icon = "ROLE",   col = 0, row = 1, states = { S.ACTIVE } },
    { var = "reloads",     icon = "LIFE",   col = 1, row = 1, states = { S.ACTIVE } },

    -- The four numbers of the turn, frozen.
    { var = "energy_left", icon = "ENERGY", col = 0, row = 0, states = { S.SUB_END } },
    { var = "time_left",   icon = "TIME",   col = 1, row = 0, states = { S.SUB_END } },
    { var = "hits",        icon = "SCORE",  col = 0, row = 1, states = { S.SUB_END } },
    { var = "red",         icon = "DOWN",   col = 1, row = 1, states = { S.SUB_END } },
  },

  -- Never elected (no scoring state), but declared so the descriptor
  -- stays complete if a variant of this file ever grows an ending.
  winners = {
    { var = "score", dir = "max" },
    { var = "red",   dir = "min" },
  },

  -- No totems and no radio: the targets are passive panels read by
  -- Enlight, so there is nothing to activate and nothing to answer.
  totem_slots = {},
  teams = 0,

  on_begin = function(vars)
    vars.counter = vars.played_before + 1
    welcome(vars)
    la.ui("GameStart")
  end,

  rules = {
    { from = S.PRE_START, to = S.ACTIVE,
      when   = function() return start_now end,
      action = start_turn },

    -- A red, or all six taken: the ending was decided inside update, and
    -- the line that names it travels with it.
    { from = S.ACTIVE, to = S.SUB_END,
      when   = function() return over ~= nil end,
      action = sub_end },

    { from = S.ACTIVE, to = S.SUB_END,
      when   = function(vars) return vars.time_left <= 0 end,
      action = function(vars)
        over = "Tempo scaduto!"
        sub_end(vars)
      end },

    -- Energy gone, both magazines.  The grace window is the last beam's:
    -- a CLEAR result still on its way refunds it, and the turn goes on.
    { from = S.ACTIVE, to = S.SUB_END,
      when   = function(vars)
        return vars.energy_left <= 0 and la.now() >= grace_until
      end,
      action = function(vars)
        over = "Energia finita!"
        sub_end(vars)
      end },

    -- The staff's key: A+B together hands the projector to the next
    -- child.  Deliberately not written on the stats screen.
    { from = S.SUB_END, to = S.PRE_START,
      when   = function() return la.key_down("A") and la.key_down("B") end,
      action = function(vars)
        vars.counter = vars.counter + 1
        welcome(vars)
        la.ui("GameStart")
      end },
  },

  update = {
    -- Welcome screen: free aim, the green that starts the turn, and the
    -- battery.  Nothing reaches the radio, and no other colour is even
    -- looked at — the run's own targets must not be reachable from here.
    [S.PRE_START] = function(vars)
      local id = proj.result(vars)
      if id == C.CLEAR then
        la.ui("Taken")
      elseif id == C.GREEN then
        start_now = true
      end
      proj.tick(vars)

      local v = la.sensor(1)                     -- 1 = battery divider
      vars.batt = v and string.format("%.2fV", v) or "--"
    end,

    [S.ACTIVE] = function(vars)
      -- The ending is already decided; the rule fires next tick.  Nothing
      -- more may land on numbers that are about to be read out.
      if over then return end

      local id = proj.result(vars)
      if id then on_target(vars, id) end

      -- The reload: the second trigger, and only with the magazine
      -- actually empty.  "pressed" is the press edge, so holding it down
      -- cannot spend the reserve a magazine at a time.
      if vars.energy <= 0 and vars.reloads > 0
         and la.trigger_state(2) == "pressed" then
        vars.energy  = vars.pool
        vars.reloads = vars.reloads - 1
        reload_hint  = false
        la.ui("Bonus")
        la.show("RICARICA!", 1500)
      end

      proj.tick(vars)

      -- A beam Enlight accepted, seen through the cost it charged: the
      -- turn may not end on an empty pool until that beam's own result
      -- has had time to come back.
      if vars.energy_spent > spent_seen then
        spent_seen  = vars.energy_spent
        grace_until = la.now() + GRACE_MS
      end

      recount_energy(vars)

      if vars.energy <= 0 and vars.reloads > 0 and not reload_hint then
        reload_hint = true
        la.show("Premi il 2o grilletto", 2500)
      end
    end,
  },
}
