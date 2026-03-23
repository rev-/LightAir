#pragma once
#include "../game/LightAir_GameManager.h"

// Registers all games defined in AllGames.cpp into the manager.
// Defined in AllGames.cpp; include this header in sketches that
// need to populate a LightAir_GameManager.
void registerAllGames(LightAir_GameManager& mgr);
