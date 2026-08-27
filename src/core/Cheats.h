#pragma once

// Cheat functions defined in Pad.cpp. They are plain file-scope globals there;
// declaring them here lets other translation units (e.g. the gamepad cheat-
// combo input in src/skel/gbm/cheat_input.cpp) invoke them by pointer.
//
// Only the unconditionally-compiled cheats are declared. Cheats guarded by
// build options (KANGAROO_CHEAT, ALLCARSHELI_CHEAT, ALT_DODO_CHEAT) are left
// out so this header has no build-flag dependencies.

void WeaponCheat();
void HealthCheat();
void TankCheat();
void BlowUpCarsCheat();
void ChangePlayerCheat();
void MayhemCheat();
void EverybodyAttacksPlayerCheat();
void WeaponsForAllCheat();
void FastTimeCheat();
void SlowTimeCheat();
void MoneyCheat();
void ArmourCheat();
void WantedLevelUpCheat();
void WantedLevelDownCheat();
void SunnyWeatherCheat();
void CloudyWeatherCheat();
void RainyWeatherCheat();
void FoggyWeatherCheat();
void FastWeatherCheat();
void OnlyRenderWheelsCheat();
void ChittyChittyBangBangCheat();
void StrongGripCheat();
void NastyLimbsCheat();
