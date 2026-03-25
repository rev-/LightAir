#include "AllTotems.h"
#include "../totem-rulesets/TotemRoleIds.h"
#include "LightAir_TotemRunner.h"

// Runner singletons are defined as file-scope objects in their
// respective totem-ruleset .cpp files and exposed here as plain
// pointers so AllTotems.cpp needs no concrete-class knowledge.
extern LightAir_TotemRunner* totemRunner_baseO;
extern LightAir_TotemRunner* totemRunner_baseX;
extern LightAir_TotemRunner* totemRunner_flagO;
extern LightAir_TotemRunner* totemRunner_flagX;
extern LightAir_TotemRunner* totemRunner_cp;
extern LightAir_TotemRunner* totemRunner_bonus;
extern LightAir_TotemRunner* totemRunner_malus;

void registerAllTotems(LightAir_TotemRoleManager& mgr) {
    mgr.registerRole({ TotemRoleId::BASE_O, "BASE_O", 0,    totemRunner_baseO });
    mgr.registerRole({ TotemRoleId::BASE_X, "BASE_X", 1,    totemRunner_baseX });
    mgr.registerRole({ TotemRoleId::FLAG_O, "FLAG_O", 0,    totemRunner_flagO });
    mgr.registerRole({ TotemRoleId::FLAG_X, "FLAG_X", 1,    totemRunner_flagX });
    mgr.registerRole({ TotemRoleId::CP,     "CP",     0xFF, totemRunner_cp    });
    mgr.registerRole({ TotemRoleId::BONUS,  "BONUS",  0xFF, totemRunner_bonus });
    mgr.registerRole({ TotemRoleId::MALUS,  "MALUS",  0xFF, totemRunner_malus });
}
