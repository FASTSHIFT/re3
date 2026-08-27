/*
 * cheat_input.cpp - gamepad button-combo cheat codes (docs/18).
 *
 * Loads cheats.ini once, mapping controller button combinations to the engine's
 * built-in cheat functions, and fires them on the rising edge of a combo while
 * a modifier button is held. Hooks the evdev gamepad sampling path.
 *
 * Active in RE3_CHEATS builds only.
 */
#if defined RW_GL3 && defined LIBRW_GBM && defined RE3_CHEATS

#include "cheat_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "common.h"
#include "Pad.h"
#include "Cheats.h"

// ---- key name <-> bit -----------------------------------------------------
enum {
#define CHEAT_KEY(name, bit) CK_##name = (1u << (bit)),
#include "cheat_keys.def"
#undef CHEAT_KEY
	CK_NONE = 0u
};

struct KeyName {
	const char *name;
	uint32 bit;
};
static const KeyName kKeyNames[] = {
#define CHEAT_KEY(name, bit) {#name, (1u << (bit))},
#include "cheat_keys.def"
#undef CHEAT_KEY
};

// Analogue trigger press threshold (L2/R2 are 0..255).
#define CHEAT_TRIGGER_THRESH 200

// ---- cheat name -> function ----------------------------------------------
struct CheatEntry {
	const char *name;
	void (*fn)(void);
};
static const CheatEntry kCheats[] = {
    {"weapons", WeaponCheat},
    {"health", HealthCheat},
    {"armour", ArmourCheat},
    {"money", MoneyCheat},
    {"tank", TankCheat},
    {"blowupcars", BlowUpCarsCheat},
    {"changeplayer", ChangePlayerCheat},
    {"mayhem", MayhemCheat},
    {"everybodyattacks", EverybodyAttacksPlayerCheat},
    {"weaponsforall", WeaponsForAllCheat},
    {"fasttime", FastTimeCheat},
    {"slowtime", SlowTimeCheat},
    {"wanted_up", WantedLevelUpCheat},
    {"wanted_down", WantedLevelDownCheat},
    {"sunny", SunnyWeatherCheat},
    {"cloudy", CloudyWeatherCheat},
    {"rainy", RainyWeatherCheat},
    {"foggy", FoggyWeatherCheat},
    {"fastweather", FastWeatherCheat},
    {"wheelsonly", OnlyRenderWheelsCheat},
    {"alldodos", ChittyChittyBangBangCheat},
    {"stronggrip", StrongGripCheat},
    {"nastylimbs", NastyLimbsCheat},
};

// ---- loaded mappings ------------------------------------------------------
#define CHEAT_MAX_MAPPINGS 64
#define CHEAT_MAX_STEPS 12         // longest supported sequence (GTA-style combos fit)
#define CHEAT_SEQ_TIMEOUT_MS 2000u // idle gap that resets a partial sequence

// A mapping is a SEQUENCE of steps. Each step is a key combo (bits pressed
// together). A one-step mapping is just a plain combo; multi-step mappings
// (config uses ',' between steps) are pressed in order, so the same key can
// repeat across steps (e.g. "CROSS,CROSS,UP"). The modifier must stay held for
// the whole sequence.
struct CheatMapping {
	uint32 steps[CHEAT_MAX_STEPS]; // per-step required key bits
	int nSteps;
	void (*fn)(void); // cheat to run
	// runtime sequence progress
	int progress;     // how many steps matched so far
	uint32 prevStep;  // key bits that satisfied the current step last frame (edge)
	uint32 lastAdvMs; // time of last successful step advance (for timeout)
};

static CheatMapping sMappings[CHEAT_MAX_MAPPINGS];
static int sNumMappings = 0;
static uint32 sModifierBit = CK_CREATE; // default modifier: Create button
static bool sInited = false;
static bool sEnabled = false;

// ---- helpers --------------------------------------------------------------
#include <time.h>
static uint32
mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static uint32
key_bit_by_name(const char *tok)
{
	for(size_t i = 0; i < ARRAY_SIZE(kKeyNames); i++)
		if(strcmp(kKeyNames[i].name, tok) == 0) return kKeyNames[i].bit;
	return CK_NONE;
}

static void (*cheat_fn_by_name(const char *name))(void)
{
	for(size_t i = 0; i < ARRAY_SIZE(kCheats); i++)
		if(strcmp(kCheats[i].name, name) == 0) return kCheats[i].fn;
	return nil;
}

// Uppercase + strip leading/trailing spaces in place.
static char *
trim_upper(char *s)
{
	while(*s == ' ' || *s == '\t') s++;
	char *end = s + strlen(s);
	while(end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = '\0';
	for(char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p);
	return s;
}

// Parse a "A+B+C" key list into a bitmask. Returns CK_NONE on unknown token.
static uint32
parse_combo(char *list)
{
	uint32 mask = 0;
	char *save = nil;
	for(char *tok = strtok_r(list, "+", &save); tok; tok = strtok_r(nil, "+", &save)) {
		char *t = trim_upper(tok);
		if(*t == '\0') continue;
		uint32 bit = key_bit_by_name(t);
		if(bit == CK_NONE) {
			printf("cheats: unknown key '%s', ignoring line\n", t);
			return CK_NONE;
		}
		mask |= bit;
	}
	return mask;
}

// ---- config parsing -------------------------------------------------------
static void
parse_line(char *line)
{
	// strip comments (# or ;) and skip blanks
	for(char *p = line; *p; p++) {
		if(*p == '#' || *p == ';') {
			*p = '\0';
			break;
		}
	}
	char *eq = strchr(line, '=');
	if(eq == nil) return; // no key=value -> ignore
	*eq = '\0';
	char *lhs = line;
	char *rhs = eq + 1;

	// modifier = KEY
	char lhsUp[64];
	strncpy(lhsUp, lhs, sizeof(lhsUp) - 1);
	lhsUp[sizeof(lhsUp) - 1] = '\0';
	trim_upper(lhsUp);
	if(strcmp(lhsUp, "MODIFIER") == 0) {
		char rhsUp[64];
		strncpy(rhsUp, rhs, sizeof(rhsUp) - 1);
		rhsUp[sizeof(rhsUp) - 1] = '\0';
		trim_upper(rhsUp);
		uint32 combo = parse_combo(rhsUp);
		if(combo != CK_NONE) sModifierBit = combo;
		return;
	}

	// <step[,step...]> = cheatname
	if(sNumMappings >= CHEAT_MAX_MAPPINGS) {
		printf("cheats: too many mappings (max %d), ignoring rest\n", CHEAT_MAX_MAPPINGS);
		return;
	}

	// Split the LHS into steps on ',' then parse each step as a combo. A single
	// step (no comma) is a plain combo; multiple steps form an ordered sequence
	// where the same key may repeat across steps.
	uint32 steps[CHEAT_MAX_STEPS];
	int nSteps = 0;
	char *seqSave = nil;
	for(char *step = strtok_r(lhs, ",", &seqSave); step; step = strtok_r(nil, ",", &seqSave)) {
		if(nSteps >= CHEAT_MAX_STEPS) {
			printf("cheats: sequence too long (max %d steps), ignoring line\n", CHEAT_MAX_STEPS);
			return;
		}
		uint32 combo = parse_combo(step);
		if(combo == CK_NONE) return; // unknown key already warned
		steps[nSteps++] = combo;
	}
	if(nSteps == 0) return;

	char rhsLow[64];
	strncpy(rhsLow, rhs, sizeof(rhsLow) - 1);
	rhsLow[sizeof(rhsLow) - 1] = '\0';
	// trim (keep case-insensitive by lowercasing) the cheat name
	char *r = rhsLow;
	while(*r == ' ' || *r == '\t') r++;
	char *rend = r + strlen(r);
	while(rend > r && (rend[-1] == ' ' || rend[-1] == '\t' || rend[-1] == '\r' || rend[-1] == '\n')) *--rend = '\0';
	for(char *p = r; *p; p++) *p = (char)tolower((unsigned char)*p);

	void (*fn)(void) = cheat_fn_by_name(r);
	if(fn == nil) {
		printf("cheats: unknown cheat '%s', ignoring line\n", r);
		return;
	}

	CheatMapping &m = sMappings[sNumMappings];
	for(int i = 0; i < nSteps; i++) m.steps[i] = steps[i];
	m.nSteps = nSteps;
	m.fn = fn;
	m.progress = 0;
	m.prevStep = 0;
	m.lastAdvMs = 0;
	sNumMappings++;
}

// ---- public ---------------------------------------------------------------
void
CheatInput_Init(void)
{
	if(sInited) return;
	sInited = true;

	const char *path = getenv("RE3_CHEATS_FILE");
	if(path == nil || *path == '\0') path = "cheats.ini";

	FILE *f = fopen(path, "r");
	if(f == nil) {
		// No config -> feature silently disabled.
		return;
	}

	char line[256];
	while(fgets(line, sizeof(line), f)) parse_line(line);
	fclose(f);

	// Fold the modifier into every step so a combo/sequence is only recognised
	// while the modifier is also held for its whole duration (avoids accidents).
	for(int i = 0; i < sNumMappings; i++)
		for(int j = 0; j < sMappings[i].nSteps; j++) sMappings[i].steps[j] |= sModifierBit;

	sEnabled = sNumMappings > 0;
	printf("cheats: %d mapping(s) loaded from %s%s\n", sNumMappings, path, sEnabled ? "" : " (disabled: none valid)");
}

bool
CheatInput_Process(const CControllerState &s, bool inMenu)
{
	if(!sEnabled || inMenu) {
		// Reset all sequence progress so re-entry doesn't resume mid-combo.
		for(int i = 0; i < sNumMappings; i++) {
			sMappings[i].progress = 0;
			sMappings[i].prevStep = 0;
		}
		return false;
	}

	// Build the current key bitmask from the sampled controller state.
	uint32 bits = 0;
	if(s.Cross) bits |= CK_CROSS;
	if(s.Circle) bits |= CK_CIRCLE;
	if(s.Triangle) bits |= CK_TRIANGLE;
	if(s.Square) bits |= CK_SQUARE;
	if(s.LeftShoulder1) bits |= CK_L1;
	if(s.RightShoulder1) bits |= CK_R1;
	if(s.LeftShoulder2 > CHEAT_TRIGGER_THRESH) bits |= CK_L2;
	if(s.RightShoulder2 > CHEAT_TRIGGER_THRESH) bits |= CK_R2;
	if(s.LeftShock) bits |= CK_L3;
	if(s.RightShock) bits |= CK_R3;
	if(s.Select) bits |= CK_CREATE;
	if(s.Start) bits |= CK_OPTIONS;
	if(s.DPadUp) bits |= CK_UP;
	if(s.DPadDown) bits |= CK_DOWN;
	if(s.DPadLeft) bits |= CK_LEFT;
	if(s.DPadRight) bits |= CK_RIGHT;

	bool modifierHeld = (bits & sModifierBit) == sModifierBit;
	uint32 now = mono_ms();
	bool inCombo = false;

	// Releasing the modifier aborts every in-progress sequence.
	if(!modifierHeld) {
		for(int i = 0; i < sNumMappings; i++) {
			sMappings[i].progress = 0;
			sMappings[i].prevStep = 0;
		}
		return false;
	}
	inCombo = true; // modifier held => suppress its normal injection

	for(int i = 0; i < sNumMappings; i++) {
		CheatMapping &m = sMappings[i];

		// Timeout: too long since the last advance -> restart this sequence.
		if(m.progress > 0 && (uint32)(now - m.lastAdvMs) > CHEAT_SEQ_TIMEOUT_MS) {
			m.progress = 0;
			m.prevStep = 0;
		}

		uint32 want = m.steps[m.progress];
		bool stepNow = (bits & want) == want;

		// Rising edge of the current step's combo: it is satisfied now but was
		// NOT satisfied last frame. prevStep records last frame's satisfaction
		// so holding the keys down only counts as ONE press (no auto-repeat).
		bool rising = stepNow && !m.prevStep;

		if(rising) {
			m.progress++;
			m.lastAdvMs = now;
			if(m.progress >= m.nSteps) {
				m.fn();         // whole sequence complete: fire once
				m.progress = 0; // rearm for a fresh sequence...
				// ...but require a release first: keep prevStep marked so the
				// still-held keys are not read as a new step-0 press next frame.
				m.prevStep = 1;
				continue;
			}
		}
		// Remember whether this step's combo is currently held, so next frame's
		// rising-edge test works and a held combo can't re-advance.
		m.prevStep = stepNow ? 1 : 0;
	}

	return inCombo;
}

#endif
