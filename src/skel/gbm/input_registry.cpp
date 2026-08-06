/*
 * input_registry.cpp - multi-source input dispatch for the GBM skeleton.
 *
 * Maintains a static table of registered InputSource instances. Each source
 * file calls InputSource_Register() at startup (or gbm.cpp calls the
 * appropriate Get functions to populate). gbm.cpp then calls
 * InputSource_InitAll/PollAll/TerminateAll; CapturePad calls
 * InputSource_CapturePadAll.
 */
#if defined RW_GL3 && defined LIBRW_GBM

#include "input_source.h"
#include <stdio.h>
#include <string.h>

static InputSource *sTable[INPUT_SOURCE_MAX];
static int sCount = 0;

bool InputSource_Register(InputSource *src)
{
	if (!src || sCount >= INPUT_SOURCE_MAX) {
		if (src) printf("input: registry full, cannot add '%s'\n", src->name);
		return false;
	}
	// Avoid duplicates (idempotent register).
	for (int i = 0; i < sCount; i++)
		if (sTable[i] == src) return true;
	sTable[sCount++] = src;
	return true;
}

void InputSource_InitAll(void)
{
	for (int i = 0; i < sCount; i++)
		if (sTable[i]->init) sTable[i]->init();
}

void InputSource_PollAll(void)
{
	for (int i = 0; i < sCount; i++)
		if (sTable[i]->poll) sTable[i]->poll();
}

void InputSource_TerminateAll(void)
{
	for (int i = sCount - 1; i >= 0; i--)
		if (sTable[i]->terminate) sTable[i]->terminate();
	sCount = 0;
}

void InputSource_CapturePadAll(int padID)
{
	for (int i = 0; i < sCount; i++)
		if (sTable[i]->capturePad) sTable[i]->capturePad(padID);
}

InputSource *InputSource_Get(void)
{
	return sCount > 0 ? sTable[0] : 0;
}

#endif
