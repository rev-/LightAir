#include "LightAir_TotemDriver.h"
using RadioMsg::MSG_TOTEM_BEACON;
using RadioMsg::MSG_TOTEM_ROSTER;
using RadioMsg::MSG_TOTEM_TOUCH;

// ----------------------------------------------------------------
LightAir_TotemDriver::LightAir_TotemDriver(LightAir_Radio&       radio,
                                            LightAir_TotemUICtrl& ui)
    : _radio(radio), _ui(ui),
      _runner(nullptr), _roleId(0), _lastBeacon(0), _revertDeadline(0)
{}

// ----------------------------------------------------------------
bool LightAir_TotemDriver::begin() {
    if (!_radio.begin()) return false;
    // Start the Idle background animation immediately.
    LightAir_TotemOutput boot;
    boot.ui.trigger(TotemUIEvent::Idle);
    _ui.apply(boot.ui);
    return true;
}

// ----------------------------------------------------------------
void LightAir_TotemDriver::loop() {
    const RadioReport& report = _radio.poll();
    LightAir_TotemOutput out;

    // ---- 1. Periodic beacon broadcast ----
    // Payload advertises this firmware's capabilities so a host can check
    // compatibility (S4c) before assigning a role:
    //   payload[0] = Lua game api version, payload[1] = TotemVM version.
    uint32_t now = millis();
    if ((now - _lastBeacon) >= GameDefaults::TOTEM_BEACON_INTERVAL_MS) {
        _lastBeacon = now;
        uint8_t caps[2] = { LuaDefaults::API_VERSION, TotemVMDefs::VERSION };
        _radio.broadcastUniversal(MSG_TOTEM_BEACON, caps, sizeof(caps), 2);
    }

    // ---- 2. Self-revert watchdog: no MSG_TOTEM_ROSTER ever arrived ----
    if (_runner && _revertDeadline && now >= _revertDeadline) {
        revertToIdle(out);
    }

    // ---- 3. Process incoming events ----
    bool touchAnimated = false;
    for (uint8_t i = 0; i < report.count; i++) {
        const RadioEvent& ev = report.events[i];

        // Determine which packet carries the game typeId.
        // ReplyReceived: the reply packet (ev.packet) has the sender's typeId.
        // MessageReceived: the packet itself has the sender's typeId.
        uint16_t incomingTypeId = ev.packet.typeId;

        // MSG_TOTEM_ROSTER is universal; it signals end-of-game and resets the runner.
        bool isRoster = (ev.type == RadioEventType::MessageReceived &&
                         ev.packet.msgType == MSG_TOTEM_ROSTER);

        if (isRoster) {
            if (_runner) {
                _runner->onRoster(ev.packet, out);
                revertToIdle(out);
            }
            continue;
        }

        // Activate on the first 0xF1 activation reply, which carries the
        // whole behaviour as a TotemVM program
        // (docs/totem-behavior-handshake.md):
        //   [role][session][timeleft2][vmVer][len16][program…]
        if (!_runner && incomingTypeId != RadioTypeId::UNIVERSAL) {
            if (ev.packet.msgType == (RadioMsg::MSG_TOTEM_BEACON + 1) &&
                ev.packet.payloadLen >= 7 &&
                ev.packet.payload[4] == TotemVMDefs::VERSION) {
                LightAir_TotemActivation info;
                info.roleId           = ev.packet.payload[0];
                info.sessionToken     = ev.packet.payload[1];
                info.gameTimeLeftSecs = ((uint16_t)ev.packet.payload[2] << 8) |
                                         ev.packet.payload[3];
                info.hasConfigSecs    = false;
                info.configSecs       = 0;
                // Own logical id, so a program can match notifies addressed to it.
                info.selfId           = _radio.playerId();

                uint16_t progLen = (uint16_t)(ev.packet.payload[5] |
                                              (ev.packet.payload[6] << 8));
                // A malformed program leaves the totem IDLE (fail loud on
                // the host side, harmless here).
                if (progLen == (uint16_t)(ev.packet.payloadLen - 7) &&
                    _vm.load(ev.packet.payload + 7, progLen)) {
                    _runner = &_vm;
                    _roleId = info.roleId;
                    _radio.setTypeId(incomingTypeId);
                    _radio.setSessionToken(info.sessionToken);
                    _revertDeadline = (info.gameTimeLeftSecs == 0xFFFF)
                        ? 0
                        : now + (uint32_t)(info.gameTimeLeftSecs + 10) * 1000;
                    _runner->onActivate(info, out);
                }
            }
            continue;
        }

        // A touch is the driver's, never the program's.
        if (_runner && ev.type == RadioEventType::MessageReceived &&
            ev.packet.msgType == MSG_TOTEM_TOUCH) {
            handleTouch(ev, out, touchAnimated);
            continue;
        }

        // Forward to the VM (RSSI-aware: the rssi guard needs the
        // receive-side signal strength).
        if (_runner) {
            _vm.onPacket(ev.packet, ev.rssi, out);
        }
    }

    // ---- 4. Periodic runner update ----
    if (_runner) {
        _runner->update(out);
    }

    // ---- 5. Flush output ----
    flushOutput(out);

    // ---- 6. Advance strip animation ----
    _ui.update();
}

// ----------------------------------------------------------------
// MSG_TOTEM_TOUCH: proximity is judged here, on this totem's own reading of
// the touch, so it works for every role in every state — a pickup in its
// cooldown is silent, but it still hears.  Only ACK is the driver's; the
// codes above it are reserved for the program, and ignored until TotemVM
// can take them.
void LightAir_TotemDriver::handleTouch(const RadioEvent& ev, LightAir_TotemOutput& out,
                                       bool& animated) {
    const RadioPacket& t = ev.packet;
    if (t.payloadLen < 2) return;
    const uint8_t gate   = t.payload[0];
    const uint8_t action = t.payload[1];
    if (action != TotemTouch::ACK) return;
    if (gate > 0 && ev.rssi < -(int)gate) return;

    // The chaser yields to whatever the role is animating, and to another
    // touch's chaser: one arrival at a time, never a queue of them.
    if (!animated && !_ui.busy()) {
        const uint8_t pid = (t.senderId < PlayerDefs::MAX_PLAYER_ID) ? t.senderId : 0;
        out.ui.trigger(TotemUIEvent::Respawn, PlayerColors::kColors[pid][0],
                       PlayerColors::kColors[pid][1], PlayerColors::kColors[pid][2]);
        animated = true;
    }
    const uint8_t reply[2] = { action, _roleId };
    out.radio.replyWithPayload(t, reply, sizeof(reply));
}

// ----------------------------------------------------------------
void LightAir_TotemDriver::revertToIdle(LightAir_TotemOutput& out) {
    _runner->reset();
    _runner = nullptr;
    _radio.setTypeId(RadioTypeId::UNIVERSAL);
    _radio.setSessionToken(RadioToken::UNSET);
    _revertDeadline = 0;
    out.ui.trigger(TotemUIEvent::Idle);
}

// ----------------------------------------------------------------
void LightAir_TotemDriver::flushOutput(LightAir_TotemOutput& out) {
    // ---- Radio: queued broadcasts and unicasts ----
    for (uint8_t i = 0; i < out.radio.count; i++) {
        const RadioOutMsg& m = out.radio.msgs[i];
        if (m.isBroadcast)
            _radio.broadcast(m.msgType, m.payload, m.payloadLen, m.resend);
        else
            _radio.sendTo(m.targetId, m.msgType, m.payload, m.payloadLen, m.resend);
    }
    // ---- Radio: queued replies ----
    for (uint8_t i = 0; i < out.radio.replyCount; i++) {
        const RadioReplyMsg& r = out.radio.replies[i];
        if (r.payloadLen > 0) {
            _radio.replyTo(r.senderId, r.origMsgType, r.origTimestamp, r.payload, r.payloadLen);
        } else {
            _radio.replyTo(r.senderId, r.origMsgType, r.origTimestamp);
        }
    }
    // ---- UI events ----
    _ui.apply(out.ui);
}
