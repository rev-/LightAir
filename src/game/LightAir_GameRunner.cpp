#include "LightAir_GameRunner.h"
#include "../enlight/Enlight.h"
#include <Arduino.h>
#include <string.h>
#include <esp_system.h>

// The optical device, constructed by the sketch once NVS calibration is
// loaded.  The runner only ever applies the optics a game queued during the
// LOGIC phase; starting and polling measurements stays with the ruleset.
extern Enlight* enlightPtr;

/* =========================================================
 *   BEGIN — one-time setup
 * ========================================================= */

void LightAir_GameRunner::begin(const LightAir_Game& game,
                                 LightAir_DisplayCtrl& display,
                                 LightAir_InputCtrl&   input,
                                 LightAir_Radio&       radio,
                                 LightAir_UICtrl*      ui,
                                 Enlight*              enlight,
                                 SpiAdcSensor**        sensors,
                                 uint8_t               sensorCount,
                                 float*                battVoltsOut) {
    _game    = &game;
    _display = &display;
    _input   = &input;
    _radio   = &radio;
    _ui      = ui;
    _bindingCount = 0;

    _enlight     = enlight;
    _battVoltsOut = battVoltsOut;
    _sensorCount = (sensorCount < MAX_SENSORS) ? sensorCount : MAX_SENSORS;
    for (uint8_t i = 0; i < _sensorCount; i++)
        _sensors[i] = sensors ? sensors[i] : nullptr;
    _lastEnlightActiveMs = 0;
    _nextSensorReadMs    = 0;
    _sensorReadPending   = false;

    // -- Build display binding sets from MonitorVar::stateMask --
    // From zero: the set table belongs to one ruleset at a time, and a second
    // begin() in the same boot would otherwise append to the last one's.
    display.resetBindingSets();

    // Pass 1: collect unique state indices that need a binding set.
    for (uint8_t v = 0; v < game.monitorCount; v++) {
        uint32_t mask = game.monitorVars[v].stateMask;
        for (uint8_t s = 0; s < 32 && mask; s++, mask >>= 1) {
            if (!(mask & 1)) continue;
            bool found = false;
            for (uint8_t b = 0; b < _bindingCount; b++)
                if (_bindings[b].state == s) { found = true; break; }
            if (!found && _bindingCount < DisplayDefaults::MAX_SETS) {
                uint8_t setId = display.createBindingSet();
                if (setId != 255)
                    _bindings[_bindingCount++] = { s, setId };
            }
        }
    }

    // Pass 2: bind each monitor var to the sets for its states.
    for (uint8_t v = 0; v < game.monitorCount; v++) {
        const MonitorVar& var = game.monitorVars[v];
        for (uint8_t b = 0; b < _bindingCount; b++) {
            if (!(var.stateMask & (1u << _bindings[b].state))) continue;
            display.selectBindingSet(_bindings[b].setId);
            const uint8_t px = var.col * DisplayDefaults::CELL_WIDTH;
            const uint8_t py = var.row * DisplayDefaults::CELL_HEIGHT
                               + 3 * DisplayDefaults::FONT_HEIGHT;
            if (var.type == VarType::BAR)
                display.bindBarVariable(var.asInt, var.icon, px, py,
                                        var.barTrigger, var.barFill,
                                        var.barWidth ? var.barWidth
                                                     : DisplayDefaults::BAR_WIDTH,
                                        var.barStart, var.iconVar);
            else if (var.type == VarType::INT)
                display.bindIntVariable(var.asInt, var.icon, px, py, var.iconVar);
            else
                display.bindStringVariable(var.asChars, var.icon, px, py);
        }
    }

    // Create an empty binding set (no vars) used to freeze the display after scoreAnnounce.
    _emptyBindingSetId = display.createBindingSet();
    _endExitReady = false;

    // Reset state and activate the initial binding set.  A new match starts
    // with no measurement pending, whatever state the last one ended in.
    if (enlightPtr) enlightPtr->discardResult();
    enterState(game.initialState);

    // Stamp the game's typeId on the radio layer so all outgoing packets
    // carry it and incoming packets from other games are filtered out.
    radio.setTypeId(game.typeId);

    // User-provided setup (radio init, opening messages, etc.)
    if (game.onBegin) game.onBegin(display, radio, ui, *this);
}

/* =========================================================
 *   ROSTER
 * ========================================================= */

void LightAir_GameRunner::clearRoster() {
    _expectedPlayerMask = 0;
}

uint8_t LightAir_GameRunner::sessionSeed() const {
    return _radio ? _radio->sessionToken() : 0;
}

void LightAir_GameRunner::addToRoster(uint8_t id) {
    if (id == 0 || id >= PlayerDefs::MAX_PLAYER_ID) return;  // totem IDs and reserved silently ignored
    _expectedPlayerMask |= (1u << id);
}

/* =========================================================
 *   TOTEMS
 * ========================================================= */

void LightAir_GameRunner::clearTotems() {
    _totemCount = 0;
}

void LightAir_GameRunner::addTotem(uint8_t id, uint8_t roleId, uint8_t option) {
    if (_totemCount >= GameDefaults::MAX_PARTICIPANTS) return;
    for (uint8_t i = 0; i < _totemCount; i++)
        if (_totems[i].id == id) return;  // ignore duplicate
    _totems[_totemCount++] = { id, roleId, option };
}

uint8_t LightAir_GameRunner::totemOption(uint8_t id) const {
    for (uint8_t t = 0; t < _totemCount; t++)
        if (_totems[t].id == id) return _totems[t].option;
    return 0;
}

uint8_t LightAir_GameRunner::totemIdForRole(uint8_t roleId, uint8_t idx) const {
    uint8_t count = 0;
    for (uint8_t t = 0; t < _totemCount; t++) {
        if (_totems[t].roleId == roleId) {
            if (count == idx) return _totems[t].id;
            count++;
        }
    }
    return 0;  // not found
}

/* =========================================================
 *   SENSOR VALUE ACCESSOR
 * ========================================================= */

float LightAir_GameRunner::sensorValue(uint8_t idx) const {
    if (idx >= _sensorCount) return 0.0f;
    return _sensorValues[idx];
}

/* =========================================================
 *   TEAM MAP
 * ========================================================= */

void LightAir_GameRunner::setTeam(uint8_t id, uint8_t team) {
    if (id < PlayerDefs::MAX_PLAYER_ID) _teamMap[id] = team;
}

uint8_t LightAir_GameRunner::teamOf(uint8_t id) const {
    if (id < PlayerDefs::MAX_PLAYER_ID) return _teamMap[id];
    return 0xFF;
}

/* =========================================================
 *   UPDATE — one loop iteration
 * ========================================================= */

// Every state change goes through here.  Besides the display, it drops any
// Enlight measurement still undelivered: a beam belongs to the state it was
// fired in.  Only in-play states read results (proj.result), so a beam in
// flight when its shooter went down used to sit in Enlight's result slot for
// the whole wait and go out as a LIT on the first tick back in play — at
// whoever it hit seconds earlier, wherever they are now.  Replies are
// deliberately NOT touched: a SHONE reply arriving after the shooter went
// down is a point earned while in play.
void LightAir_GameRunner::enterState(uint8_t s) {
    if (*_game->currentState != s && enlightPtr) enlightPtr->discardResult();
    *_game->currentState = s;
    activateStateDisplay(s);
}

void LightAir_GameRunner::update() {
    uint32_t loopStart = millis();

    // ---- Sensor scheduling (three-state cadence) ----
    if (_enlight && _sensorCount > 0) {
        if (_enlight->isActive()) _lastEnlightActiveMs = loopStart;
        if (loopStart >= _nextSensorReadMs || _sensorReadPending) {
            if (_enlight->busy()) {
                // A run owns the ADC bus.  Ask it to leave the AFE rail up when
                // it finishes: the sensors hang off that rail, and riding on a
                // run's uptime saves powering and settling it ourselves.
                _enlight->holdAfe();
                _sensorReadPending = true;
            } else {
                // Free if a run (or the holdAfe() above) already has the rail
                // up, otherwise this powers it and blocks for the settling time.
                _enlight->ensureAfePowered();
                for (uint8_t i = 0; i < _sensorCount; i++) {
                    if (!_sensors[i]) continue;
                    float v;
                    if (_sensors[i]->read(v)) {
                        _sensorValues[i] = v;
                        if (i == 0 && _battVoltsOut) *_battVoltsOut = v;
                    }
                }
                _enlight->releaseAfe();
                _sensorReadPending = false;
                bool recentlyActive = (loopStart - _lastEnlightActiveMs)
                                      < SensorDefaults::ACTIVE_WINDOW_MS;
                _nextSensorReadMs = loopStart + (recentlyActive
                    ? SensorDefaults::SENSOR_ACTIVE_CADENCE_MS
                    : SensorDefaults::SENSOR_STANDBY_CADENCE_MS);
            }
        }
    }

    // ---- Step 1: READ ----
    const InputReport& inputs = _input->poll();

    // A+B: the player asks for the tools menu.  Checked before the radio is
    // polled, so nothing received this cycle is dropped — the hold's own
    // cycles poll it from here on.
    if (holdChord(inputs)) {
        runHold();
        return;
    }

    const RadioReport& radio  = _radio->poll();

    // ---- Step 2: LOGIC ----
    GameOutput output;

    if (_scoreActive) {
        scoreRadio(radio, output);
        scoreInput(inputs);
        _display->update();
        flushOutput(output);
        while ((millis() - loopStart) < GameDefaults::LOOP_MS) {}
        return;
    }

    logic(inputs, radio, output);           // steps 2a-2d
    startScoringIfEntered(output);

    // Step 2e: StateBehavior — per-state continuous logic.
    for (uint8_t i = 0; i < _game->behaviorCount; i++) {
        if (_game->behaviors[i].state != *_game->currentState) continue;
        if (_game->behaviors[i].onUpdate)
            _game->behaviors[i].onUpdate(inputs, radio, *_display, output);
        break;
    }

    // ---- Step 3: OUTPUT ----
    _display->update();
    flushOutput(output);

    // Enforce fixed loop duration.
    while ((millis() - loopStart) < GameDefaults::LOOP_MS) {}
}

// Steps 2a-2d, shared by update() and the reduced cycles of a hold.
// Held (_held), two things change — everything else is identical, which is
// the point: a held player is answered by the very same handlers.
//   * direct rules ignore totem senders, and anything outside the ruleset's
//     hold.accept list;
//   * the end-of-match transition keeps its radio but defers its UI cue
//     (runEndAction()).
// State rules DO run while held, fed an empty InputReport (the keys belong
// to the tool).  So the player's state follows from what happens to them —
// shone to zero lives, a respawn clock, the match clock — and never from
// what they do.  Freezing the rules instead looks tidier and is wrong: a
// player shone to zero would stay in play and answer every further LIT with
// another SHONE, handing out a point per beam and counting lives below zero.
void LightAir_GameRunner::logic(const InputReport& inputs,
                                const RadioReport& radio,
                                GameOutput&        output) {
    // Step 2a: Infrastructure intercepts — handle before DirectRadioRules.
    // Marked events are skipped by the DirectRadioRules loop below.
    bool infraHandled[RADIO_MAX_PENDING] = {};

    // MSG_END_GAME: force scoringState entry on any device that hasn't yet transitioned.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != GameDefaults::MSG_END_GAME) continue;
        infraHandled[e] = true;
        if (*_game->currentState != _game->scoringState) {
            uint8_t prev = *_game->currentState;
            enterState(_game->scoringState);
            const StateRule* match = nullptr;
            for (uint8_t i = 0; i < _game->ruleCount; i++) {
                const StateRule& r = _game->rules[i];
                if (r.fromState == prev && r.toState == _game->scoringState) {
                    match = &r;
                    break;
                }
            }
            runEndAction(match, match != nullptr, output);
        }
    }

    // MSG_TOTEM_BEACON: reply with the totem's assigned role, the current session
    // token, and the game's remaining time (see replyToTotemBeacon()).
    // No reply is sent to non-totem senders or unconfigured totems.
    // Host infrastructure, not a player action: it runs while held too.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type           != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != RadioMsg::MSG_TOTEM_BEACON)      continue;
        infraHandled[e] = true;
        replyToTotemBeacon(ev, output);
    }

    // Step 2b: DirectRadioRules — handle all incoming MessageReceived events.
    // Events intercepted above (MSG_END_GAME, MSG_TOTEM_BEACON) are skipped.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type != RadioEventType::MessageReceived) continue;
        if (infraHandled[e]) continue;

        if (_held) {
            // Every totem action is an answer to a totem's beacon (pickups,
            // CP presence, BASE respawn): dropping totem senders removes all
            // of them and nothing else.
            if (TotemDefs::isTotemId(ev.packet.senderId)) continue;
            if (_game->holdAccept) {
                bool ok = false;
                for (uint8_t k = 0; k < _game->holdAcceptCount; k++)
                    if (_game->holdAccept[k] == ev.packet.msgType) { ok = true; break; }
                if (!ok) continue;
            }
        }

        for (uint8_t i = 0; i < _game->directRadioRuleCount; i++) {
            const DirectRadioRule& r = _game->directRadioRules[i];
            if (r.fromState != *_game->currentState) continue;
            if (r.msgType   != ev.packet.msgType)    continue;
            if (r.condition && !r.condition(ev.packet)) continue;

            if (r.onReceive) r.onReceive(ev.packet, ev.rssi, *_display, output);
            // DYNAMIC_REPLY: the callback queued its own reply with a
            // runtime-decided sub-type (Lua handlers return it).
            if (r.replySubType != DirectRadioRule::DYNAMIC_REPLY)
                output.radio.reply(ev.packet, r.replySubType);
            break;
        }
        // No blanket reply for an unmatched message.  Totem beacons are
        // broadcasts every player in range hears; answering all of them was
        // pure airtime, and it let an uninterested player's empty reply stand
        // in for the deliberate one a BASE or BONUS was waiting for.  A reply
        // now means "this ruleset acted on your beacon", nothing else.
    }

    // Step 2c: ReplyRadioRules — handle all ReplyReceived and Timeout events.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type != RadioEventType::ReplyReceived &&
            ev.type != RadioEventType::Timeout) continue;

        uint8_t state = *_game->currentState;
        for (uint8_t i = 0; i < _game->replyRadioRuleCount; i++) {
            const ReplyRadioRule& r = _game->replyRadioRules[i];
            if (!(r.activeInStateMask & (1u << state))) continue;
            if (r.eventType != ev.type) continue;
            if (ev.type == RadioEventType::ReplyReceived &&
                r.replySubType != 0 &&
                (ev.packet.payloadLen == 0 || ev.packet.payload[0] != r.replySubType)) continue;
            if (r.condition && !r.condition(ev.packet, ev.original)) continue;

            if (r.onReply) r.onReply(ev.packet, ev.original, ev.rssi, *_display, output);
            break;
        }
    }

    // Step 2d: StateRules — evaluate transitions (first match wins).
    for (uint8_t i = 0; i < _game->ruleCount; i++) {
        const StateRule& r = _game->rules[i];
        if (r.fromState != *_game->currentState) continue;
        if (r.condition && !r.condition(inputs, radio)) continue;

        enterState(r.toState);
        if (r.toState == _game->scoringState) runEndAction(&r, true, output);
        else if (r.onTransition)              r.onTransition(*_display, output);
        break;
    }
}

// The transition into the scoring state, by rule or by MSG_END_GAME.
// `matched`: a rule describes this transition (run its action, if any);
// otherwise the default EndGame cue plays.
//
// Held, the action still runs NOW — it can compute the very winner vars
// this device is about to broadcast (Virus's clean_secs) — and its radio
// goes out now, but its UI cue waits for the hold to end, with the end
// screen it belongs to.  Its tray lines wait too: the tray is paused.
void LightAir_GameRunner::runEndAction(const StateRule* rule, bool matched,
                                       GameOutput& output) {
    GameOutput  local;
    GameOutput& dst = _held ? local : output;
    if (matched) { if (rule && rule->onTransition) rule->onTransition(*_display, dst); }
    else         dst.ui.trigger(LightAir_UICtrl::UIEvent::EndGame);
    if (!_held) return;

    flushRadio(local);
    for (uint8_t i = 0; i < local.ui.count; i++) {
        if (_deferredUi.count >= UI_OUT_MAX) break;
        _deferredUi.msgs[_deferredUi.count++] = local.ui.msgs[i];
    }
    if (local.optics.hasCycles)   _heldOptics.setCycles(local.optics.cycles);
    if (local.optics.hasCooldown) _heldOptics.setCooldown(local.optics.cooldownMs);
}

// After Step 2d: detect scoringState entry and kick off score collection.
// Radio and bookkeeping only; what reaches the screen goes through the tray,
// which a hold keeps paused until the tool returns.
void LightAir_GameRunner::startScoringIfEntered(GameOutput& output) {
    if (*_game->currentState != _game->scoringState || _scoreActive) return;

    _scoreActive      = true;
    _scoreResultShown = false;
    _scorePresent     = 0;
    _scoreSentAt      = 0;
    _scoreEntryAt     = millis();
    memset(_scoreSlots, 0, sizeof(_scoreSlots));

    // Record own scores immediately.
    uint8_t myId = _radio->playerId();
    if (_expectedPlayerMask & (1u << myId)) {
        scoreFillSlot(_scoreSlots[myId]);
        _scorePresent |= (1u << myId);
    }

    // Flood MSG_END_GAME so devices still in a non-scoring state transition.
    output.radio.broadcast(GameDefaults::MSG_END_GAME, nullptr, 0, 2);
    // Flood MSG_TOTEM_ROSTER so any activated totem reverts to stateless.
    // A single broadcast can be lost, so scoreRadio() keeps re-sending it
    // for the duration of the end-game screen (see _rosterSentAt).
    output.radio.broadcast(RadioMsg::MSG_TOTEM_ROSTER, nullptr, 0, 2);
    _rosterSentAt = millis();
    scoreBroadcastFused(output);
    _scoreSentAt = millis();

    if (_scorePresent == _expectedPlayerMask) {
        _scoreResultShown = true;
        postScoreAnnounce();
        scoreAnnounce();
    }
}

/* =========================================================
 *   IN-GAME HOLD — see LightAir_GameHold.h
 * ========================================================= */

struct LightAir_GameRunner::HoldHostAdapter : LightAir_HoldHost {
    explicit HoldHostAdapter(LightAir_GameRunner& r) : _r(r) {}
    void service() override { _r.holdService(); }
    LightAir_GameRunner& _r;
};

static bool keyDown(const InputReport& in, char key) {
    for (uint8_t i = 0; i < in.keyEventCount; i++) {
        const InputReport::KeyEntry& ke = in.keyEvents[i];
        if (ke.keypadId != InputDefaults::KEYPAD_ID || ke.key != key) continue;
        return ke.state == KeyState::PRESSED || ke.state == KeyState::HELD;
    }
    return false;
}

static bool keyHeld(const InputReport& in, char key) {
    for (uint8_t i = 0; i < in.keyEventCount; i++) {
        const InputReport::KeyEntry& ke = in.keyEvents[i];
        if (ke.keypadId != InputDefaults::KEYPAD_ID || ke.key != key) continue;
        return ke.state == KeyState::HELD;
    }
    return false;
}

// Both keys past the long press.  After a hold the chord stays latched
// until both are up, so keys still down when the tool returns cannot
// reopen it at once.
bool LightAir_GameRunner::holdChord(const InputReport& in) {
    if (!_holdTool) return false;
    if (_chordLatched) {
        if (!keyDown(in, 'A') && !keyDown(in, 'B')) _chordLatched = false;
        return false;
    }
    return keyHeld(in, 'A') && keyHeld(in, 'B');
}

void LightAir_GameRunner::runHold() {
    holdBegin();
    HoldHostAdapter host(*this);
    _holdTool->runHeld(host);
    holdEnd();
}

void LightAir_GameRunner::holdBegin() {
    _held        = true;
    _holdLastMs  = 0;
    _heldOptics  = OpticsOutput();
    _deferredUi.count = 0;

    // Detach the game from the optics: every la.shine* verb and the optics
    // flush guard on this handle, so with it unplugged the ruleset cannot
    // start, read or reconfigure a measurement — the tool has the device.
    _heldEnlight = enlightPtr;
    enlightPtr   = nullptr;

    _display->pauseTray();

    _holdHooked = false;
    if (!_scoreActive && _game->onHoldEnter) {
        GameOutput o;
        _game->onHoldEnter(*_display, o);
        flushOutput(o);
        _holdHooked = true;
    }
}

// One reduced cycle.  Radio in, the shared logic with the held filters, the
// score exchange if the match is over, declarative clocks — never the
// keypad (the tool polls it) and never the screen (the tool draws it).
void LightAir_GameRunner::holdService() {
    static InputReport s_noInput;            // zero-initialised: no keys, no buttons

    const uint32_t now = millis();
    if (_holdLastMs != 0 && now - _holdLastMs < GameDefaults::LOOP_MS) return;
    _holdLastMs = now ? now : 1;

    const RadioReport& radio = _radio->poll();
    GameOutput output;

    if (_scoreActive) {
        scoreRadio(radio, output);
    } else {
        logic(s_noInput, radio, output);
        startScoringIfEntered(output);
        if (!_scoreActive && _game->onClockTick) _game->onClockTick();
    }
    flushOutput(output);
}

void LightAir_GameRunner::holdEnd() {
    if (_holdHooked && _game->onHoldExit) {
        GameOutput o;
        _game->onHoldExit(*_display, o);
        flushOutput(o);                      // still held: optics are stashed
    }
    _holdHooked = false;
    _held       = false;

    // Plug the optics back in.  A result the tool left behind is not the
    // game's; the optics the ruleset asked for meanwhile apply now, over
    // whatever the tool restored.
    enlightPtr = _heldEnlight;
    if (enlightPtr) {
        enlightPtr->discardResult();
        if (_heldOptics.hasCycles)   enlightPtr->setRepetitions(_heldOptics.cycles);
        if (_heldOptics.hasCooldown) enlightPtr->setCooldown((int64_t)_heldOptics.cooldownMs);
    }

    // Back to the game screen — the end screen, if the match ended meanwhile
    // — with every tray line queued during the hold, full duration each.
    _display->resumeTray();
    _display->requestRedraw();
    if (_ui) {
        for (uint8_t i = 0; i < _deferredUi.count; i++)
            _ui->trigger(_deferredUi.msgs[i].event);
    }
    _deferredUi.count = 0;

    _chordLatched      = true;
    _restartDownAt     = 0;
    _restartSpoiled    = false;
    _sensorReadPending = false;
    _nextSensorReadMs  = millis();
}

/* =========================================================
 *   TOTEM BEACON REPLY (0xF1) — shared by update() and a hold's cycles
 * ========================================================= */

// Builds the activation reply for one MSG_TOTEM_BEACON event:
//   payload[0]   = the totem's assigned roleId
//   payload[1]   = the current session token (so the totem learns it for the
//                  duration of the game)
//   payload[2:3] = gameTimeLeft as uint16_t, big-endian, exact seconds;
//                  0xFFFF if the ruleset has no live countdown to report
//   payload[4]   = TotemVMDefs::VERSION
//   payload[5:6] = program length, uint16_t little-endian
//   payload[7..] = the serialized TotemVM program (the totem's whole
//                  behaviour; per-role config seconds are baked into it)
//
// No reply is sent to non-totem senders, totems with no assigned role,
// or roles the game defines no program for.
void LightAir_GameRunner::replyToTotemBeacon(const RadioEvent& ev, GameOutput& output) {
    uint8_t id = ev.packet.senderId;
    if (!TotemDefs::isTotemId(id)) return;

    for (uint8_t t = 0; t < _totemCount; t++) {
        if (_totems[t].id != id) continue;
        uint8_t roleId = _totems[t].roleId;
        uint8_t buf[7 + TotemVMDefs::MAX_PROG] = { roleId, _radio->sessionToken(), 0xFF, 0xFF };

        if (_game->gameTimeLeft) {
            uint16_t secs = (uint16_t)*_game->gameTimeLeft;   // exact, no rounding
            buf[2] = (uint8_t)(secs >> 8);
            buf[3] = (uint8_t)(secs & 0xFF);
        }

        const TotemProgramEntry* prog =
            _game->totemProgram ? _game->totemProgram(roleId) : nullptr;
        if (!prog || !prog->bytes || prog->len > TotemVMDefs::MAX_PROG)
            break;   // no program for this role: no reply, totem stays IDLE
        buf[4] = TotemVMDefs::VERSION;
        buf[5] = (uint8_t)(prog->len & 0xFF);
        buf[6] = (uint8_t)(prog->len >> 8);
        memcpy(buf + 7, prog->bytes, prog->len);
        output.radio.replyWithPayload(ev.packet, buf, (uint8_t)(7 + prog->len));
        break;
    }
}

/* =========================================================
 *   SCORE UPDATE — ongoing scoring phase (runs when _scoreActive)
 * ========================================================= */

// The radio half: runs every cycle, held or not, so a device busy in a tool
// still answers the round-robin and the winner is decided on time for all.
void LightAir_GameRunner::scoreRadio(const RadioReport& radio, GameOutput& output) {
    const uint8_t slotSize = _game->winnerVarCount * 4;
    const uint8_t recSize  = 1 + slotSize;  // id byte + score data

    // Accumulate per-player score messages.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != _game->scoreMsgType)  continue;
        if (ev.packet.payloadLen == 0 || ev.packet.payloadLen % recSize != 0) continue;

        bool changed = false;
        for (uint8_t off = 0; off + recSize <= ev.packet.payloadLen; off += recSize) {
            uint8_t id = ev.packet.payload[off];
            if (id == 0 || id >= PlayerDefs::MAX_PLAYER_ID) continue;
            if (!(_expectedPlayerMask & (1u << id))) continue;
            if (_scorePresent & (1u << id)) continue;
            memcpy(_scoreSlots[id], ev.packet.payload + off + 1, slotSize);
            _scorePresent |= (1u << id);
            changed = true;
        }
        if (changed) {
            scoreBroadcastFused(output);
            _scoreSentAt = millis();
        }

        if (_scorePresent == _expectedPlayerMask && !_scoreResultShown) {
            _scoreResultShown = true;
            postScoreAnnounce();
            scoreAnnounce();
        }
    }

    // MSG_TOTEM_BEACON: the game is over, so do NOT answer beacons with a role
    // activation here — doing so would immediately re-activate any totem that
    // just reverted to its stateless state, bouncing it back into game mode.
    // Instead we periodically re-broadcast MSG_TOTEM_ROSTER (below) to drive all
    // totems back to stateless; reverted totems ignore repeats harmlessly.
    if (millis() - _rosterSentAt >= GameDefaults::PRESTART_BROADCAST_MS) {
        output.radio.broadcast(RadioMsg::MSG_TOTEM_ROSTER, nullptr, 0, 2);
        _rosterSentAt = millis();
    }

    // Timed retry — re-broadcast fused scores while waiting for all devices.
    if (!_scoreResultShown &&
        _scoreSentAt != 0 &&
        millis() - _scoreSentAt >= GameDefaults::SCORE_RETRY_MS) {
        scoreBroadcastFused(output);
        _scoreSentAt = millis();
    }

    // Timeout — show winner with whatever scores were collected if a device never responds.
    if (!_scoreResultShown && _scoreEntryAt != 0 &&
        millis() - _scoreEntryAt >= GameDefaults::SCORE_TIMEOUT_MS) {
        _scoreResultShown = true;
        postScoreAnnounce();
        scoreAnnounce();
    }

}

// The keypad half, end screen only: A held ALONE for RESTART_HOLD_MS
// restarts the device.  "Alone": a B seen at any point of the press spoils
// it — that press is the A+B menu chord, however the two keys landed.
void LightAir_GameRunner::scoreInput(const InputReport& inputs) {
    const bool aDown = keyDown(inputs, 'A');
    const bool bDown = keyDown(inputs, 'B');
    if (!_endExitReady || !aDown) {
        _restartDownAt  = 0;
        _restartSpoiled = false;
        return;
    }
    const uint32_t now = millis();
    if (_restartDownAt == 0) _restartDownAt = now ? now : 1;
    if (bDown) _restartSpoiled = true;
    if (_restartSpoiled || now - _restartDownAt < GameDefaults::RESTART_HOLD_MS) return;

    if (_game->onEnd) _game->onEnd(*_display);
    _display->update();
    esp_restart();
}

/* =========================================================
 *   SCORE COLLECTION HELPERS
 * ========================================================= */

// Fill buf with winnerVarCount × int32_t LE from winnerVars[v].value.
void LightAir_GameRunner::scoreFillSlot(uint8_t* buf) const {
    for (uint8_t v = 0; v < _game->winnerVarCount; v++) {
        int32_t val = (int32_t)*_game->winnerVars[v].value;
        memcpy(buf + v * 4, &val, 4);
    }
}

// Return true if slot a strictly beats slot b under winnerVars priority + direction.
bool LightAir_GameRunner::scoreSlotBeats(const uint8_t* a, const uint8_t* b) const {
    for (uint8_t v = 0; v < _game->winnerVarCount; v++) {
        int32_t va, vb;
        memcpy(&va, a + v * 4, 4);
        memcpy(&vb, b + v * 4, 4);
        if (_game->winnerVars[v].dir == WinnerDir::MAX) {
            if (va > vb) return true;
            if (va < vb) return false;
        } else {
            if (va < vb) return true;
            if (va > vb) return false;
        }
    }
    return false;  // all equal — not a strict win
}

// Return true if all winnerVar values are identical between a and b.
bool LightAir_GameRunner::scoreSlotsEqual(const uint8_t* a, const uint8_t* b) const {
    return memcmp(a, b, _game->winnerVarCount * 4) == 0;
}

// Build self-describing payload ([id][data…] records) and queue as a broadcast.
void LightAir_GameRunner::scoreBroadcastFused(GameOutput& output) const {
    uint8_t slotSize = _game->winnerVarCount * 4;
    uint8_t recSize  = 1 + slotSize;
    uint8_t buf[GameDefaults::RADIO_OUT_PAYLOAD];
    uint8_t off = 0;
    for (uint8_t id = 1; id < PlayerDefs::MAX_PLAYER_ID; id++) {
        if (!(_scorePresent & (1u << id))) continue;
        if (off + recSize > GameDefaults::RADIO_OUT_PAYLOAD) break;
        buf[off] = id;
        memcpy(buf + off + 1, _scoreSlots[id], slotSize);
        off += recSize;
    }
    if (off > 0)
        output.radio.broadcast(_game->scoreMsgType, buf, off, 2);
}

// Find the winner from accumulated slots and call showMessage() on the display.
// If the game provides onScoreAnnounce, delegates entirely to that callback
// (used for team-aggregate or other non-individual winner logic).
// Otherwise shows two tray lines:
//   top    — "[NAME] WINS!"  or  "TIE: [NAMES]"
//   bottom — "You arrived Xth"  (only if own ID is in the roster)
void LightAir_GameRunner::scoreAnnounce() const {
    // Delegate to game-specific announce if provided.
    if (_game->onScoreAnnounce) {
        ScoreTable table;
        table.accumMask      = _scorePresent;
        table.slots          = _scoreSlots;
        table.winnerVarCount = _game->winnerVarCount;
        table.winnerVars     = _game->winnerVars;
        table.teamMap        = _teamMap;
        table.myPlayerId     = _radio->playerId();
        _game->onScoreAnnounce(table, *_display);
        return;
    }

    // Default: individual-player ranking.
    uint8_t bestId = 0;
    bool    tied   = false;

    for (uint8_t id = 1; id < PlayerDefs::MAX_PLAYER_ID; id++) {
        if (!(_scorePresent & (1u << id))) continue;
        if (bestId == 0) {
            bestId = id;
        } else if (scoreSlotBeats(_scoreSlots[id], _scoreSlots[bestId])) {
            bestId = id;
            tied   = false;
        } else if (scoreSlotsEqual(_scoreSlots[id], _scoreSlots[bestId])) {
            tied = true;
        }
    }

    // --- Winner / tie message ---
    char msg[32];
    if (bestId == 0) {
        snprintf(msg, sizeof(msg), "No scores!");
    } else if (!tied) {
        snprintf(msg, sizeof(msg), "%s WINS!", PlayerDefs::playerShort[bestId]);
    } else {
        // Collect tied participant short-names, space-separated.
        char    names[24] = {};
        uint8_t off       = 0;
        for (uint8_t id = 1; id < PlayerDefs::MAX_PLAYER_ID && off < 20; id++) {
            if (!(_scorePresent & (1u << id))) continue;
            if (scoreSlotsEqual(_scoreSlots[id], _scoreSlots[bestId])) {
                if (off) names[off++] = ' ';
                memcpy(names + off, PlayerDefs::playerShort[id], 3);
                off += 3;
            }
        }
        snprintf(msg, sizeof(msg), "TIE: %s", names);
    }

    // --- Own position ---
    uint8_t myId = _radio->playerId();
    if (_scorePresent & (1u << myId)) {
        uint8_t rank = 1;
        for (uint8_t id = 1; id < PlayerDefs::MAX_PLAYER_ID; id++) {
            if (id == myId || !(_scorePresent & (1u << id))) continue;
            if (scoreSlotBeats(_scoreSlots[id], _scoreSlots[myId])) rank++;
        }
        const char* sfx = (rank == 1) ? "st" : (rank == 2) ? "nd"
                        : (rank == 3) ? "rd" : "th";
        char pos[24];
        snprintf(pos, sizeof(pos), "You arrived %u%s", rank, sfx);
        _display->showMessage(pos, 0);
    }

    _display->showMessage(msg, 0);
}

// Arm the hold-A restart after winner announcement.
// The GAME_END binding set remains active so monitor vars stay visible
// alongside the tray messages produced by scoreAnnounce().
void LightAir_GameRunner::postScoreAnnounce() {
    _display->showMessage("Hold A: Restart");
    _endExitReady = true;
}

/* =========================================================
 *   HELPERS
 * ========================================================= */

void LightAir_GameRunner::activateStateDisplay(uint8_t state) {
    for (uint8_t b = 0; b < _bindingCount; b++) {
        if (_bindings[b].state == state) {
            _display->activateBindingSet(_bindings[b].setId);
            return;
        }
    }
    // No binding set for this state — display left unchanged.
}

void LightAir_GameRunner::flushOutput(const GameOutput& out) {
    // Projector optics, before anything else: reconfiguring Enlight while a
    // measurement is in flight corrupts it, and this is the point in the cycle
    // where nothing has been started yet.  Values arrive already clamped by
    // the la.shine_config verb.  Held, the tool owns the device: keep what
    // the ruleset asked for and apply it when the hold ends.
    if (_held) {
        if (out.optics.hasCycles)   _heldOptics.setCycles(out.optics.cycles);
        if (out.optics.hasCooldown) _heldOptics.setCooldown(out.optics.cooldownMs);
    } else if (enlightPtr) {
        if (out.optics.hasCycles)   enlightPtr->setRepetitions(out.optics.cycles);
        if (out.optics.hasCooldown) enlightPtr->setCooldown((int64_t)out.optics.cooldownMs);
    }

    flushRadio(out);

    // UI events (skipped if no UICtrl was provided)
    if (!_ui) return;
    for (uint8_t i = 0; i < out.ui.count; i++) {
        const UIOutMsg& m = out.ui.msgs[i];
        if (m.event == LightAir_UICtrl::UIEvent::Enlight)
            _ui->triggerEnlight(m.enlightMs);
        else
            _ui->trigger(m.event);
    }
}

void LightAir_GameRunner::flushRadio(const GameOutput& out) {
    // Radio messages
    for (uint8_t i = 0; i < out.radio.count; i++) {
        const RadioOutMsg& m = out.radio.msgs[i];
        if (m.isBroadcast)
            _radio->broadcast(m.msgType, m.payload, m.payloadLen, m.resend);
        else
            _radio->sendTo(m.targetId, m.msgType, m.payload, m.payloadLen, m.resend);
    }

    // Radio replies
    for (uint8_t i = 0; i < out.radio.replyCount; i++) {
        const RadioReplyMsg& r = out.radio.replies[i];
        if (r.payloadLen)
            _radio->replyTo(r.senderId, r.origMsgType, r.origTimestamp, r.payload, r.payloadLen);
        else
            _radio->replyTo(r.senderId, r.origMsgType, r.origTimestamp);
    }
}
