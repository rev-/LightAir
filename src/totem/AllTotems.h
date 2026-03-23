#pragma once
#include "LightAir_TotemRoleManager.h"

// ----------------------------------------------------------------
// registerAllTotems — populate a TotemRoleManager with every
// built-in totem role: BASE_O/X, FLAG_O/X, CP, BONUS, MALUS.
//
// Call once at boot before constructing the TotemDriver.
// ----------------------------------------------------------------
void registerAllTotems(LightAir_TotemRoleManager& mgr);
