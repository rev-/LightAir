#ifndef LIGHTAIR_UIEVENTOBSERVER_H
#define LIGHTAIR_UIEVENTOBSERVER_H

#include <stdint.h>

/*
 * Observer interface for UI events.
 *
 * Called exactly once when an event starts execution.
 * Not called on enqueue.
 * Not called per-step.
 */
class LightAir_UIEventObserver {
public:
  virtual ~LightAir_UIEventObserver() {}

  virtual void onEventStarted(
    uint8_t eventId,
    uint8_t priority,
    uint8_t stepCount
  ) = 0;
};

#endif
