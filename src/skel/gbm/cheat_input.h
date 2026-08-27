/*
 * cheat_input.h - gamepad button-combo cheat codes for the GBM skeleton.
 *
 * Maps controller button combinations (loaded from cheats.ini) to the engine's
 * built-in cheat functions (src/core/Cheats.h), so cheats can be triggered on a
 * PS5/evdev pad without typing the PC letter strings. See docs/18.
 *
 * Active only in RE3_CHEATS builds. The evdev gamepad source calls:
 *   CheatInput_Init()  once at startup (parses cheats.ini)
 *   CheatInput_Process(state, inMenu) each frame after sampling the pad
 */
#ifndef RE3_CHEAT_INPUT_H
#define RE3_CHEAT_INPUT_H

#ifdef RE3_CHEATS

class CControllerState;

// Parse cheats.ini (path overridable via RE3_CHEATS_FILE). Safe to call more
// than once; only the first call does work. Missing/empty file -> disabled.
void
CheatInput_Init(void);

// Per-frame combo detection. Pass the freshly sampled pad state and whether the
// frontend menu is active. Fires a cheat on the rising edge of a configured
// combo (modifier held + all keys down, not satisfied last frame).
// Returns true if the modifier participated in a cheat combo this frame, so the
// caller can suppress the modifier's normal button injection.
bool
CheatInput_Process(const CControllerState &state, bool inMenu);

#endif /* RE3_CHEATS */
#endif /* RE3_CHEAT_INPUT_H */
