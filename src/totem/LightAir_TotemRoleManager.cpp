#include "LightAir_TotemRoleManager.h"

bool LightAir_TotemRoleManager::registerRole(const TotemRole& role) {
    if (_count >= TotemDefs::MAX_TOTEM_ROLES) return false;
    if (role.roleId == 0)                     return false;  // NONE is not registerable
    for (uint8_t i = 0; i < _count; i++) {
        if (_roles[i].roleId == role.roleId)  return false;  // duplicate
    }
    _roles[_count++] = role;
    return true;
}

const TotemRole* LightAir_TotemRoleManager::findById(uint8_t roleId) const {
    for (uint8_t i = 0; i < _count; i++) {
        if (_roles[i].roleId == roleId) return &_roles[i];
    }
    return nullptr;
}
