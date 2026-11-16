-- ================================================================
-- LightAir projector — the light-beam device a player carries.
--
-- Load with:  local proj = la.lib("projector")
--
-- This is the object between the game, the ruleset and Enlight.  It owns
-- what used to be spread across every ruleset's shine loop: the optics in
-- hand, the energy that pays for a beam, how that energy comes back, how
-- far the beam reaches, what a hit weighs on the wire, and which
-- projectors the player is carrying.
--
-- It replaces std.shiner, which was the same idiom with one fixed
-- profile.
--
-- ---- Layering ----------------------------------------------------
--
-- Enlight never learns what a projector is.  Three verbs carry
-- everything across: la.shine_config{} pushes the optics, la.shine()
-- starts a burst, la.shine_result() reports what came back.  Every
-- decision — whether to fire, what it cost, whether the target was in
-- range, what the hit weighs — is made here, in Lua, where it can be
-- retuned by shipping a file instead of reflashing.
--
-- ---- Why range is decided here -----------------------------------
--
-- la.shine_result() reports an ESTIMATED DISTANCE; it does not gate on
-- it.  That is deliberate: gating in the driver would freeze one policy
-- into firmware, while here a profile can gate on distance, correct for
-- target colour, or grade an effect by range — all as data.
--
-- ---- Allocation --------------------------------------------------
--
-- Everything allocates at define() time.  tick() runs allocation-free.
-- ================================================================

local P = { }

-- ---- Declaration, filled by define() -----------------------------
local cfg          = nil   -- the whole declaration
local defs         = {}    -- id -> clamped profile
local var          = {}    -- role -> var id, see define()

-- ---- Inventory ---------------------------------------------------
-- Slot 1 is the baseline: always held, never counted against
-- max_owned, never evicted.  Slots 2..n are powered.
local slots        = {}    -- { id, energy, acquired_at, ramp_at }
local active_idx   = 1

-- ---- Shared pool -------------------------------------------------
-- define{ shared_pool = true }: every projector the player holds draws
-- from ONE pool, the baseline's — for a ruleset where the pool is the
-- player's life (Outflow).  The pool's size, its recharge and DIM are the
-- baseline's; each projector still brings its own optics, cost per beam,
-- strength, feedback and area.  A pickup puts a projector in hand without
-- touching the pool (a full pool would be a free heal), and switching
-- moves no energy.  The pool's state (its ramp clock) lives in slot 1.
local shared       = false

-- ---- Live state --------------------------------------------------
local was_active   = false
local release_at   = 0
-- Where the recharge wait counts from.  The release, unless the profile
-- has gates (std.gates): a gate passed moves it back, a gate missed moves
-- it forward, and the bar and the refill both follow it.
local wait_from    = 0
local gate         = nil   -- the gated wait in progress, see tick_recharge
local gate_for     = nil   --   ...and the release it belongs to
-- True between an accepted beam and the trigger release that follows it.
-- The recharge wait STARTS at that release, so nothing may tick while this
-- is set — otherwise a player who had been idle would see the pool refill
-- on the very tick they emptied it.
local awaiting_release = false
local ready_at     = 0     -- millis before which trigger() refuses
local evicted_name = nil
-- `<` and `>` step through what the player holds (define{ switch_keys =
-- false } turns it off).  Both together are a chord some rulesets use, so
-- a key that was part of one switches nothing.
local switch_keys  = true
local chord        = false

-- ---- The beam in flight, and what the press is building ----------
-- All of this is the shining side's own business: none of it travels,
-- only the strength the beam ends up with.
local shot_units     = nil  -- strength of the hit result() just returned; nil = the profile's
local beam_mul       = 1    -- what the beam in flight was charged to
local held_beams     = 0    -- beams accepted during this press (spinup)
local burst_left     = 0    -- beams this press still owes (semi / burst)
local charge_from    = nil  -- millis the charge began
local charge_ready   = nil  -- a charged beam waiting for Enlight to take it
-- The lock: how many beams in a row landed on one target.
--   closing  the press ended; deliver at the next result()
--   deaf     delivered; ignore measurements until the next beam
--   warned   millis of the last warning sent to the target
--   acked    ...and whether the target's device said it arrived
local lock = { target = nil, n = 0, last = 0, closing = false, deaf = false,
               warned = nil, acked = false }
-- A lock warns the player it is building on, and keeps warning: this
-- often while it holds, and the warning stands this long at the target —
-- longer than the interval, so one lost message does not blink it off.
-- A warning nobody acknowledged is sent again much sooner.
local LOCK_WARN_EVERY_MS = 1000
local LOCK_WARN_RETRY_MS = 300
local LOCK_WARN_HOLD_MS  = 1500

-- ---- DIM (a MALUS) -----------------------------------------------
-- While dimmed, every projector the player holds has a smaller pool, a
-- slower recharge and a longer cooldown.  Fixed factors: retune here.
-- min_cooldown_ms stands in for a profile that declares no cooldown at
-- all (the baseline) — doubling nothing would not be "longer".
local DIM = { energy_div = 2, recharge_mul = 2, cooldown_mul = 2,
              min_cooldown_ms = 250 }
local dimmed = false
-- Why: a MALUS holds it until the player goes out, a `dim` beam until its
-- time is up.  Dimmed while either says so.
local dim_hold, dim_until = false, 0

-- ---- What the player's class changes (set_mods) -------------------
-- Multipliers over every projector the player holds.  1 = no change.
local mods = { energy = 1, recharge = 1, cooldown = 1, strength = 1 }

-- ---- What beams did to this player --------------------------------
local mute_until   = 0     -- cannot shine before this
local expose_until = 0     -- the next beam weighs more until this
local trace = { until_ms = 0, next_ms = 0, shiner = 0, team = 0xFF }
local locked_until = 0     -- someone is locking on to this player until this
-- Traced players this device may follow: [player id] = { rssi, millis }.
local seen = {}

-- ---- The game's unit ----------------------------------------------
-- define{ game_unit = n }: what ONE standard unit of strength is worth in
-- this game's own currency.  1 for a coarse game (1 to 5 lives), 10 for a
-- fine one (about 100): a profile states its strength once, in standard
-- units, and the wire carries strength * game_unit.  A profile that only
-- makes sense fine-grained says min_unit = 10 and is simply not known to
-- a coarser game.  proj.units(n) is the same scaling for any other field.
local unit = 1

-- ---- What a beam says about itself --------------------------------
-- role_tag: who is shining (the player's class or role), set by the
-- ruleset.  kind and effects are the projector's, plus what the ruleset
-- switched on at run time (a power-up, a rule, a key): `sticky` survives
-- strip(), the rest is lost on going out.
local role_tag   = 0
local kind_on,  kind_sticky = 0, 0
local fx_on,    fx_sticky   = 0, 0
local bonus_ids = {}       -- BONUS option label -> projector id, see bonus_options()

-- ================================================================
--   Limits.  The load-time equivalent of the C++ clamp the projector
--   used to carry: a typo in a game file is corrected once, here,
--   instead of reaching the hardware or the balance.
--
--   The three optical bounds are re-applied by la.shine_config; these
--   exist so a bad value is visible at load rather than silently
--   corrected at the boundary.
-- ================================================================
local LIM = {
  cycles            = { 1,  100 },
  cooldown_ms       = { 0,  10000 },
  range_m           = { 0,  100 },     -- 0 = whatever the device can see
  cost              = { 0,  10 },
  max_energy        = { 0,  200 },
  strength          = { 0,  10 },      -- standard units, see game_unit
  kind              = { 0,  255 },     -- a mask of 8, the game's to define
  game_unit         = { 1,  25 },
  rssi_min          = { -120, 0 },     -- dBm; 0 = no gate
  ready_ms          = { 0,  5000 },
  -- Milliseconds, not seconds: a recharge quantised to whole seconds is far
  -- too coarse to separate a projector that snaps back from one that
  -- crawls.
  recharge_delay_ms = { 0, 60000 },
  recharge_ms       = { 0, 60000 },
  recharge_step_ms  = { 1, 60000 },
  max_owned         = { 1,  8 },
  burst             = { 1,  10 },
  overheat_ms       = { 0,  60000 },
  uses              = { 0,  1000 },    -- 0 = no limit
  lifetime_ms       = { 0,  3600000 }, -- 0 = no limit
  on_lit_refund     = { 0,  200 },
}

-- Fields that were renamed, and what replaced them.  Named rather than
-- silently ignored: a profile that keeps the old spelling would otherwise
-- get the field's default and behave subtly wrong.
local RETIRED = {
  recharge_delay_secs = "recharge_delay_ms",
  recharge_secs       = "recharge_ms",
  -- The role tag is the PLAYER's now (their class or role); what the
  -- projector says about itself is its kind.
  role_tag            = "kind, or proj.set_role_tag()",
}

-- ================================================================
--   EFFECTS — what a beam does at the receiver besides its strength.
--
--   Each is one bit of the LIT's 16-bit effects word, so stacking costs
--   nothing on the wire.  A profile lists the ones it always carries
--   (effects = { "dim", "beacon" }); proj.set_effect() switches one on or
--   off at run time for whatever is in hand.  What an effect DOES, and for
--   how long, is this table's — the same on every device, because every
--   device runs this file — so a stronger variant is another entry, not a
--   parameter of the beam.  define{ effects = { dim = { ms = 8000 } } }
--   retunes one for a game.
--
--   Only receiver-side effects live here.  What the shining projector
--   does on its own (lock, charge, fire modes) sends nothing but the
--   strength it ends up with.
--
--   Bits 0-8 are the library's, 9-15 go to profiles that declare an area
--   of their own, in id order.
-- ================================================================
local FX = {
  restore         = { bit = 0 },                 -- strength is given, not taken
  dim             = { bit = 1, ms = 5000 },      -- the receiver is dimmed
  mute            = { bit = 2, ms = 3000 },      -- the receiver cannot shine
  expose          = { bit = 3, ms = 5000, mul = 2 }, -- the next beam weighs more
  siphon          = { bit = 4, amount = 5 },     -- energy leaves the receiver
  beacon          = { bit = 5, ms = 20000, every_ms = 1000, to = "all" },
  ignore_immunity = { bit = 6 },
  silent          = { bit = 7 },                 -- the receiver is not told who
  splash          = { bit = 8 },                 -- the standard SPLASH area
}
local FX_FIRST_FREE, FX_LAST = 9, 15
-- A traced player repeats a radio message; this is as often as it may.
local TRACE_MIN_MS = 1000
local TRACE_TO     = { shiner = 0, team = 1, all = 2 }
-- name -> resolved entry.  The library's own until define() retunes it
-- for a game, so a receiver that reads a beam before any define() — a
-- ruleset with no projector declaration of its own — still knows them.
local fx = {}
for name, e in pairs(FX) do fx[name] = e end

local function fx_mask(list, who)
  local m = 0
  for _, name in ipairs(list or {}) do
    local e = fx[name]
    if not e then
      error("projector " .. tostring(who) .. ": unknown effect '" .. tostring(name) .. "'", 0)
    end
    m = m | (1 << e.bit)
  end
  return m
end

local RECHARGE = { refill = true, ramp = true, none = true }
local MODE     = { auto = true, semi = true, burst = true }
local CURVE    = { linear = true, square = true, fib = true }
local DELIVER  = { release = true, beam = true }

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

-- A profile field is a literal number, the id of a game var, or a function
-- of vars.  The var-id form is what lets a value the config menu owns (the
-- energy pool, the recharge time) track the menu without the profile being
-- rebuilt — the property std.shiner had, kept.  The function form covers
-- the rest: a config var in seconds feeding a field in milliseconds, say.
local function val(vars, v, dflt)
  if v == nil then return dflt end
  local t = type(v)
  if t == "string"   then return vars[v] or dflt end
  if t == "function" then return v(vars) or dflt end
  return v
end

-- Clamp only literals: a var id is resolved per tick and clamped there.
local function clamp_field(p, field)
  local lim = LIM[field]
  if lim and type(p[field]) == "number" then
    p[field] = clamp(p[field], lim[1], lim[2])
  end
end

-- ================================================================
--   THE BASELINE
--
--   One energy per beam, a full refill after the configured idle, pool
--   and delay read from the game's own config vars.  Its optics are
--   declared outright rather than left to whatever Enlight last held:
--   a baseline with no cycles of its own would inherit the previous
--   projector's, so switching back from LONG would keep LONG's beam.
--
--   range_m is a LABEL here and everywhere: what the profile is meant to
--   reach.  Nothing gates on it (see result()).
-- ================================================================
-- ICON_ENERGY, resolved once: a profile that names no icon, or names one
-- the firmware does not carry, keeps the standard energy glyph.
local ICON_FALLBACK = (la.icons and la.icons.ENERGY) or 0

local BASELINE = {
  id                  = 0,
  name                = "BASE",
  cycles              = 10,
  cooldown_ms         = 50,
  range_m             = 40,
  cost                = 1,
  max_energy          = "start_energy",
  recharge            = "refill",
  -- The menu owns this one in seconds; the projector works in ms.
  recharge_delay_ms   = function(vars) return (vars.recharge_secs or 0) * 1000 end,
  strength            = 1,
  kind                = 0,
  rssi_min            = 0,
  ready_ms            = 0,
}

-- ================================================================
--   Relations to the baseline.
--
--   A powered projector is a power-up only relative to the baseline it
--   replaces in hand.  Fixed numbers cannot promise that: the menu owns
--   the baseline's pool and recharge, so a strong enough baseline would
--   out-shoot every "powered" one.  So the standard profiles state their
--   values as functions of the baseline's RESOLVED value — whatever this
--   game declared for id 0, read through its config vars on the tick.
--
--   Every field is stated that way, including the ones the baseline
--   fixes today (cycles, cooldown): they may become menu values, and a
--   relation keeps holding when they do.
--
--     cycles = proj.rel("cycles", function(r) return r * 5 end)
--     cost   = proj.rel("cost")                   -- same as the baseline
--
--   The result is clamped to the field's LIM, like a literal at load.
-- ================================================================
local function base_value(vars, field)
  local b = defs[0]
  return val(vars, b and b[field], 0)
end

local function rel(field, f)
  local lim = LIM[field]
  return function(vars)
    local v = base_value(vars, field)
    if f then v = f(v) end
    if lim then v = clamp(v, lim[1], lim[2]) end
    return v
  end
end
P.rel = rel

-- n standard units in this game's currency, for any field that is not
-- scaled on its own (strength is; a cost charged to "lives" is not):
--   self_cost = { var = "lives", amount = proj.units(1) }
function P.units(n)
  return function() return n * unit end
end
function P.game_unit() return unit end

local function half(x)       return x // 2 end
local function twice(x)      return x * 2 end
-- A pool may never round down to nothing: a projector with no energy
-- could be held but never fired.
local function half_pool(x)  return math.max(1, x // 2) end

-- ================================================================
--   STANDARD PROFILES
--
--   Ready-made profiles a game can drop straight into its `profiles`
--   list.  Their ids are FIXED and reserved, because a projector id
--   travels on the wire: a LIT names the projector that fired, and every
--   receiver looks the profile up by that id locally.  A game's own
--   profiles should start above this range.
--
--     profiles = { proj.standard.SPLASH, { id = 10, name = "MINE", ... } }
--
--   Each is the baseline with a few relations changed (B = baseline):
--
--             pool   recharge               cooldown  cycles  strength
--     FAST    B      ramp: B/2 - 500 ms     B/2       B       B
--                    idle, then 10 ms/unit
--     STRONG  B/2    B/2                    2B        B       3
--     SPLASH  B/2    B/2                    2B        B       B + area
--     LONG    B/2    B                      B/2       5B      B
--
--   Every one costs what the baseline costs.  Every receiver resolves
--   these against the same config, so a profile looked up by id means the
--   same thing on every device.
-- ================================================================
P.standard = {
  -- SPLASH — the burst projector.  The point is not the direct hit but
  -- what it does to everyone standing near the person it lands on: the
  -- direct hit is a single standard hit, while the beacon it triggers
  -- hands out two at close range and one further out.
  --
  -- It is the ONLY profile that declares an area.  An area is loud, in
  -- radio traffic and in play, and a field where every projector splashed
  -- would be chaos rather than tactics.
  SPLASH = {
    id       = 1,
    name     = "SPLASH",
    icon     = "SPLASH",

    -- Optics: the baseline's beam, twice the wait between beams, so it is
    -- a considered shot rather than a held trigger.
    cycles      = rel("cycles"),
    cooldown_ms = rel("cooldown_ms", twice),
    range_m     = 40,        -- label only

    -- Economy: half the pool, back in half the time.
    cost              = rel("cost"),
    max_energy        = rel("max_energy", half_pool),
    recharge          = "refill",
    recharge_delay_ms = rel("recharge_delay_ms", half),

    -- Handling: heavy to bring up after a switch.
    ready_ms = 600,

    -- Effect: a direct hit weighs what the baseline's does.  Repeat hits
    -- on one target are the ruleset's immunity window, as for every
    -- projector.
    strength = rel("strength"),

    -- The area its hits throw around whoever they land on: an area policy
    -- for the firmware's area service (la.area_policy, registered by
    -- define() under this projector's id) and triggered by the "splash"
    -- EFFECT — so anything in hand splashes while that effect is on.  The
    -- firmware triggers it, grades it, credits it; the ruleset's own LIT
    -- handler decides what a hit does.
    effects = { "splash" },
    area = {
      effect   = "splash",
      on       = "lit",      -- every hit that lands bursts, the knock-out included
      -- Graded: RSSI is coarse, so a misread moves a bystander one band
      -- rather than between hit and nothing.
      bands    = { { -55, 2 }, { -70, 1 } },
      friendly = "game",     -- the ruleset's friendly fire, against the shooter
      self     = false,      -- the shooter is never caught in their own area
      credit   = true,       -- an area knock-out scores for the shooter
    },

    -- A punch that flares out: the step ms are a SHAPE, scaled to fit the
    -- real burst, so what identifies the projector is the pattern rather
    -- than one note's pitch.  Short and hard, then longer and lower.
    shine_action = {
      priority = 2,
      steps = { { ms = 1, freq = 1400, vib = 255, rgb = { 255, 200, 0 } },
                { ms = 3, freq =  700, vib = 200, rgb = { 255,  60, 0 } } },
    },
  },

  -- FAST — the baseline's beam with half the wait between beams, and a
  -- pool that comes back sooner and trickles rather than arriving all at
  -- once, which suits a projector meant to be held down: an idle of half
  -- the baseline's recharge less half a second, then one unit every
  -- 10 ms.  It is the only standard profile that ramps.
  FAST = {
    id       = 2,
    name     = "FAST",
    icon     = "FAST",

    cycles      = rel("cycles"),
    cooldown_ms = rel("cooldown_ms", half),
    range_m     = 30,        -- label only

    cost              = rel("cost"),
    max_energy        = rel("max_energy"),
    recharge          = "ramp",
    recharge_delay_ms = rel("recharge_delay_ms", function(ms) return ms // 2 - 500 end),
    recharge_step_ms  = 10,

    ready_ms = 150,
    strength = rel("strength"),

    -- Two even rising ticks: light, quick, unmistakably not the heavy ones.
    shine_action = {
      priority = 1,
      steps = { { ms = 1, freq = 2600, vib = 80, rgb = {   0, 180, 255 } },
                { ms = 1, freq = 3400, vib = 90, rgb = { 120, 220, 255 } } },
    },
  },

  -- LONG — five times the baseline's integration, which is what reaches
  -- further.  Half the pool, but each beam is five times the light and
  -- the wait between beams is half the baseline's.  Slow to bring up.
  LONG = {
    id       = 3,
    name     = "LONG",
    icon     = "LONG",

    cycles      = rel("cycles", function(r) return r * 5 end),
    cooldown_ms = rel("cooldown_ms", half),
    range_m     = 0,         -- label only: no limit

    cost              = rel("cost"),
    max_energy        = rel("max_energy", half_pool),
    recharge          = "refill",
    recharge_delay_ms = rel("recharge_delay_ms"),

    ready_ms = 400,
    strength = rel("strength"),

    -- A short chirp then a long held tone: reads as "reaching out".
    shine_action = {
      priority = 1,
      steps = { { ms = 1, freq = 2000, vib = 120, rgb = { 220, 120, 255 } },
                { ms = 4, freq = 1200, vib = 150, rgb = { 180,   0, 255 } } },
    },
  },

  -- STRONG — the heavy hitter: one beam weighs three standard hits, which
  -- in a lives game is three lives at once.  Half the pool, back in half
  -- the time, and twice the wait between beams.
  STRONG = {
    id       = 4,
    name     = "STRONG",
    icon     = "STRONG",

    cycles      = rel("cycles"),
    cooldown_ms = rel("cooldown_ms", twice),
    range_m     = 40,        -- label only

    cost              = rel("cost"),
    max_energy        = rel("max_energy", half_pool),
    recharge          = "refill",
    recharge_delay_ms = rel("recharge_delay_ms", half),

    ready_ms = 400,
    strength = 3,

    -- Three descending notes, evenly spaced: heavy, and nothing else in the
    -- catalogue has three.
    shine_action = {
      priority = 1,
      steps = { { ms = 1, freq = 1000, vib = 255, rgb = { 255, 140, 0 } },
                { ms = 1, freq =  800, vib = 255, rgb = { 255,  80, 0 } },
                { ms = 1, freq =  600, vib = 230, rgb = { 200,  30, 0 } } },
    },
  },
}

-- ================================================================
--   define(declaration)
--
--     proj.define{
--       vars = { energy = "energy", spent = "energy_spent",
--                reload = "reload", reload_ms = "reload_ms" },
--       max_owned = 3,
--       shared_pool = false,          -- true: one pool for all (see above)
--       profiles = { { id = 1, name = "STRONG", ... }, ... },
--     }
--
--   Profile id 0 is the baseline.  Declaring one replaces the standard
--   baseline's values in place; it is still structural and undroppable.
--
--   recharge is "refill" (the default), "ramp" or "none"; anything else
--   refuses the load rather than recharging some other way.
-- ================================================================
-- A profile as define() keeps it: a copy, checked, with its literals
-- clamped.  nil for one this game's unit is too coarse for (min_unit).
local function prepare(raw)
  if (raw.min_unit or 1) > unit then return nil end
  local p = {}
  for k, v in pairs(raw) do p[k] = v end
  local who = tostring(p.name or p.id)
  -- A profile still naming a retired key would silently get that field's
  -- default, so say so at load rather than shipping a projector that
  -- behaves subtly wrong.
  for old, new in pairs(RETIRED) do
    if p[old] ~= nil then
      error("projector profile '" .. who .. "' uses retired field '" .. old ..
            "'; use " .. new, 0)
    end
  end
  if p.recharge ~= nil and not RECHARGE[p.recharge] then
    error("projector profile '" .. who ..
          "': recharge must be \"refill\", \"ramp\" or \"none\"", 0)
  end
  if p.mode ~= nil and not MODE[p.mode] then
    error("projector profile '" .. who ..
          "': mode must be \"auto\", \"semi\" or \"burst\"", 0)
  end
  local L = p.lock
  if L then
    if L.curve ~= nil and type(L.curve) ~= "table" and not CURVE[L.curve] then
      error("projector profile '" .. who ..
            "': lock.curve must be \"linear\", \"square\", \"fib\" or a table", 0)
    end
    if L.deliver ~= nil and not DELIVER[L.deliver] then
      error("projector profile '" .. who ..
            "': lock.deliver must be \"release\" or \"beam\"", 0)
    end
  end
  if p.self_cost and type(p.self_cost.var) ~= "string" then
    error("projector profile '" .. who .. "': self_cost needs a var", 0)
  end
  for k in pairs(LIM) do clamp_field(p, k) end
  return p
end

function P.define(decl)
  cfg  = decl or {}
  var  = cfg.vars or {}
  defs = {}
  shared = cfg.shared_pool and true or false
  switch_keys = cfg.switch_keys ~= false
  unit = clamp(cfg.game_unit or 1, LIM.game_unit[1], LIM.game_unit[2])

  -- The effects table, with this game's retuning.  The bit is the wire's
  -- and is not a game's to move.
  fx = {}
  for name, e in pairs(FX) do
    local r = {}
    for k, v in pairs(e) do r[k] = v end
    fx[name] = r
  end
  for name, over in pairs(cfg.effects or {}) do
    if not fx[name] then error("projector: unknown effect '" .. tostring(name) .. "'", 0) end
    for k, v in pairs(over) do
      if k ~= "bit" then fx[name][k] = v end
    end
  end

  if fx.beacon.every_ms < TRACE_MIN_MS then fx.beacon.every_ms = TRACE_MIN_MS end
  if not TRACE_TO[fx.beacon.to] then
    error("projector: effect beacon: to must be \"shiner\", \"team\" or \"all\"", 0)
  end

  local base = {}
  for k, v in pairs(BASELINE) do base[k] = v end
  defs[0] = base

  -- A ruleset may say which projectors exist in it: `only` lists the ids
  -- that do, `except` the ids that do not.  One that does not exist is not
  -- known at all — never given, never offered by a BONUS totem's menu —
  -- which is how a game mode limits the field to a few, or keeps one out.
  -- The baseline always exists.
  local only, except = nil, {}
  if cfg.only then
    only = {}
    for _, id in ipairs(cfg.only) do only[id] = true end
  end
  for _, id in ipairs(cfg.except or {}) do except[id] = true end
  local function exists(id)
    return id == 0 or ((not only or only[id]) and not except[id])
  end

  for _, raw in ipairs(cfg.profiles or {}) do
    local p = exists(raw.id or 0) and prepare(raw) or nil
    if p then
      local id = p.id or 0
      if id == 0 then
        -- Retune the baseline in place rather than naming a different id:
        -- everything else treats slot 1 as structural.
        for k, v in pairs(p) do defs[0][k] = v end
      else
        defs[id] = p
      end
    end
  end

  -- The standard catalogue is always KNOWN, declared or not: a BONUS totem
  -- may hand any of it to any game.  Known is not owned — the inventory
  -- still starts with the baseline alone.
  for _, std_p in pairs(P.standard) do
    if not defs[std_p.id] and exists(std_p.id) then defs[std_p.id] = prepare(std_p) end
  end

  -- A profile's area is a policy for the firmware's area service, declared
  -- under the projector's own id and triggered by an EFFECT: a landed LIT
  -- carrying that effect's bit, which the target's ruleset answered TAKEN /
  -- SHONE, makes the target the centre.  Every device registers the same
  -- policies with the same bits, because every device runs the same file
  -- and the bits are handed out in id order.
  local ids = {}
  for id, p in pairs(defs) do
    if p.area then ids[#ids + 1] = id end
  end
  table.sort(ids)
  local next_bit = FX_FIRST_FREE
  for _, id in ipairs(ids) do
    local p, a = defs[id], defs[id].area
    assert(id ~= 0, "the baseline projector cannot carry an area")
    local name = a.effect or string.lower(tostring(p.name or id))
    if not fx[name] then
      if next_bit > FX_LAST then error("projector: too many area effects", 0) end
      fx[name] = { bit = next_bit }
      next_bit = next_bit + 1
    end
    p.area_fx = 1 << fx[name].bit
  end

  -- What each profile always carries, as the wire's mask — and, for a
  -- profile whose recharge has gates, the gated wait itself (std.gates),
  -- built here so that nothing allocates on the tick.
  for id, p in pairs(defs) do
    p.fx = fx_mask(p.effects, p.name or id) | (p.area_fx or 0)
    if p.gates and (p.gates.gates or 1) > 0 then
      local g = {}
      for k, v in pairs(p.gates) do g[k] = v end
      if g.label == nil then g.label = "RELOAD" end
      p.gate = la.lib("std").gates(g)
    end
  end

  for _, id in ipairs(ids) do
    local p, a = defs[id], defs[id].area
    -- A band's strength is in standard units, like a profile's.
    local bands = {}
    for i, b in ipairs(a.bands or {}) do
      bands[i] = { b[1], clamp(b[2] * unit, 1, 255) }
    end
    la.area_policy(id, { effect = p.area_fx, on = a.on or "lit", bands = bands,
                         friendly = a.friendly, self = a.self, credit = a.credit,
                         -- What a bystander's hit carries: the policy's own
                         -- list, never the beam's.
                         effects = fx_mask(a.effects, p.name or id) })
  end

  cfg.max_owned = clamp(cfg.max_owned or 3, LIM.max_owned[1], LIM.max_owned[2])
  return P
end

-- ---- Inventory helpers -------------------------------------------
local function profile_of(idx) return defs[slots[idx].id] end
local function active()        return profile_of(active_idx) end
-- Whose economy the pool follows, and where its clock is kept: the
-- projector in hand's, or with a shared pool always the baseline's.
local function pool_profile()  return shared and defs[0] or active() end
local function pool_slot()     return shared and slots[1] or slots[active_idx] end

local function find_slot(id)
  for i = 1, #slots do
    if slots[i].id == id then return i end
  end
  return nil
end

-- ---- Energy ------------------------------------------------------
-- The energy var is the authority for the ACTIVE projector's pool: a
-- ruleset may write it directly (Outflow's passive drain does).  Its
-- value is banked into the slot on a switch and reloaded from the next.
local function get_energy(vars)     return vars[var.energy] or 0 end
local function set_energy(vars, v)  vars[var.energy] = v end

local function max_energy(vars, p)
  local m = val(vars, p.max_energy, 0)
  if mods.energy ~= 1 and m > 0 then m = math.max(1, math.floor(m * mods.energy + 0.5)) end
  if dimmed and m > 0 then m = math.max(1, m // DIM.energy_div) end
  return m
end

-- A recharge duration, stretched while dimmed.
local function stretch(ms)
  if mods.recharge ~= 1 then ms = math.floor(ms * mods.recharge + 0.5) end
  if dimmed then return ms * DIM.recharge_mul end
  return ms
end

-- The cooldown pushed to Enlight.  Always an integer, never nil: a profile
-- that declares none must still undo a dimmed (or a previous profile's)
-- cooldown when it becomes the one in hand.
local function cooldown_of(vars, p)
  local c = val(vars, p.cooldown_ms, 0)
  if mods.cooldown ~= 1 then c = math.floor(c * mods.cooldown + 0.5) end
  if dimmed then
    c = (c > 0) and (c * DIM.cooldown_mul) or DIM.min_cooldown_ms
  end
  return c
end

-- ================================================================
--   The reload bar.
--
--   The LCD shows the energy cell as a number, and as a filling bar
--   while the pool is empty.  The bar's clock cannot be the moment
--   energy reached zero: with a "refill" recharge the wait starts when
--   the TRIGGER IS RELEASED, so a player holding a dead trigger would
--   watch a bar complete while nothing came back.
--
--   So the projector publishes both halves and owns their timing:
--     reload    — millis at which the recharge clock started, 0 = idle
--     reload_ms — how long this profile's recharge takes, in milliseconds
--   Both are ordinary game vars, which is what lets the display bind to
--   them without the projector reaching into the display layer.
--
--   The bar only ever shows while the pool is EMPTY, so it covers the wait
--   until energy starts coming back — for every recharge mode, that is the
--   recharge_delay_ms idle and nothing more.  A "ramp" projector starts
--   trickling the moment the idle ends, the pool leaves zero and the bar
--   gives way to the number; timing the bar to the whole refill made it
--   vanish a third of the way across.
-- ================================================================
local reload_mark = 0      -- the marker last told to the display

local function reload_total_ms(vars, p)
  return stretch(val(vars, p.recharge_delay_ms, 0))
end

local function publish_reload(vars, started_at, p)
  if var.reload    then vars[var.reload]    = started_at end
  if var.reload_ms then vars[var.reload_ms] = reload_total_ms(vars, p) end
  -- The bar's marker, for a wait with gates: on the bar that times THIS
  -- wait, named by its start var, and on no other.  Nothing shows while
  -- there is no wait.  Told to the display only when it changes.
  if var.reload then
    local mark = (started_at ~= 0 and gate and gate.active()) and gate.mark() or 0
    if mark ~= reload_mark then
      reload_mark = mark
      la.bar_mark(var.reload, mark)
    end
  end
end

-- ================================================================
--   Switching
-- ================================================================
local function activate(vars, idx)
  -- Bank the outgoing pool before leaving: a ruleset may have written the
  -- energy var directly since the last switch.
  if idx ~= active_idx then
    slots[active_idx].energy = get_energy(vars)
  end
  active_idx = idx

  local p = active()
  -- A shared pool stays put: it is never loaded from a slot.
  if not shared then set_energy(vars, slots[idx].energy) end

  -- Optics.  Queued by the verb and applied in the OUTPUT phase, so this
  -- can never reconfigure Enlight mid-measurement.
  -- A profile that names no cycles takes the baseline's, never whatever
  -- Enlight was left holding by the projector before it.
  la.shine_config{ reps = val(vars, p.cycles, nil) or base_value(vars, "cycles"),
                   cooldown_ms = cooldown_of(vars, p) }
  if la.shine_action then la.shine_action(p.shine_action) end
  if var.name then vars[var.name] = p.name or "" end
  -- The energy cell's icon follows the projector in hand.  Published as an
  -- la.icons value into an ordinary var, so the display binding reads it
  -- through a pointer and nothing here reaches into the display layer.
  if var.icon then
    vars[var.icon] = (p.icon and la.icons and la.icons[p.icon]) or ICON_FALLBACK
  end

  ready_at = la.now() + val(vars, p.ready_ms, 0)
  -- Whatever the press was building belonged to the projector that left.
  burst_left, held_beams, charge_from, charge_ready = 0, 0, nil, nil
  lock.target, lock.n, lock.closing, lock.warned = nil, 0, false, nil
  if var.lock   then vars[var.lock]   = 0 end
  if var.charge then vars[var.charge] = 0 end
  -- A shared pool's reload clock is the pool's, and a switch leaves it be.
  if not shared then publish_reload(vars, 0, p) end
end

local function evict_oldest(vars)
  if #slots <= 1 then return end
  local oldest = 2
  for i = 3, #slots do
    if slots[i].acquired_at < slots[oldest].acquired_at then oldest = i end
  end
  evicted_name = defs[slots[oldest].id].name or "?"
  la.show("LOST " .. evicted_name, 2000)      -- it made room for the new one
  P.drop(vars, slots[oldest].id)
end

-- ================================================================
--   Public inventory API
-- ================================================================
function P.owns(id) return find_slot(id) ~= nil end
function P.active_id() return slots[active_idx].id end
function P.active_profile() return active() end
function P.owned_count() return #slots end

-- quiet = true skips the ProjectorChange cue, for a caller that plays its
-- own (a BONUS pickup has a sound of its own).
function P.select(vars, id, quiet)
  local idx = find_slot(id)
  if not idx then return false end
  if idx ~= active_idx then
    activate(vars, idx)
    if not quiet then la.ui("ProjectorChange") end
  end
  return true
end

-- Add at full energy, or refill if already held.  Keeps acquired_at on a
-- re-grant so restocking cannot be used to dodge eviction.  With a shared
-- pool there is nothing of its own to fill: the projector is added and
-- the pool is left as it is.
function P.give(vars, id)
  if not defs[id] then return false end
  if id == 0 then return true end                 -- always held already

  local held = find_slot(id)
  if held and shared then return true end
  -- A limited pickup counts from the moment it is given, and a re-grant
  -- restocks it.
  local p   = defs[id]
  local now = la.now()
  local uses = val(vars, p.uses, 0)
  local life = val(vars, p.lifetime_ms, 0)
  local function limit(s)
    s.uses       = (uses > 0) and uses or nil
    s.expires_at = (life > 0) and (now + life) or nil
    s.drop_at    = nil
  end
  if held then
    slots[held].energy = max_energy(vars, p)
    limit(slots[held])
    if held == active_idx then set_energy(vars, slots[held].energy) end
    return true
  end

  if #slots - 1 >= cfg.max_owned then evict_oldest(vars) end
  local s = { id = id, energy = max_energy(vars, p), acquired_at = now, ramp_at = now }
  limit(s)
  slots[#slots + 1] = s
  return true
end

function P.grant(vars, id, quiet)
  if not P.give(vars, id) then return false end
  return P.select(vars, id, quiet)
end

-- ================================================================
--   DIM — the MALUS that weakens every projector the player holds.
--
--   set_dim(vars, true): half the pool (current energy clamped down to
--   it), twice the recharge, twice the cooldown.  set_dim(vars, false) lifts it; the pool then
--   refills by the ordinary recharge, not at once.  reset() lifts it too.
--   (A profile with no cooldown gets DIM.min_cooldown_ms instead.)
--   A ruleset lifts it when the player goes out of the game.
-- ================================================================
local function dim_apply(vars)
  local on = dim_hold or la.now() < dim_until
  if on == dimmed then return end
  dimmed = on
  local p = active()
  -- Re-push the optics for the projector in hand.
  la.shine_config{ cooldown_ms = cooldown_of(vars, p) }
  if on and shared then
    local m = max_energy(vars, defs[0])
    if get_energy(vars) > m then set_energy(vars, m) end
  elseif on then
    slots[active_idx].energy = get_energy(vars)
    for i = 1, #slots do
      local m = max_energy(vars, profile_of(i))
      if slots[i].energy > m then slots[i].energy = m end
    end
    set_energy(vars, slots[active_idx].energy)
  end
  publish_reload(vars, 0, pool_profile())
end

function P.set_dim(vars, on)
  dim_hold = on and true or false
  if not dim_hold then dim_until = 0 end    -- lifting it lifts a beam's too
  dim_apply(vars)
end

function P.dimmed() return dimmed end

-- ================================================================
--   set_mods(vars, { energy = 1, recharge = 1, cooldown = 1, strength = 1 })
--
--   What a player's CLASS changes about every projector they hold
--   (std.classes sets it): a bigger or smaller pool, a slower or faster
--   recharge, a longer or shorter wait between beams, heavier or lighter
--   beams.  Multipliers; a missing one is 1.  They stack with DIM, they
--   survive going out (a class is what the player is), and reset()
--   clears them.
-- ================================================================
function P.set_mods(vars, m)
  m = m or {}
  mods.energy   = m.energy   or 1
  mods.recharge = m.recharge or 1
  mods.cooldown = m.cooldown or 1
  mods.strength = m.strength or 1
  la.shine_config{ cooldown_ms = cooldown_of(vars, active()) }
  -- A smaller pool cannot hold what a bigger one did.
  slots[active_idx].energy = get_energy(vars)
  for i = 1, #slots do
    local top = max_energy(vars, shared and defs[0] or profile_of(i))
    if slots[i].energy > top then slots[i].energy = top end
  end
  if shared then
    local top = max_energy(vars, defs[0])
    if get_energy(vars) > top then set_energy(vars, top) end
  else
    set_energy(vars, slots[active_idx].energy)
  end
  publish_reload(vars, 0, pool_profile())
end

-- The pool of the projector in hand — or the shared pool — as it stands
-- (dimmed or not).
function P.max_energy(vars) return max_energy(vars, pool_profile()) end

-- ================================================================
--   BONUS totem options.
--
--   The list the DM cycles with O on a BONUS totem, for a game's
--   totem_slots entry:
--
--     { role = "BONUS", min = 0, max = 16, options = proj.bonus_options() }
--
--   "LIFE", then the standard catalogue in id order, then the game's own
--   profiles (id > 0) unless a profile says `bonus = false` — a practice
--   or role projector is not something a totem should hand out.  (A
--   ruleset whose pool is the player's life offers them too, with
--   define{ shared_pool = true }: picking one up then swaps no lives.)
--
--   Labels are cut to the menu's 8 characters; bonus_id() maps a label
--   back to its projector, so a long custom name still resolves.
--   Call after define().
-- ================================================================
function P.bonus_options()
  local list = { "LIFE" }
  bonus_ids = {}
  local ids = {}
  for id, p in pairs(defs) do
    if id ~= 0 and p.bonus ~= false then ids[#ids + 1] = id end
  end
  table.sort(ids)
  for _, id in ipairs(ids) do
    local label = string.sub(tostring(defs[id].name or id), 1, 8)
    if label ~= "LIFE" and not bonus_ids[label] then
      bonus_ids[label] = id
      list[#list + 1] = label
    end
  end
  return list
end

-- Projector id behind a BONUS option label, or nil (e.g. for "LIFE").
function P.bonus_id(label) return bonus_ids[label] end

function P.drop(vars, id)
  local idx = find_slot(id)
  if not idx or idx == 1 then return false end    -- slot 1 is structural
  local was_active_slot = (idx == active_idx)

  table.remove(slots, idx)
  if was_active_slot then
    -- Point at the baseline BEFORE activating, so activate() skips its
    -- usual "bank the outgoing pool" step: that slot no longer exists and
    -- its energy went with it.
    active_idx = 1
    activate(vars, 1)
    la.ui("ProjectorChange")
  elseif idx < active_idx then
    active_idx = active_idx - 1                    -- the removal shifted us down
  end
  return true
end

-- ================================================================
--   strip(vars) — out of the game.
--
--   Everything a BONUS can hand out — the standard catalogue and a game's
--   own profiles, unless declared bonus = false — is dropped, DIM is
--   lifted, and the baseline comes back in hand with its own banked pool.
--   Role and practice projectors (bonus = false: Virus's VIRUS, a stand's
--   TRIAL) are kept: they are what the player IS, not what they picked up.
--   Silent: going out has its own cue.  Call it from the transition that
--   takes a player out, BEFORE a respawn writes the pool — the pool it
--   writes must be the baseline's, not the one of a projector about to go.
-- ================================================================
function P.strip(vars)
  P.set_dim(vars, false)
  mute_until, expose_until, trace.until_ms = 0, 0, 0
  kind_on, fx_on = kind_on & kind_sticky, fx_on & fx_sticky
  -- Whatever a press was building is gone with the player.
  lock.target, lock.n, lock.closing, lock.warned = nil, 0, false, nil
  locked_until = 0
  for k in pairs(seen) do seen[k] = nil end
  burst_left, held_beams, charge_from, charge_ready = 0, 0, nil, nil
  if var.lock   then vars[var.lock]   = 0 end
  if var.charge then vars[var.charge] = 0 end
  local in_hand = slots[active_idx].id
  slots[active_idx].energy = get_energy(vars)      -- bank before reshuffling
  for i = #slots, 2, -1 do
    local p = defs[slots[i].id]
    if p and p.bonus ~= false then table.remove(slots, i) end
  end
  local idx = find_slot(in_hand)
  if idx then
    active_idx = idx                               -- a kept role projector
  else
    active_idx = 1                                 -- skip the banking step:
    activate(vars, 1)                              --   that slot is gone
  end
end

local function cycle(vars, dir)
  if #slots <= 1 then return end
  activate(vars, ((active_idx - 1 + dir) % #slots) + 1)
  la.show(tostring(active().name or ""), 1000)   -- which one is in hand now
  la.ui("ProjectorChange")
end

function P.next(vars) cycle(vars,  1) end
function P.prev(vars) cycle(vars, -1) end

-- The switch keys, read once per tick.  A switch happens on the key's
-- RELEASE, and only if the other key was never down with it.
local function tick_switch(vars)
  local l, r = la.key_state("<"), la.key_state(">")
  local l_down = (l == "pressed" or l == "held")
  local r_down = (r == "pressed" or r == "held")
  if l_down and r_down then chord = true end
  if not chord then
    if l == "released" or l == "released_held" then cycle(vars, -1)
    elseif r == "released" or r == "released_held" then cycle(vars, 1) end
  elseif not l_down and not r_down
         and l ~= "released" and l ~= "released_held"
         and r ~= "released" and r ~= "released_held" then
    chord = false
  end
end

-- The name of a projector an eviction just dropped, once.  Reading it
-- clears it.  (The tray already says so; this is for a ruleset that wants
-- to do more.)
function P.consume_evicted()
  local n = evicted_name
  evicted_name = nil
  return n
end

-- ================================================================
--   reset(vars) — from on_begin
-- ================================================================
function P.reset(vars)
  if not cfg then P.define{} end
  local now = la.now()
  slots = { { id = 0, energy = 0, acquired_at = now, ramp_at = now } }
  active_idx  = 1
  was_active  = false
  release_at  = 0
  wait_from   = 0
  if gate then gate.stop() end
  gate_for    = nil
  awaiting_release = false
  evicted_name   = nil
  dimmed         = false
  shot_units, beam_mul = nil, 1
  dim_hold, dim_until = false, 0
  mods.energy, mods.recharge, mods.cooldown, mods.strength = 1, 1, 1, 1
  -- -1: whatever marker the display was left with, the next publish
  -- tells it again.
  locked_until, reload_mark = 0, -1
  for k in pairs(seen) do seen[k] = nil end
  mute_until, expose_until, trace.until_ms = 0, 0, 0
  lock.deaf      = false
  role_tag       = 0
  kind_on, kind_sticky = 0, 0
  fx_on,   fx_sticky   = 0, 0

  slots[1].energy = max_energy(vars, defs[0])
  activate(vars, 1)
  set_energy(vars, slots[1].energy)
  publish_reload(vars, 0, defs[0])   -- activate() leaves a shared pool's alone
  if var.spent then vars[var.spent] = 0 end
  ready_at = 0                       -- no deploy delay on the opening beam
end

-- ================================================================
--   result() — interpret the measurement
--
--   Returns the target's player id, plus the estimated distance, or nil
--   when there is nothing to act on.  The second return is the reason
--   (the measurement's status), so a ruleset can tell a miss from a
--   totem.
--
--   Nothing gates on distance.  A profile's range_m is a label for now:
--   the estimate is not trustworthy enough to refuse a hit on (see
--   docs/projector.md §2), and reach is what a profile's cycles buy.
--
--   A profile with a `lock` does not report each beam: it counts the
--   beams that land on one target in a row and reports once, when the
--   press ends (see "The lock" below).
-- ================================================================

-- What a beam of the projector in hand weighs, in the game's currency.
local function strength_of(vars, p)
  return clamp(math.floor(val(vars, p.strength, 1) * unit * mods.strength + 0.5), 0, 255)
end

-- A beam reached a player: on_lit_refund gives energy back.
local function lit(vars, p)
  local back = val(vars, p.on_lit_refund, 0)
  if back > 0 then
    local m = max_energy(vars, pool_profile())
    local e = get_energy(vars) + back
    set_energy(vars, (m > 0 and e > m) and m or e)
  end
end

-- ================================================================
--   The lock ("tunneling").
--
--     lock = { grace_ms = 300,     -- the lock survives this long unlit
--              curve    = "fib",   -- "linear", "square", "fib", or a table
--              max      = 8,       -- cap, in standard units
--              deliver  = "release" }   -- or "beam"
--
--   While the trigger is held, every beam that lands on the SAME target
--   is one step.  Step n adds curve(n) standard units (linear 1,1,1…;
--   square 1,3,5…; fib 1,1,2,3,5…; a table lists them, its last value
--   repeating), times the profile's strength, and the total is capped at
--   `max`.  With deliver = "release" nothing is sent until the press ends:
--   one LIT, weighing the total.  With "beam" each step is sent as it
--   lands, weighing that step alone — which needs ignore_immunity to get
--   past a ruleset's immunity window.
--
--   The lock is lost, with what it had built, when a beam lands on
--   someone else or when no beam has landed for grace_ms.  It is
--   delivered when the trigger is released and when the pool runs dry.
--   Enlight shines in bursts, so a grace shorter than one burst and its
--   cooldown would lose the lock between two beams that both landed: it is
--   never taken as less than that.
--
--   The target is warned.  From step `warn_at` (default 1, the first
--   beam that lands) the projector sends MSG.LOCK to the target alone, and
--   again every second while the lock holds; the target's device
--   acknowledges it, and one that was not acknowledged is sent again
--   after 300 ms.  The target's own projector library shows LOCKED and
--   plays the Locked cue for as long as the warnings keep coming (see
--   tick_inbox).  It never says who.
--   warn_at = 0 is a lock that does not warn — one that restores, say.
--
--   vars.lock, when the game declares it, shows the step reached.
-- ================================================================
local function curve_step(c, n)
  if type(c) == "table" then return c[n] or c[#c] or 0 end
  if c == "fib" then
    local a, b = 1, 1
    for _ = 3, n do a, b = b, a + b end
    return b
  end
  if c == "square" then return 2 * n - 1 end
  return 1
end

local function lock_units(vars, p, from, to)
  local sum = 0
  for i = from, to do sum = sum + curve_step(p.lock.curve, i) end
  local cap = clamp(p.lock.max or LIM.strength[2], 1, 25) * unit
  if cap > 255 then cap = 255 end
  return clamp(math.floor(sum * val(vars, p.strength, 1) * unit * mods.strength + 0.5), 0, cap)
end

local function lock_clear(vars)
  lock.target, lock.n, lock.warned, lock.acked = nil, 0, nil, false
  if var.lock then vars[var.lock] = 0 end
end

function P.result(vars)
  local p = active()
  local status, id, metres = la.shine_result()
  shot_units = nil

  if not p.lock then
    if status ~= "player" then return nil, status end
    lit(vars, p)
    if beam_mul ~= 1 then
      shot_units = clamp(math.floor(strength_of(vars, p) * beam_mul + 0.5), 0, 255)
    end
    return id, metres
  end

  local now  = la.now()
  local beam = (p.lock.deliver == "beam")
  if status == "player" and not lock.deaf then
    lit(vars, p)
    if lock.target ~= id then                       -- someone else: start over
      lock.target, lock.n, lock.warned, lock.acked = id, 0, nil, false
    end
    lock.n    = lock.n + 1
    lock.last = now
    if var.lock then vars[var.lock] = lock.n end
    local warn_at = p.lock.warn_at or 1
    if warn_at > 0 and lock.n >= warn_at
       and (not lock.warned
            or now - lock.warned >= (lock.acked and LOCK_WARN_EVERY_MS or LOCK_WARN_RETRY_MS)) then
      lock.warned, lock.acked = now, false
      la.send(id, la.msg.LOCK)
    end
    if beam then
      shot_units = lock_units(vars, p, lock.n, lock.n)
      return id, metres
    end
  end

  if lock.target then
    local grace = val(vars, p.lock.grace_ms, 0)
    local floor = la.shine_ms() + cooldown_of(vars, p)
    if grace < floor then grace = floor end
    if now - lock.last > grace then lock_clear(vars) end
  end

  if lock.closing then
    lock.closing, lock.deaf = false, true
    local target, n = lock.target, lock.n
    lock_clear(vars)
    if target and n > 0 and not beam then
      shot_units = lock_units(vars, p, 1, n)
      return target, 0
    end
  end
  return nil, status
end

-- ================================================================
--   payload() — what a hit carries on the wire
--
--     la.send(target, MSG.LIT, proj.payload(vars))
--
--   [strength, kind, role tag, rssi gate, effects lo, effects hi, spare,
--   spare].  Strength is in the game's currency (standard units times
--   game_unit).  The gate travels as a positive magnitude because payload
--   bytes are unsigned: 50 means -50 dBm.  The two spare bytes are the
--   ruleset's: proj.payload(vars, a, b).
--
--   No projector id travels: a receiver acts on what a beam IS (its kind,
--   its effects), never on which projector produced it.
-- ================================================================
function P.payload(vars, spare1, spare2)
  local p = active()
  local gate = val(vars, p.rssi_min, 0)
  local e = (p.fx or 0) | fx_on
  -- shot_units: the hit result() just returned was charged, or is a
  -- lock's total — not what one plain beam of this profile weighs.
  return shot_units or strength_of(vars, p),
         val(vars, p.kind, 0) | kind_on,
         role_tag,
         (gate < 0) and -gate or 0,
         e & 0xFF, e >> 8,
         spare1 or 0, spare2 or 0
end

-- ================================================================
--   Role tag, kind and effects at run time.
--
--   set_role_tag(n)            who is shining: the player's class or role
--   set_kind(mask, on, sticky) add or remove kind bits for what is in hand
--   set_effect(vars, name, on, sticky)   the same for one effect
--
--   A power-up, a rule or a key may all call these.  What was switched on
--   is lost at strip() — going out — unless it was set sticky, which is
--   for what the player IS rather than what they picked up.  reset()
--   clears everything.
-- ================================================================
function P.set_role_tag(n) role_tag = clamp(n or 0, 0, 255) end
function P.role_tag() return role_tag end

function P.set_kind(mask, on, sticky)
  mask = (mask or 0) & 0xFF
  if on == false then
    kind_on, kind_sticky = kind_on & ~mask, kind_sticky & ~mask
  else
    kind_on = kind_on | mask
    if sticky then kind_sticky = kind_sticky | mask end
  end
end

function P.set_effect(vars, name, on, sticky)
  local e = fx[name]
  if not e then error("projector: unknown effect '" .. tostring(name) .. "'", 0) end
  local mask = 1 << e.bit
  if on == false then
    fx_on, fx_sticky = fx_on & ~mask, fx_sticky & ~mask
  else
    fx_on = fx_on | mask
    if sticky then fx_sticky = fx_sticky | mask end
  end
end

-- Does the next beam carry this effect (the profile's or switched on)?
function P.has_effect(name)
  local e = fx[name]
  if not e then return false end
  return (((active().fx or 0) | fx_on) & (1 << e.bit)) ~= 0
end

-- The effect a mask bit stands for, for a receiver: its entry, or nil.
function P.effect(name) return fx[name] end

-- ================================================================
--   The receiving side.
--
--   A LIT handler reads a beam through these, so a ruleset never spells
--   out a payload offset:
--
--     proj.beam_has(pkt, "restore")   does the beam carry this effect
--     proj.kind(pkt), proj.role(pkt)  the beam's kind mask and role tag
--     proj.shiner(pkt)                who shone, for the tray: a short
--                                     name, or "?" for a silent beam
--     proj.weight(vars, pkt)          what it weighs HERE, in game units:
--                                     its strength, more while exposed
--     proj.landed(vars, pkt)          the hit landed and the player is
--                                     still in play: apply dim, mute,
--                                     expose, siphon, beacon
--
--   std.lit_target{ proj = proj, ... } does all of it; a ruleset with its
--   own LIT handler calls weight() and landed() itself.  restore,
--   ignore_immunity and silent are read by the handler (they change what
--   a hit IS), and splash by the firmware.
--
--   The timed effects are run by tick(), so they only pass while the
--   player is in play, and strip() ends them all: going out is a clean
--   slate.
-- ================================================================
local function pkt_fx(pkt)
  if not pkt.len or pkt.len < 6 then return 0 end
  return pkt:byte(5) | (pkt:byte(6) << 8)
end

function P.beam_has(pkt, name)
  local e = fx[name]
  return e ~= nil and (pkt_fx(pkt) & (1 << e.bit)) ~= 0
end

function P.kind(pkt) return (pkt.len and pkt.len >= 2) and pkt:byte(2) or 0 end
function P.role(pkt) return (pkt.len and pkt.len >= 3) and pkt:byte(3) or 0 end

function P.shiner(pkt)
  if P.beam_has(pkt, "silent") then return "?" end
  return la.player_short(pkt.sender)
end

function P.weight(vars, pkt)
  local w = (pkt.len and pkt.len >= 1) and pkt:byte(1) or 1
  if la.now() < expose_until then
    expose_until = 0                       -- one beam, then it is used up
    w = math.floor(w * fx.expose.mul + 0.5)
  end
  return w
end

function P.exposed() return la.now() < expose_until end
function P.muted()   return la.now() < mute_until end
function P.traced()  return la.now() < trace.until_ms end

-- `spared`, optional: a mask of effects that do not take on this player
-- (proj.effects_mask{ "dim", "mute" } — a class's own).
function P.effects_mask(list) return fx_mask(list, "effects_mask") end

function P.landed(vars, pkt, spared)
  local m = pkt_fx(pkt) & ~(spared or 0)
  if m == 0 then return end
  local now = la.now()
  if m & (1 << fx.dim.bit) ~= 0 then
    dim_until = now + fx.dim.ms
    dim_apply(vars)
    la.show("DIMMED", 2000)
    la.ui("MalusDim")
  end
  if m & (1 << fx.mute.bit) ~= 0 then
    mute_until = now + fx.mute.ms
    la.show("MUTED", 2000)
    la.ui("Malus")
  end
  if m & (1 << fx.expose.bit) ~= 0 then
    expose_until = now + fx.expose.ms
    la.show("EXPOSED", 2000)
    la.ui("Malus")
  end
  if m & (1 << fx.siphon.bit) ~= 0 then
    local e = get_energy(vars) - fx.siphon.amount
    set_energy(vars, e > 0 and e or 0)
  end
  -- Nothing is shown for a beacon: being traced is not something the
  -- traced player is told.
  if m & (1 << fx.beacon.bit) ~= 0 then
    trace.until_ms = now + fx.beacon.ms
    trace.next_ms  = now
    trace.shiner   = pkt.sender
    trace.team     = pkt.team or 0xFF
  end
end

-- A received MSG.TRACE, for the ruleset that tracks with it:
--   [MSG.TRACE] = function(vars, pkt)
--     local who, rssi = proj.trace(pkt)      -- nil when not meant for us
-- `who` is the traced player (the sender), and rssi how near they are.
local function trace_for_me(shiner, team, to)
  if to == TRACE_TO.shiner and shiner ~= la.my_id() then return false end
  if to == TRACE_TO.team and shiner ~= la.my_id()
     and (team == 0xFF or team ~= la.my_team()) then return false end
  return true
end

function P.trace(pkt)
  if not pkt.len or pkt.len < 3 then return nil end
  if not trace_for_me(pkt:byte(1), pkt:byte(2), pkt:byte(3)) then return nil end
  return pkt.sender, pkt.rssi
end

-- ================================================================
--   The library's own messages.
--
--   MSG.LOCK and MSG.TRACE are the projector library's, not a ruleset's:
--   a ruleset should not have to declare a handler for every projector
--   that might turn up on the field.  The firmware keeps the ones no
--   ruleset handler took (la.inbox) and tick() reads them here — so they
--   are acted on exactly while the player is in play, because that is
--   when a ruleset ticks the projector.  A ruleset that does declare a
--   handler for one of them takes it instead: the handler wins.
--
--     proj.locked()        is someone locking on to this player now
--     proj.trace_of(id)    rssi, age_ms of the last trace heard from that
--                          player and meant for us; nil if none
--     proj.nearest_trace(max_age_ms)   id, rssi of the strongest one
-- ================================================================
function P.locked() return la.now() < locked_until end

function P.trace_of(id)
  local t = seen[id]
  if not t then return nil end
  return t[1], la.now() - t[2]
end

function P.nearest_trace(max_age_ms)
  local now, best, best_rssi = la.now(), nil, nil
  for id, t in pairs(seen) do
    if now - t[2] <= (max_age_ms or 3000) and (not best_rssi or t[1] > best_rssi) then
      best, best_rssi = id, t[1]
    end
  end
  return best, best_rssi
end

local function tick_inbox(now)
  local msg, sender, team, rssi, b1, b2, b3 = la.inbox()
  while msg do
    if msg == la.msg.LOCK then
      -- Sent to this player alone, and already acknowledged by the runner.
      -- A warning sent again because its acknowledgement was lost must not
      -- play the cue twice: one pulse per interval.
      if now >= locked_until - (LOCK_WARN_HOLD_MS - LOCK_WARN_RETRY_MS * 2) then
        la.show("LOCKED", LOCK_WARN_HOLD_MS)
        la.ui("Locked")
      end
      locked_until = now + LOCK_WARN_HOLD_MS
    elseif msg == la.msg.LOCK + 1 then
      -- The target's device says our warning arrived.
      if sender == lock.target then lock.acked = true end
    elseif msg == la.msg.TRACE then
      if trace_for_me(b1, b2, b3) then
        local t = seen[sender]
        if t then t[1], t[2] = rssi, now else seen[sender] = { rssi, now } end
      end
    end
    msg, sender, team, rssi, b1, b2, b3 = la.inbox()
  end
end

local function tick_effects(vars, now)
  tick_inbox(now)
  if dimmed and not dim_hold and now >= dim_until then dim_apply(vars) end
  if now < trace.until_ms and now >= trace.next_ms then
    trace.next_ms = now + fx.beacon.every_ms
    la.broadcast(la.msg.TRACE, trace.shiner, trace.team, TRACE_TO[fx.beacon.to])
  end
end

-- ================================================================
--   Recharge
-- ================================================================
local function tick_recharge(vars, p, now)
  local mode = p.recharge or "refill"
  if mode == "none" then return end

  local max = max_energy(vars, p)
  local e   = get_energy(vars)
  if max <= 0 or e >= max then
    if gate then gate.stop() end
    publish_reload(vars, 0, p)
    return
  end

  local delay_ms = stretch(val(vars, p.recharge_delay_ms, 0))

  -- ---- Gates: the reload mini-game ---------------------------------
  -- A profile with `gates` lets the player shorten this wait by pressing
  -- the secondary trigger at the right moments (std.gates has the rules).
  -- The nominal time is the wait itself — recharge_delay_ms, stretched
  -- while dimmed — and with no gates (the default) none of this runs: the
  -- wait is the release plus the delay, exactly as before.
  --
  -- A gated wait belongs to ONE release: a beam fired meanwhile starts a
  -- new one from its own release.
  if p.gate and delay_ms > 0 then
    if gate ~= p.gate then
      if gate then gate.stop() end
      gate, gate_for = p.gate, nil
    end
    if gate_for ~= release_at then
      gate_for = release_at
      gate.start(release_at, delay_ms)
    end
    if gate.active() then
      gate.tick(now)
      -- Over: put the start where a plain wait of this length would be.
      wait_from = gate.active() and gate.from() or (now - delay_ms)
    end
  else
    wait_from = release_at
  end

  if (now - wait_from) < delay_ms then
    -- Waiting out the idle: the clock started at the release, which is
    -- exactly what the bar must show.
    publish_reload(vars, wait_from, p)
    return
  end

  if mode == "refill" then
    set_energy(vars, max)
    publish_reload(vars, 0, p)
    return
  end

  -- ramp: one unit every recharge_step_ms when the profile names one,
  -- otherwise every recharge_ms / max, stepped in integers.
  --
  -- Standard policy for a ramp: the trickle STARTS when the idle ends, the
  -- first unit arriving at that instant, so an empty pool fills at an even
  -- rate from then on — never before.  ramp_at is otherwise left at the
  -- last beam, and counting from there would hand back everything "earned"
  -- during the idle in one lump the moment it ends.
  local step_ms
  if p.recharge_step_ms ~= nil then
    step_ms = stretch(val(vars, p.recharge_step_ms, 1))
  else
    local total_ms = stretch(val(vars, p.recharge_ms, 0))
    step_ms = (max > 0) and (total_ms // max) or 0
  end
  if step_ms < 1 then step_ms = 1 end
  local s = pool_slot()
  local ramp_from = wait_from + delay_ms
  if s.ramp_at < ramp_from then s.ramp_at = ramp_from end
  while now >= s.ramp_at and get_energy(vars) < max do
    set_energy(vars, get_energy(vars) + 1)
    s.ramp_at = s.ramp_at + step_ms
  end
  if get_energy(vars) >= max then
    s.ramp_at = now
    publish_reload(vars, 0, p)
  else
    publish_reload(vars, wait_from, p)
  end
end

-- ================================================================
--   Fire modes, charge, spin-up, overheat, limited use, self cost.
--
--     mode = "auto"            fires while the trigger is held (default)
--     mode = "semi"            one beam per press
--     mode = "burst", burst = 3    that many beams per press
--
--     charge = { ms = 1500, max = 3 }
--         Hold to charge, release to fire ONE beam: its strength grows
--         linearly from 1x to `max`x over `ms`.  vars.charge, when the
--         game declares it, shows 0..100.
--
--     spinup = { to_ms = 50, beams = 8 }
--         The wait between beams goes from the profile's cooldown_ms to
--         to_ms over that many beams of one press, and back on release.
--
--     overheat_ms      emptying the pool locks the projector for this long
--     uses             dropped after that many beams          } a pickup,
--     lifetime_ms      dropped that long after it was given   } never the baseline
--     on_lit_refund    energy back for each beam that reaches a player
--     self_cost = { var = "lives", amount = proj.units(1), floor = 1 }
--         Each beam also costs `amount` of any game var, and is refused
--         if that would take the var below `floor` (default 0).
-- ================================================================
local function charge_mul(vars, p, now)
  local c = p.charge
  local ms  = val(vars, c.ms, 1000)
  local top = val(vars, c.max, 2)
  local held = now - (charge_from or now)
  if ms <= 0 or held >= ms then return top end
  return 1 + (top - 1) * held / ms
end

local function spin_cooldown(vars, p, beams)
  local from, sp = cooldown_of(vars, p), p.spinup
  local to = val(vars, sp.to_ms, from)
  if dimmed then to = to * DIM.cooldown_mul end
  local n = val(vars, sp.beams, 1)
  if n < 1 or beams >= n then return to end
  return from + (to - from) * beams // n
end

local function can_pay(vars, p)
  local sc = p.self_cost
  if not sc then return true end
  return (vars[sc.var] or 0) - val(vars, sc.amount, 1) >= val(vars, sc.floor, 0)
end

-- What the press ends: the lock closes, the spin-up winds down.
local function end_press(vars, p)
  if p.lock then lock.closing = true end
  if p.spinup and held_beams > 0 then
    la.shine_config{ cooldown_ms = cooldown_of(vars, p) }
  end
  held_beams = 0
end

-- Limited use and limited time: a pickup that has run out is dropped.  One
-- that fired its last beam is kept (and refuses to fire) until that beam
-- has been measured — the hit must go out as this projector's, not the
-- baseline's.
local function expire(vars, now)
  for i = #slots, 2, -1 do
    local s = slots[i]
    if (s.drop_at and now >= s.drop_at)
       or (s.expires_at and now >= s.expires_at) then
      P.drop(vars, s.id)
    end
  end
end

-- ================================================================
--   tick(vars) — the whole trigger-to-beam path, once per cycle
--
--   Energy is spent ONLY on a run Enlight actually accepted.  la.shine()
--   returns false while a burst is still in flight or cooling down, and
--   the short-circuit is what keeps one trigger pull costing one beam
--   rather than one per tick.
--
--   Returns true while the trigger is down, for a ruleset that cares.
-- ================================================================
function P.tick(vars)
  local now    = la.now()
  tick_effects(vars, now)
  expire(vars, now)
  if switch_keys and #slots > 1 then tick_switch(vars) end
  local p      = active()
  local s      = slots[active_idx]
  local active_trigger = la.trigger_down(1)
  local pressed  = active_trigger and not was_active
  local released = was_active and not active_trigger

  -- What this tick would fire, by mode.
  local want
  if p.charge then
    if pressed then charge_from, charge_ready = now, nil end
    if released and charge_from then
      charge_ready = charge_mul(vars, p, now)
      charge_from  = nil
    end
    if var.charge then
      vars[var.charge] = charge_from
        and math.floor((charge_mul(vars, p, now) - 1) * 100 /
                       math.max(val(vars, p.charge.max, 2) - 1, 0.001)) or 0
    end
    want = charge_ready ~= nil
  elseif (p.mode or "auto") == "auto" then
    want = active_trigger
  else
    if pressed then burst_left = (p.mode == "burst") and val(vars, p.burst, 3) or 1 end
    want = burst_left > 0
  end

  local cost = val(vars, p.cost, 1)
  if want
     and now >= ready_at
     and now >= mute_until                  -- a `mute` beam landed
     and now >= (s.hot_until or 0)         -- overheated: this projector only
     and not s.drop_at
     and get_energy(vars) >= cost
     and can_pay(vars, p)
     and la.shine() then
    set_energy(vars, get_energy(vars) - cost)
    if var.spent then vars[var.spent] = (vars[var.spent] or 0) + cost end
    local sc = p.self_cost
    if sc then vars[sc.var] = (vars[sc.var] or 0) - val(vars, sc.amount, 1) end
    la.ui_enlight(la.shine_ms())
    pool_slot().ramp_at = now
    lock.deaf = false
    beam_mul  = charge_ready or 1
    charge_ready = nil
    if burst_left > 0 then burst_left = burst_left - 1 end
    -- The recharge wait starts at the release that follows a beam.  A beam
    -- fired with the trigger already up (a charged one, the tail of a
    -- burst) has had its release.
    if active_trigger then awaiting_release = true
    else release_at, awaiting_release = now, false end

    held_beams = held_beams + 1
    if p.spinup then
      la.shine_config{ cooldown_ms = spin_cooldown(vars, p, held_beams) }
    end
    if s.uses then
      s.uses = s.uses - 1
      if s.uses <= 0 then s.drop_at = now + la.shine_ms() + cooldown_of(vars, p) + 20 end
    end
    if get_energy(vars) < cost then
      local hot = val(vars, p.overheat_ms, 0)
      if hot > 0 then s.hot_until = now + stretch(hot) end
    end
  end

  -- A lock whose pool has run dry delivers what it built.
  if p.lock and active_trigger and lock.target and get_energy(vars) < cost then
    lock.closing = true
  end
  if released then end_press(vars, p) end

  -- The wait is anchored by the release that FOLLOWS a beam.  A press that
  -- spends nothing — an empty pool — neither re-anchors it nor holds it
  -- back: once started, the wait runs to completion, so leaning on a dead
  -- trigger still gets the energy back on time.  Only a press that actually
  -- fires restarts the clock, and it does so from its own release.
  if released and awaiting_release then
    release_at       = now
    awaiting_release = false
  end
  was_active = active_trigger

  -- Runs whatever the trigger is doing.  The one thing that stops it is a
  -- beam waiting for its release, because until then the wait has not begun.
  if not awaiting_release then tick_recharge(vars, pool_profile(), now) end

  return active_trigger
end

return P
