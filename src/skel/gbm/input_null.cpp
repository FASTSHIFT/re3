/*
 * input_null.cpp - no-op InputSource fallback.
 *
 * Active for GBM builds that configure no input backend (e.g. the plain fbdev
 * debug build). Keeps the skeleton's InputSource_Get() resolvable. Input is
 * empty; the game still runs and renders.
 */
#if defined RW_GL3 && defined LIBRW_GBM && !defined(RE3_INPUT_EVDEV) && !defined(RE3_INPUT_SDL) && !defined(RE3_OUTPUT_SPI)

#include "input_source.h"
#include <stdio.h>

static void
null_init(void)
{
	printf("input: none (no input backend configured)\n");
}
static void
null_poll(void)
{
}
static void
null_terminate(void)
{
}

static InputSource sSrc = {null_init, null_poll, null_terminate, 0, "null"};

static struct NullAutoReg {
	NullAutoReg() { InputSource_Register(&sSrc); }
} sAutoReg;

#endif
