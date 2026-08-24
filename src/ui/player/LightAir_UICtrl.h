#ifndef LIGHTAIR_UICTRL_H
#define LIGHTAIR_UICTRL_H

#include <Arduino.h>
#include <Ticker.h>
#include "audio/LightAir_Audio.h"
#include "vib/LightAir_Vibration.h"
#include "rgb/LightAir_RGB.h"
#include "display/LightAir_DisplayCtrl.h"
#include "LightAir_UIEventObserver.h"
#include "LightAir_UIAction.h"

#define MAX_QUEUE 10

class LightAir_UICtrl
{
public:

  enum class UIEvent : uint8_t {
    Enlight,
    Lit,
    Taken,
    GotLit,
    Immune,
    Friend,
    AlreadyDown,
    Down,
    Up,
    EndGame,
    GameStart,
    FlagGain,
    FlagTaken,
    FlagReturn,
    ControlGain,
    ControlLoss,
    RoleChange,
    ProjectorChange,
    Stop,
    Bonus,
    Malus,
    Special1,
    Special2,
    Custom1,
    Custom2,
    Custom3,
    Custom4,
    Count
  };

  // Defined in LightAir_UIAction.h so lightweight value types can carry an
  // action without including this header.  Both spellings name one type.
  using UIAction = LightAir_UIAction;

  LightAir_UICtrl(
    LightAir_Audio& audio,
    LightAir_Vibration& vib,
    LightAir_RGB& rgb
  );

  void trigger(UIEvent event);
  void triggerEnlight(uint16_t ms);

  void setObserver(LightAir_UIEventObserver* obs);
  void defineCustomAction(UIEvent slot,
                          const UIAction& action);

  // Replace the action played for UIEvent::Enlight, or restore the standard
  // one by passing nullptr.  Used by LightAir_ProjectorCtrl so each projector
  // can sound different WITHOUT introducing new UIEvent values: the runtime
  // burst-duration override in executeStep() is keyed on the event *value*,
  // so a per-projector event id would silently discard the real burst length.
  void setEnlightAction(const UIAction* action);

  void setBackground(const UIAction& action);
  void clearBackground();

private:

  struct ScheduledEvent {
    UIEvent id;
    const UIAction* action;
    uint16_t extraMs;
    uint32_t sequence;
  };

  // Core engine
  void applyPolicy(UIEvent event,
                   uint16_t ms);
  void startNextEvent();
  void startEvent(const ScheduledEvent& ev);
  void executeStep();
  void finishEvent();
  void interruptAll();

  // Heap
  void heapPush(const ScheduledEvent& ev);
  ScheduledEvent heapPop();
  void heapifyUp(int index);
  void heapifyDown(int index);
  bool compareEvents(const ScheduledEvent& a,
                     const ScheduledEvent& b);

  const UIAction& resolveAction(UIEvent event);

  static void tickerTrampoline(
    LightAir_UICtrl* self);

  // Hardware
  LightAir_Audio* _audio;
  LightAir_Vibration* _vib;
  LightAir_RGB* _rgb;
  LightAir_UIEventObserver* _observer;

  // Heap queue
  ScheduledEvent _heap[MAX_QUEUE];
  uint8_t _heapSize;
  uint32_t _sequenceCounter;

  // Running event
  ScheduledEvent _current;
  bool _isRunning;
  uint8_t _currentStep;

  // Background (looping ambient alert)
  bool _hasBackground;
  bool _backgroundRunning;
  UIAction _backgroundAction;

  Ticker _ticker;

  static const UIAction
  _actionTable[(uint8_t)UIEvent::Count];

  UIAction _customActions[4];
  bool _customDefined[4];

  // Enlight-slot override (see setEnlightAction).
  UIAction _enlightAction;
  bool     _enlightDefined;
};

#endif
