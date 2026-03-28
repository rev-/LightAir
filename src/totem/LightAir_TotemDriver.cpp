#include "LightAir_TotemDriver.h"
#include "LightAir_TotemRole.h"
using RadioMsg::MSG_TOTEM_BEACON;
using RadioMsg::MSG_TOTEM_ROSTER;

// ----------------------------------------------------------------
LightAir_TotemDriver::LightAir_TotemDriver(LightAir_Radio&            radio,
                                            LightAir_TotemUICtrl&      ui,
                                            LightAir_TotemRoleManager& roleMgr)
    : _radio(radio), _ui(ui), _roleMgr(roleMgr),
      _runner(nullptr), _lastBeacon(0)
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
    uint32_t now = millis();
    if ((now - _lastBeacon) >= GameDefaults::TOTEM_BEACON_INTERVAL_MS) {
        _lastBeacon = now;
        _radio.broadcastUniversal(MSG_TOTEM_BEACON);
    }

    // ---- 2. Process incoming events ----
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
                flushOutput(out);
                out = LightAir_TotemOutput{};  // reset for next cycle
                _runner->reset();
                _runner = nullptr;
                _radio.setTypeId(RadioTypeId::UNIVERSAL);
                // Return to idle animation.
                out.ui.trigger(TotemUIEvent::Idle);
            }
            continue;
        }

        // Activate runner on first 0xF1 activation reply.
        // payload[0] holds the roleId.
        if (!_runner && incomingTypeId != RadioTypeId::UNIVERSAL) {
            if (ev.packet.msgType == (RadioMsg::MSG_TOTEM_BEACON + 1) &&
                ev.packet.payloadLen >= 1) {
                uint8_t roleId = ev.packet.payload[0];
                const TotemRole* role = _roleMgr.findById(roleId);
                if (role && role->runner) {
                    _runner = role->runner;
                    _radio.setTypeId(incomingTypeId);
                    _runner->onActivate(ev.packet.payload, ev.packet.payloadLen, out);
                }
            }
            continue;
        }

        // Forward to active runner.
        if (_runner) {
            _runner->onMessage(ev.packet, out);
        }
    }

    // ---- 3. Periodic runner update ----
    if (_runner) {
        _runner->update(out);
    }

    // ---- 4. Flush output ----
    flushOutput(out);

    // ---- 5. Advance strip animation ----
    _ui.update();
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
