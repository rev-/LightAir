#include <LightAir.h>
#include "TotemRoleIds.h"
#include "CPTotemPolicy.h"
#include "../config.h"

// ================================================================
// CPTotem — generalised "point-giving" totem runner.
//
// A single class that implements CP, BASE, BONUS, MALUS, and FLAG
// roles through a seven-state machine fully parameterised by the
// activation payload.  Role-specific behaviour (UI events, beacon
// message type, enemy-team filtering, silence in INACTIVE) is
// selected via the new payload bytes [12..15] and mode bits 17–18.
//
// See CPTotemPolicy.h for the full protocol documentation.
// ================================================================

using RadioMsg::MSG_CP_BEACON;
using RadioMsg::MSG_CP_SCORE;

static constexpr uint32_t CP_BEACON_INTERVAL_MS   = 2000;
static constexpr uint32_t CP_DEFAULT_COUNTDOWN_MS = 10000;
static constexpr uint32_t CP_DEFAULT_COOLDOWN_MS  = 30000;
static constexpr uint32_t CP_DEFAULT_PRIME_MS     =  5000;
static constexpr uint8_t  CP_NO_PLAYER            = 0;
static constexpr uint8_t  CP_NO_TEAM              = 0xFF;
static constexpr uint8_t  CP_NO_ROLE              = 0;
static constexpr uint8_t  CP_SECS_NA              = 0xFF;
static constexpr uint8_t  CP_PRESENCE_MAX         = 16;


class CPTotem : public LightAir_TotemRunner {

    // ── Configuration (set at activation; never changed during a game) ──
    uint8_t  _roleId;           // from payload[0]; drives UI event selection
    uint32_t _mode;
    uint32_t _countdownMs;
    uint32_t _cooldownMs;
    uint32_t _suspendMs;
    uint32_t _primeMs;
    uint8_t  _beaconMsgType;    // message type for beacon broadcasts
    uint8_t  _maxPoints;        // 0 = unlimited; INACTIVE after this many awards
    uint32_t _beaconIntervalMs; // window / broadcast period
    uint8_t  _totemTeam;        // own team; 0xFF = teamless

    // ── Mutable game state ───────────────────────────────────────────
    CPState  _state;
    uint16_t _context;
    uint8_t  _pointsAwarded;    // counts awardScore() calls; reset on activation

    // ── Current association ──────────────────────────────────────────
    uint8_t  _assocPlayer;
    uint8_t  _assocTeam;
    uint8_t  _assocRole;

    // ── Pre-contest snapshot (restored on RESOLUTION_PREV_WINS) ──────
    uint8_t  _prevPlayer;
    uint8_t  _prevTeam;
    uint8_t  _prevRole;

    // ── Timers ───────────────────────────────────────────────────────
    uint32_t _stateEnteredAt;
    uint32_t _accumulatedMs;
    uint32_t _windowStart;

    // ── Presence collection (reset each window) ──────────────────────
    struct PresenceEntry { uint8_t senderId, team, role; };
    PresenceEntry _presence[CP_PRESENCE_MAX];
    uint8_t       _presenceCount;

    // ── Mode field accessors ─────────────────────────────────────────
    uint8_t assocScope()    const { return (uint8_t)( _mode        & 0x03u); }
    uint8_t contestBeh()    const { return (uint8_t)((_mode >>  2) & 0x03u); }
    uint8_t postScore()     const { return (uint8_t)((_mode >>  4) & 0x03u); }
    uint8_t resolution()    const { return (uint8_t)((_mode >>  6) & 0x03u); }
    uint8_t timerMode()     const { return (uint8_t)((_mode >>  8) & 0x03u); }

    bool pauseInContest()   const { return (_mode & CPPolicy::FLAG_TIMER_PAUSE_IN_CONTEST) != 0; }
    bool lockOnPrime()      const { return (_mode & CPPolicy::FLAG_LOCK_ON_PRIME)          != 0; }
    bool lockOnCount()      const { return (_mode & CPPolicy::FLAG_LOCK_ON_COUNT)          != 0; }
    bool roleGated()        const { return (_mode & CPPolicy::FLAG_ROLE_GATED)             != 0; }
    bool suspendEnabled()   const { return (_mode & CPPolicy::FLAG_SUSPEND_ENABLED)        != 0; }
    bool contestTimer()     const { return (_mode & CPPolicy::FLAG_CONTEST_TIMER)          != 0; }
    bool reactivatable()    const { return (_mode & CPPolicy::FLAG_REACTIVATABLE)          != 0; }
    bool enemyTeamOnly()    const { return (_mode & CPPolicy::FLAG_ENEMY_TEAM_ONLY)        != 0; }
    bool silentInactive()   const { return (_mode & CPPolicy::FLAG_SILENT_INACTIVE)        != 0; }

    bool isFlagRole() const {
        return _roleId == TotemRoleId::FLAG_O || _roleId == TotemRoleId::FLAG_X;
    }
    bool isBaseRole() const {
        return _roleId == TotemRoleId::BASE_O
            || _roleId == TotemRoleId::BASE_X
            || _roleId == TotemRoleId::BASE;
    }

    // ── Association helpers ──────────────────────────────────────────

    void clearAssoc() {
        _assocPlayer = CP_NO_PLAYER;
        _assocTeam   = CP_NO_TEAM;
        _assocRole   = CP_NO_ROLE;
    }

    void applyAssoc(const PresenceEntry& e) {
        _assocPlayer = e.senderId;
        _assocTeam   = e.team;
        _assocRole   = e.role;
    }

    void savePrev() {
        _prevPlayer = _assocPlayer;
        _prevTeam   = _assocTeam;
        _prevRole   = _assocRole;
    }

    void restorePrev() {
        _assocPlayer = _prevPlayer;
        _assocTeam   = _prevTeam;
        _assocRole   = _prevRole;
    }

    bool matchesAssoc(const PresenceEntry& e) const {
        switch (assocScope()) {
            case CPPolicy::Scope::PLAYER: return e.senderId == _assocPlayer;
            case CPPolicy::Scope::ROLE:   return e.role     == _assocRole;
            case CPPolicy::Scope::ANY:    return true;
            default:                      return e.team     == _assocTeam;  // TEAM
        }
    }

    bool assocPresent() const {
        for (uint8_t i = 0; i < _presenceCount; i++)
            if (matchesAssoc(_presence[i])) return true;
        return false;
    }

    const PresenceEntry* firstOther() const {
        for (uint8_t i = 0; i < _presenceCount; i++)
            if (!matchesAssoc(_presence[i])) return &_presence[i];
        return nullptr;
    }

    bool snapshotPresent(uint8_t pid, uint8_t tm, uint8_t rl) const {
        for (uint8_t i = 0; i < _presenceCount; i++) {
            const PresenceEntry& e = _presence[i];
            switch (assocScope()) {
                case CPPolicy::Scope::PLAYER: if (e.senderId == pid) return true; break;
                case CPPolicy::Scope::ROLE:   if (e.role     == rl)  return true; break;
                case CPPolicy::Scope::ANY:    return _presenceCount > 0;
                default:                      if (e.team     == tm)  return true; break;
            }
        }
        return false;
    }

    // ── State entry ──────────────────────────────────────────────────

    void enterState(CPState s, uint32_t now, LightAir_TotemOutput& out,
                    bool preserveAccum = false) {
        _state          = s;
        _stateEnteredAt = now;
        if (!preserveAccum) _accumulatedMs = 0;
        updateBackground(out);
    }

    // ── UI helpers ───────────────────────────────────────────────────

    // Colour of the currently associated entity (team or player).
    void assocColor(uint8_t& r, uint8_t& g, uint8_t& b) const {
        if (assocScope() != CPPolicy::Scope::PLAYER && _assocTeam < TeamColors::kCount) {
            r = TeamColors::kColors[_assocTeam][0];
            g = TeamColors::kColors[_assocTeam][1];
            b = TeamColors::kColors[_assocTeam][2];
        } else if (_assocPlayer < PlayerDefs::MAX_PLAYER_ID) {
            r = PlayerColors::kColors[_assocPlayer][0];
            g = PlayerColors::kColors[_assocPlayer][1];
            b = PlayerColors::kColors[_assocPlayer][2];
        } else {
            r = 255; g = 255; b = 255;
        }
    }

    // Colour identifying this totem's own team; falls back to assocColor.
    void totemColor(uint8_t& r, uint8_t& g, uint8_t& b) const {
        if (_totemTeam < TeamColors::kCount) {
            r = TeamColors::kColors[_totemTeam][0];
            g = TeamColors::kColors[_totemTeam][1];
            b = TeamColors::kColors[_totemTeam][2];
        } else {
            assocColor(r, g, b);
        }
    }

    // ── Background animation ──────────────────────────────────────────

    void updateBackground(LightAir_TotemOutput& out) const {
        uint8_t r, g, b;
        switch (_state) {

            case CPState::IDLE:
                out.ui.trigger(TotemUIEvent::Idle);
                break;

            case CPState::PRIMED:
                r = 0; g = 0; b = 0;
                assocColor(r, g, b);
                r /= 4; g /= 4; b /= 4;
                out.ui.trigger(TotemUIEvent::Custom1, r, g, b);
                break;

            case CPState::ASSOCIATED:
                if (assocScope() == CPPolicy::Scope::PLAYER ||
                    assocScope() == CPPolicy::Scope::ANY) {
                    out.ui.trigger(TotemUIEvent::Control, 0xFF, _assocPlayer);
                } else {
                    out.ui.trigger(TotemUIEvent::Control, _assocTeam);
                }
                break;

            case CPState::CONTESTED:
                out.ui.trigger(TotemUIEvent::ControlContest);
                break;

            case CPState::SUSPENDED:
                out.ui.trigger(TotemUIEvent::Custom2, 40, 40, 40);
                break;

            case CPState::COOLDOWN:
                r = 0; g = 0; b = 0;
                assocColor(r, g, b);
                r /= 8; g /= 8; b /= 8;
                out.ui.trigger(TotemUIEvent::Custom3, r, g, b);
                break;

            case CPState::INACTIVE:
                if (isFlagRole()) {
                    // Flag is out: show team colour looping "missing" animation.
                    r = 0; g = 0; b = 0;
                    totemColor(r, g, b);
                    out.ui.trigger(TotemUIEvent::FlagMissing, r, g, b);
                } else {
                    // Depleted or permanently locked out: dark/off.
                    out.ui.trigger(TotemUIEvent::Custom4, 0, 0, 0);
                }
                break;
        }
    }

    // ── Beacon & scoring ─────────────────────────────────────────────

    uint8_t countdownRemainingSecs(uint32_t now) const {
        if (_state == CPState::ASSOCIATED || _state == CPState::CONTESTED) {
            uint32_t elapsed = (timerMode() == CPPolicy::Timer::ACCUMULATE)
                               ? _accumulatedMs
                               : (now - _stateEnteredAt);
            if (elapsed >= _countdownMs) return 0;
            uint32_t rem = (_countdownMs - elapsed) / 1000u;
            return (rem > 254u) ? 254u : (uint8_t)rem;
        }
        if (_state == CPState::PRIMED) {
            uint32_t elapsed = now - _stateEnteredAt;
            if (elapsed >= _primeMs) return 0;
            uint32_t rem = (_primeMs - elapsed) / 1000u;
            return (rem > 254u) ? 254u : (uint8_t)rem;
        }
        return CP_SECS_NA;
    }

    void broadcastBeacon(uint32_t now, LightAir_TotemOutput& out) const {
        uint8_t pl[7] = {
            (uint8_t)_state,
            _assocPlayer,
            _assocTeam,
            _assocRole,
            (uint8_t)( _context       & 0xFFu),
            (uint8_t)((_context >> 8) & 0xFFu),
            countdownRemainingSecs(now),
        };
        out.radio.broadcast(_beaconMsgType, pl, 7);
    }

    void awardScore(LightAir_TotemOutput& out) {
        _pointsAwarded++;
        uint8_t pl[3] = { _assocPlayer, _assocTeam, _assocRole };
        out.radio.broadcast(MSG_CP_SCORE, pl, 3);

        // Role-specific UI event for the score moment.
        uint8_t r = 0, g = 0, b = 0;
        if (_roleId == TotemRoleId::MALUS) {
            out.ui.trigger(TotemUIEvent::Malus);
        } else if (isBaseRole()) {
            assocColor(r, g, b);
            out.ui.trigger(TotemUIEvent::Respawn, r, g, b);
        } else if (isFlagRole()) {
            totemColor(r, g, b);
            out.ui.trigger(TotemUIEvent::FlagTaken, r, g, b);
        } else {
            // CP, BONUS, and anything else.
            out.ui.trigger(TotemUIEvent::Bonus);
        }
    }

    // ── Post-score transition ─────────────────────────────────────────

    void handlePostScore(uint32_t now, LightAir_TotemOutput& out) {
        // Max-points depletion overrides the per-mode post-score setting.
        if (_maxPoints > 0 && _pointsAwarded >= _maxPoints) {
            enterState(CPState::INACTIVE, now, out);
            return;
        }
        switch (postScore()) {
            case CPPolicy::PostScore::LOOP:
                clearAssoc();
                enterState(CPState::IDLE, now, out);
                break;
            case CPPolicy::PostScore::COOLDOWN:
                enterState(CPState::COOLDOWN, now, out);
                break;
            case CPPolicy::PostScore::INACTIVE:
                enterState(CPState::INACTIVE, now, out);
                break;
            case CPPolicy::PostScore::REPEAT:
                _stateEnteredAt = now;
                _accumulatedMs  = 0;
                break;
        }
    }

    // ── Contest handling ──────────────────────────────────────────────

    void handleConflict(uint32_t now, LightAir_TotemOutput& out,
                        const PresenceEntry& other) {
        switch (contestBeh()) {
            case CPPolicy::Contest::HOLD:
                break;
            case CPPolicy::Contest::RESET:
                applyAssoc(other);
                _stateEnteredAt = now;
                _accumulatedMs  = 0;
                updateBackground(out);
                break;
            case CPPolicy::Contest::PAUSE:
            case CPPolicy::Contest::PAUSE_THEN_RESET:
                savePrev();
                enterState(CPState::CONTESTED, now, out);
                break;
        }
    }

    // ── Per-state window evaluators ───────────────────────────────────

    void evaluateIdle(uint32_t now, LightAir_TotemOutput& out) {
        if (_presenceCount == 0) return;
        for (uint8_t i = 0; i < _presenceCount; i++) {
            if (roleGated() && _presence[i].role == CP_NO_ROLE) continue;
            applyAssoc(_presence[i]);
            if (timerMode() == CPPolicy::Timer::PRIMED) {
                enterState(CPState::PRIMED, now, out);
            } else if (timerMode() == CPPolicy::Timer::IMMEDIATE) {
                enterState(CPState::ASSOCIATED, now, out);
                awardScore(out);
                handlePostScore(now, out);
            } else {
                enterState(CPState::ASSOCIATED, now, out);
            }
            return;
        }
    }

    void evaluatePrimed(uint32_t now, LightAir_TotemOutput& out) {
        bool ap                  = assocPresent();
        const PresenceEntry* oth = lockOnPrime() ? nullptr : firstOther();

        if (!ap && !lockOnPrime()) {
            clearAssoc();
            enterState(CPState::IDLE, now, out);
            return;
        }
        if (oth) {
            handleConflict(now, out, *oth);
            return;
        }
        if ((now - _stateEnteredAt) >= _primeMs)
            enterState(CPState::ASSOCIATED, now, out);
    }

    void evaluateAssociated(uint32_t now, LightAir_TotemOutput& out) {
        bool ap                  = assocPresent();
        const PresenceEntry* oth = lockOnCount() ? nullptr : firstOther();

        if (!ap) {
            if (timerMode() == CPPolicy::Timer::ACCUMULATE) {
                if (oth) handleConflict(now, out, *oth);
            } else {
                clearAssoc();
                enterState(CPState::IDLE, now, out);
            }
            return;
        }
        if (oth) {
            handleConflict(now, out, *oth);
            return;
        }

        if (timerMode() == CPPolicy::Timer::ACCUMULATE)
            _accumulatedMs += (now - _windowStart);

        uint32_t elapsed = (timerMode() == CPPolicy::Timer::ACCUMULATE)
                           ? _accumulatedMs
                           : (now - _stateEnteredAt);
        if (elapsed >= _countdownMs) {
            awardScore(out);
            handlePostScore(now, out);
        }
    }

    void evaluateContested(uint32_t now, LightAir_TotemOutput& out) {
        if (contestTimer() && (now - _stateEnteredAt) >= _countdownMs) {
            clearAssoc();
            enterState(CPState::IDLE, now, out);
            return;
        }
        if (_presenceCount == 0) {
            clearAssoc();
            enterState(CPState::IDLE, now, out);
            return;
        }

        bool prevPresent = snapshotPresent(_prevPlayer, _prevTeam, _prevRole);

        // Temporarily adopt prev snapshot so firstOther() sees "others" correctly.
        uint8_t sp = _assocPlayer, st = _assocTeam, sr = _assocRole;
        _assocPlayer = _prevPlayer; _assocTeam = _prevTeam; _assocRole = _prevRole;
        const PresenceEntry* otherEntry = firstOther();
        bool othersPresent = (otherEntry != nullptr);
        _assocPlayer = sp; _assocTeam = st; _assocRole = sr;

        if (prevPresent && othersPresent) return;  // still unresolved

        bool resetTimer = (contestBeh() == CPPolicy::Contest::PAUSE_THEN_RESET);

        if (prevPresent && !othersPresent) {
            restorePrev();
            enterState(CPState::ASSOCIATED, now, out, !resetTimer);
            return;
        }

        switch (resolution()) {
            case CPPolicy::Resolution::PREV_WINS:
                clearAssoc();
                enterState(CPState::IDLE, now, out);
                break;
            case CPPolicy::Resolution::LAST_STANDING:
                if (othersPresent) {
                    applyAssoc(*otherEntry);
                    enterState(CPState::ASSOCIATED, now, out);
                } else {
                    clearAssoc();
                    enterState(CPState::IDLE, now, out);
                }
                break;
            case CPPolicy::Resolution::NEUTRALIZE:
                clearAssoc();
                enterState(CPState::IDLE, now, out);
                break;
            case CPPolicy::Resolution::FIRST_REPLY:
                applyAssoc(_presence[0]);
                enterState(CPState::ASSOCIATED, now, out);
                break;
        }
    }

    void evaluateSuspended(uint32_t now, LightAir_TotemOutput& out) {
        if (_suspendMs > 0 && (now - _stateEnteredAt) >= _suspendMs) {
            clearAssoc();
            enterState(CPState::IDLE, now, out);
        }
    }

    void evaluateCooldown(uint32_t now, LightAir_TotemOutput& out) {
        if ((now - _stateEnteredAt) >= _cooldownMs) {
            clearAssoc();
            enterState(CPState::IDLE, now, out);
        }
    }

    void evaluateWindow(uint32_t now, LightAir_TotemOutput& out) {
        switch (_state) {
            case CPState::IDLE:       evaluateIdle(now, out);       break;
            case CPState::PRIMED:     evaluatePrimed(now, out);     break;
            case CPState::ASSOCIATED: evaluateAssociated(now, out); break;
            case CPState::CONTESTED:  evaluateContested(now, out);  break;
            case CPState::SUSPENDED:  evaluateSuspended(now, out);  break;
            case CPState::COOLDOWN:   evaluateCooldown(now, out);   break;
            case CPState::INACTIVE:                                  break;
        }
    }

public:
    CPTotem() { reset(); }

    void onActivate(const uint8_t* payload, uint8_t len,
                    LightAir_TotemOutput& out) override {
        // ── Config defaults ──
        _roleId           = (len >= 1) ? payload[0] : TotemRoleId::CP;
        _mode             = 0;
        _countdownMs      = CP_DEFAULT_COUNTDOWN_MS;
        _cooldownMs       = CP_DEFAULT_COOLDOWN_MS;
        _suspendMs        = 0;
        _primeMs          = CP_DEFAULT_PRIME_MS;
        _beaconMsgType    = MSG_CP_BEACON;
        _maxPoints        = 0;
        _beaconIntervalMs = CP_BEACON_INTERVAL_MS;
        _totemTeam        = CP_NO_TEAM;

        // ── Parse payload ──
        if (len >= 5) {
            _mode = (uint32_t)payload[1]
                  | ((uint32_t)payload[2] <<  8)
                  | ((uint32_t)payload[3] << 16)
                  | ((uint32_t)payload[4] << 24);
        }
        if (len >= 7) {
            uint16_t s = (uint16_t)payload[5] | ((uint16_t)payload[6] << 8);
            if (s > 0) _countdownMs = (uint32_t)s * 1000u;
        }
        if (len >= 10 && payload[9]  > 0) _cooldownMs = (uint32_t)payload[9]  * 1000u;
        if (len >= 11)                     _suspendMs  = (uint32_t)payload[10] * 1000u;
        if (len >= 12 && payload[11] > 0)  _primeMs    = (uint32_t)payload[11] * 1000u;

        uint16_t initCtx = 0;
        if (len >= 9) initCtx = (uint16_t)payload[7] | ((uint16_t)payload[8] << 8);

        if (len >= 13 && payload[12] != 0) _beaconMsgType    = payload[12];
        if (len >= 14)                      _maxPoints        = payload[13];
        if (len >= 15 && payload[14] != 0)  _beaconIntervalMs = (uint32_t)payload[14] * 100u;
        if (len >= 16)                      _totemTeam        = payload[15];

        reset();
        _context     = initCtx;
        _windowStart = millis();
        out.ui.trigger(TotemUIEvent::Idle);
    }

    void onMessage(const RadioPacket& msg, LightAir_TotemOutput& out) override {
        if (msg.msgType != _beaconMsgType + 1) return;
        if (msg.payloadLen == 0) return;

        auto action = (CPAction)msg.payload[0];

        if (action == CPAction::PRESENCE) {
            // Enemy-team-only mode: discard presence from the totem's own team.
            if (enemyTeamOnly() && msg.team == _totemTeam) return;
            if (_presenceCount < CP_PRESENCE_MAX)
                _presence[_presenceCount++] = { msg.senderId, msg.team, msg.role };
            return;
        }

        uint32_t now = millis();

        if (action == CPAction::SUSPEND && suspendEnabled() &&
            _state != CPState::SUSPENDED && _state != CPState::INACTIVE) {
            enterState(CPState::SUSPENDED, now, out);
            return;
        }

        if (action == CPAction::RESUME && _state == CPState::SUSPENDED) {
            clearAssoc();
            enterState(CPState::IDLE, now, out);
            return;
        }

        if (action == CPAction::REACTIVATE && reactivatable() &&
            _state == CPState::INACTIVE) {
            clearAssoc();
            enterState(CPState::IDLE, now, out);
            // For FLAG roles fire FlagReturn as one-shot overlay after the Idle background.
            if (isFlagRole()) {
                uint8_t r = 0, g = 0, b = 0;
                totemColor(r, g, b);
                out.ui.trigger(TotemUIEvent::FlagReturn, r, g, b);
            }
            return;
        }

        if (action == CPAction::CONTEXT && msg.payloadLen >= 3) {
            _context = (uint16_t)msg.payload[1] | ((uint16_t)msg.payload[2] << 8);
            broadcastBeacon(now, out);  // push updated context immediately
            return;
        }
    }

    void update(LightAir_TotemOutput& out) override {
        uint32_t now = millis();
        if ((now - _windowStart) < _beaconIntervalMs) return;

        evaluateWindow(now, out);

        _presenceCount = 0;
        _windowStart   = now;

        // FLAG_SILENT_INACTIVE suppresses the beacon when the flag is "out".
        if (_state != CPState::INACTIVE || !silentInactive())
            broadcastBeacon(now, out);
    }

    void reset() override {
        _state          = CPState::IDLE;
        _context        = 0;
        _pointsAwarded  = 0;
        clearAssoc();
        _prevPlayer     = CP_NO_PLAYER;
        _prevTeam       = CP_NO_TEAM;
        _prevRole       = CP_NO_ROLE;
        _stateEnteredAt = 0;
        _accumulatedMs  = 0;
        _windowStart    = 0;
        _presenceCount  = 0;
        // Safe defaults so onMessage/update work even before onActivate.
        _beaconMsgType    = MSG_CP_BEACON;
        _beaconIntervalMs = CP_BEACON_INTERVAL_MS;
        _totemTeam        = CP_NO_TEAM;
        _maxPoints        = 0;
        _roleId           = TotemRoleId::CP;
    }
};

// ── Singleton ────────────────────────────────────────────────────
static CPTotem s_cp;
LightAir_TotemRunner* totemRunner_cp = &s_cp;
