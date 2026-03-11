#pragma once

// Configuration & calibration
#include "config.h"
#include "nvs_config.h"

// Optical hit detection
#include "enlight/Enlight.h"

// Radio
#include "radio/LightAir_Radio.h"
#include "radio/LightAir_RadioESPNow.h"

// UI — display
#include "ui/display/LightAir_DisplayCtrl.h"
#include "ui/display/LightAir_SSD1306Display.h"

// UI — audio
#include "ui/audio/LightAir_BuzzerAudio.h"

// UI — RGB
#include "ui/rgb/LightAir_RGB_HW.h"

// UI — vibration
#include "ui/vib/LightAir_MotorVibration.h"

// UI — controller & event observer
#include "ui/LightAir_UICtrl.h"
#include "ui/LightAir_UIEventObserver.h"

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
#include "game/LightAir_TotemRole.h"
#include "game/LightAir_Game.h"
#include "game/LightAir_GameRunner.h"
#include "game/LightAir_GameManager.h"
#include "game/LightAir_GameConfigMenu.h"
