#pragma once

// Configuration & calibration
#include "config.h"
#include "nvs_config.h"

// Optical hit detection
#include "enlight/Enlight.h"

// Radio
#include "radio/LightAir_Radio.h"
#include "radio/LightAir_RadioESPNow.h"

// UI — player (display, audio, RGB, vibration, controller)
#include "ui/player/display/LightAir_DisplayCtrl.h"
#include "ui/player/display/LightAir_SSD1306Display.h"
#include "ui/player/audio/LightAir_BuzzerAudio.h"
#include "ui/player/rgb/LightAir_RGB_HW.h"
#include "ui/player/vib/LightAir_MotorVibration.h"
#include "ui/player/LightAir_UICtrl.h"
#include "ui/player/LightAir_UIEventObserver.h"

// Input
#include "input/LightAir_InputCtrl.h"
#include "input/LightAir_HWButton.h"
#include "input/LightAir_HWKeypad.h"

// Game framework
#include "game/LightAir_RadioOutput.h"
#include "game/LightAir_UIOutput.h"
#include "game/LightAir_GameOutput.h"
#include "game/LightAir_GameVar.h"
#include "game/LightAir_StateRule.h"
#include "game/LightAir_StateBehavior.h"
#include "game/LightAir_TotemVar.h"
#include "game/LightAir_TotemOutput.h"
#include "game/LightAir_TotemRunner.h"
#include "game/LightAir_Game.h"
#include "game/LightAir_GameRunner.h"
#include "game/LightAir_GameManager.h"
#include "game/LightAir_GameSetupMenu.h"

// UI — totem (RGB LED + WS2812B strip)
#include "ui/totem/rgb/LightAir_TotemRGB.h"
#include "ui/totem/strip/LightAir_LEDStrip.h"
#include "ui/totem/LightAir_TotemUIOutput.h"
#include "ui/totem/LightAir_TotemUICtrl.h"

// Totem driver and role registry
#include "totem/LightAir_TotemDriver.h"
#include "totem/LightAir_TotemRole.h"
#include "totem/LightAir_TotemRoleManager.h"
#include "totem/AllTotems.h"
#include "totem-rulesets/TotemRoleIds.h"
