#include "LightAir_UICtrl.h"


// ================= STATIC TABLE =================

// ---------------- ACTION TABLE ----------------

const LightAir_UICtrl::UIAction
LightAir_UICtrl::_actionTable[(uint8_t)UIEvent::Count] = {
  // Enlight - only 1 step, duration modifiable via extraSteps
  { {10,0,0,0}, 1, {2200,0,0,0}, {120,0,0,0}, { {0,0,255},{0,0,0},{0,0,0},{0,0,0} }, "", 0, 1 },

  // Lit
  { {150,0,0,0}, 1, {4000,0,0,0}, {200,0,0,0}, { {0,255,0},{0,0,0},{0,0,0},{0,0,0} }, "LIT", 1500, 2 },

  // Down
  { {120,120,120,300}, 4, {4000,3200,2400,1800}, {200,180,160,0}, { {255,0,0},{200,0,0},{120,0,0},{0,0,0} }, "SHONE", 2000, 2 },

  // Up
  { {120,120,120,300}, 4, {1800,2400,3200,4000}, {160,180,200,0}, { {0,0,120},{0,0,200},{0,0,255},{0,0,0} }, "UP", 2000, 2 },

  // EndGame
  { {600,1200,0,0}, 2, {5200,4200,0,0}, {255,255,0,0}, { {255,0,0},{255,255,0},{0,0,0},{0,0,0} }, "GAME OVER", 5000, 5 },

  // FlagGain
  {{100,100,100,0},3,{3000,3500,4000,0},{120,160,200,0},
  {{0,0,255},{0,255,255},{255,255,255},{0,0,0}},
  "FLAG +",2000,3},

  // FlagTaken
  {{200,0,0,0},1,{1000,0,0,0},{255,0,0,0},
  {{255,0,0},{0,0,0},{0,0,0},{0,0,0}},
  "FLAG LOST",2000,3},

  // FlagReturn
  {{120,120,0,0},2,{3000,3500,0,0},{100,200,0,0},
  {{0,255,0},{255,255,255},{0,0,0},{0,0,0}},
  "FLAG BACK",2000,3},

  // ControlGain
  {{100,100,0,0},2,{2500,3000,0,0},{150,200,0,0},
  {{0,255,128},{255,255,255},{0,0,0},{0,0,0}},
  "CONTROL +",2000,3},

  // ControlLoss
  {{150,0,0,0},1,{1200,0,0,0},{255,0,0,0},
  {{255,0,0},{0,0,0},{0,0,0},{0,0,0}},
  "CONTROL -",2000,3},

  // RoleChange
  {{100,100,100,0},3,{2000,2500,3000,0},{120,160,200,0},
  {{255,0,255},{0,255,255},{255,255,255},{0,0,0}},
  "ROLE",2000,2},

  // Stop
  {{0,0,0,0},0,{0,0,0,0},{0,0,0,0},
  {{0,0,0},{0,0,0},{0,0,0},{0,0,0}},
  "STOP",0,1},

  // Bonus
  {{80,80,80,80},4,{3000,3500,4000,4500},{120,150,180,200},
  {{0,255,0},{128,255,0},{255,255,0},{255,255,255}},
  "BONUS",2000,2},

  // Malus
  {{150,150,0,0},2,{800,600,0,0},{255,200,0,0},
  {{255,0,0},{128,0,0},{0,0,0},{0,0,0}},
  "MALUS",2000,2},

  // Special1
  {{200,200,200,0},3,{4000,3000,2000,0},{150,150,150,0},
  {{0,0,255},{0,255,0},{255,0,0},{0,0,0}},
  "SPECIAL1",2000,4},

  // Special2
  {{100,200,100,0},3,{1500,3500,1500,0},{200,255,200,0},
  {{255,255,0},{0,255,255},{255,0,255},{0,0,0}},
  "SPECIAL2",2000,4},

  // Custom1
  {{0,0,0,0},0,{0,0,0,0},{0,0,0,0},
  {{0,0,0},{0,0,0},{0,0,0},{0,0,0}},
  "CUSTOM1",0,2},

  // Custom2
  {{0,0,0,0},0,{0,0,0,0},{0,0,0,0},
  {{0,0,0},{0,0,0},{0,0,0},{0,0,0}},
  "CUSTOM2",0,2},

  // Custom3
  {{0,0,0,0},0,{0,0,0,0},{0,0,0,0},
  {{0,0,0},{0,0,0},{0,0,0},{0,0,0}},
  "CUSTOM3",0,2},

  // Custom4
  {{0,0,0,0},0,{0,0,0,0},{0,0,0,0},
  {{0,0,0},{0,0,0},{0,0,0},{0,0,0}},
  "CUSTOM4",0,2},
};

// ================= CONSTRUCTOR =================

LightAir_UICtrl::LightAir_UICtrl(
  LightAir_Audio& audio,
  LightAir_Vibration& vib,
  LightAir_RGB& rgb
)
: _audio(&audio),
_vib(&vib),
_rgb(&rgb),
_observer(nullptr),
_heapSize(0),
_sequenceCounter(0),
_isRunning(false),
_currentStep(0),
_hasBackground(false),
_backgroundRunning(false)
{
  for (int i = 0; i < 4; i++)
    _customDefined[i] = false;
}

// ================= PUBLIC =================

void LightAir_UICtrl::setObserver(
  LightAir_UIEventObserver* obs)
{
  _observer = obs;
}

void LightAir_UICtrl::trigger(UIEvent event)
{
  if ((uint8_t)event >= (uint8_t)UIEvent::Count)
    return;

  applyPolicy(event, 0);
}

void LightAir_UICtrl::triggerEnlight(uint16_t ms)
{
  applyPolicy(UIEvent::Enlight, ms);
}

// ================= CUSTOM =================

void LightAir_UICtrl::defineCustomAction(
  UIEvent slot,
  const UIAction& action)
{
  uint8_t idx = (uint8_t)slot;

  if (idx < (uint8_t)UIEvent::Custom1 ||
    idx > (uint8_t)UIEvent::Custom4)
    return;

  uint8_t ci =
  idx - (uint8_t)UIEvent::Custom1;

  _customActions[ci] = action;
  _customDefined[ci] = true;
}

// ================= RESOLVE =================

const LightAir_UICtrl::UIAction&
LightAir_UICtrl::resolveAction(UIEvent event)
{
  uint8_t idx = (uint8_t)event;

  if (idx >= (uint8_t)UIEvent::Custom1 &&
    idx <= (uint8_t)UIEvent::Custom4)
  {
    uint8_t ci =
    idx - (uint8_t)UIEvent::Custom1;

    if (_customDefined[ci])
      return _customActions[ci];
  }

  return _actionTable[idx];
}

// ================= POLICY =================

void LightAir_UICtrl::applyPolicy(
  UIEvent event,
  uint16_t ms)
{
  const UIAction& action =
  resolveAction(event);

  if (action.priority == 0)
    return;

  if (action.priority == 5) {
    interruptAll();
  }

  if (action.priority == 1) {
    for (uint8_t i = 0; i < _heapSize; i++) {
      if (_heap[i].id == event)
        return;
    }
  }

  ScheduledEvent ev;
  ev.id = event;
  ev.action = &action;
  ev.extraMs = ms;
  ev.sequence = _sequenceCounter++;

  heapPush(ev);

  if (!_isRunning || _backgroundRunning) {
    if (_backgroundRunning) {
      _ticker.detach();
      if (_audio) _audio->stop();
      if (_vib)   _vib->stop();
      if (_rgb)   _rgb->setColor(0,0,0);
      _backgroundRunning = false;
    }
    startNextEvent();
  }
}

// ================= HEAP =================

bool LightAir_UICtrl::compareEvents(
  const ScheduledEvent& a,
  const ScheduledEvent& b)
{
  if (a.action->priority >
    b.action->priority)
    return true;

  if (a.action->priority ==
    b.action->priority &&
    a.sequence < b.sequence)
    return true;

  return false;
}

void LightAir_UICtrl::heapPush(
  const ScheduledEvent& ev)
{
  if (_heapSize >= MAX_QUEUE)
    return;

  _heap[_heapSize] = ev;
  heapifyUp(_heapSize);
  _heapSize++;
}

LightAir_UICtrl::ScheduledEvent
LightAir_UICtrl::heapPop()
{
  ScheduledEvent top = _heap[0];
  _heap[0] = _heap[_heapSize - 1];
  _heapSize--;
  heapifyDown(0);
  return top;
}

void LightAir_UICtrl::heapifyUp(int i)
{
  while (i > 0) {
    int parent = (i - 1) / 2;
    if (compareEvents(_heap[i],
      _heap[parent]))
    {
      ScheduledEvent tmp =
      _heap[i];
      _heap[i] = _heap[parent];
      _heap[parent] = tmp;
      i = parent;
    }
    else break;
  }
}

void LightAir_UICtrl::heapifyDown(int i)
{
  while (true) {
    int left = 2*i + 1;
    int right = 2*i + 2;
    int largest = i;

    if (left < _heapSize &&
      compareEvents(_heap[left],
                    _heap[largest]))
      largest = left;

    if (right < _heapSize &&
      compareEvents(_heap[right],
                    _heap[largest]))
      largest = right;

    if (largest != i) {
      ScheduledEvent tmp =
      _heap[i];
      _heap[i] = _heap[largest];
      _heap[largest] = tmp;
      i = largest;
    }
    else break;
  }
}

// ================= SCHEDULER =================

void LightAir_UICtrl::startNextEvent()
{
  if (_heapSize > 0) {
    _backgroundRunning = false;
    _current = heapPop();
    startEvent(_current);
    return;
  }

  if (_hasBackground) {
    _backgroundRunning = true;
    _current.action = &_backgroundAction;
    _current.extraMs = 0;
    _isRunning = true;
    _currentStep = 0;
    executeStep();
    return;
  }

  _isRunning = false;
}

void LightAir_UICtrl::startEvent(
  const ScheduledEvent& ev)
{
  _isRunning = true;
  _currentStep = 0;

  if (_observer) {
    _observer->onEventStarted(
      (uint8_t)ev.id,
      ev.action->priority,
      ev.action->stepCount);
  }

  executeStep();
}

void LightAir_UICtrl::executeStep()
{
  if (_currentStep >=
    _current.action->stepCount)
  {
    if (_backgroundRunning) {
      _currentStep = 0;
      executeStep();
      return;
    }
    finishEvent();
    return;
  }

  uint16_t duration =
  (_current.id == UIEvent::Enlight && _current.extraMs > 0)
    ? _current.extraMs
    : _current.action->durations[_currentStep];

  if (_audio)
    _audio->play(
      _current.action->soundFreqs[_currentStep]);

  if (_vib)
    _vib->vibrate(
      _current.action->vibIntensity[_currentStep]);

  if (_rgb)
    _rgb->setColor(
      _current.action->rgbColors[_currentStep][0],
      _current.action->rgbColors[_currentStep][1],
      _current.action->rgbColors[_currentStep][2]);

  _currentStep++;

  _ticker.once_ms(duration, tickerTrampoline, this);
}

void LightAir_UICtrl::finishEvent()
{
  if (_audio) _audio->stop();
  if (_vib)   _vib->stop();
  if (_rgb)   _rgb->setColor(0,0,0);

  _isRunning = false;
  startNextEvent();
}

// ================= INTERRUPT =================

void LightAir_UICtrl::interruptAll()
{
  _ticker.detach();

  if (_audio) _audio->stop();
  if (_vib)   _vib->stop();
  if (_rgb)   _rgb->setColor(0,0,0);

  _heapSize = 0;
  _isRunning = false;
  _hasBackground = false;
  _backgroundRunning = false;
}

// ================= BACKGROUND =================

void LightAir_UICtrl::setBackground(
  const UIAction& action)
{
  _backgroundAction = action;
  _hasBackground = true;

  if (!_isRunning)
    startNextEvent();
}

void LightAir_UICtrl::clearBackground()
{
  _hasBackground = false;

  if (_backgroundRunning) {
    _ticker.detach();
    if (_audio) _audio->stop();
    if (_vib)   _vib->stop();
    if (_rgb)   _rgb->setColor(0,0,0);
    _backgroundRunning = false;
    _isRunning = false;
  }
}

// ================= TICKER =================

void LightAir_UICtrl::tickerTrampoline(
  LightAir_UICtrl* self)
{
  self->executeStep();
}

