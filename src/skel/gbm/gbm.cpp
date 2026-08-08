// GBM/EGL surfaceless skeleton: no window system, renders offscreen and reads
// back to /dev/fb0 (later SPI). Derived from glfw.cpp; GLFW-specific bits
// replaced with GBM / no-op equivalents. See docs/07, docs/08.
#if defined RW_GL3 && defined LIBRW_GBM

#ifdef _WIN32
#include <shlobj.h>
#include <basetsd.h>
#include <mmsystem.h>
#include <regstr.h>
#include <shellapi.h>
#include <windowsx.h>

DWORD _dwOperatingSystemVersion;
#include "resource.h"
#else
long _dwOperatingSystemVersion;
#ifndef __SWITCH__
#ifndef __APPLE__
#include <sys/sysinfo.h>
#else
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#endif
#endif
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stddef.h>
#endif

#include "common.h"
#if(defined(_MSC_VER))
#include <tchar.h>
#endif /* (defined(_MSC_VER)) */
#include <stdio.h>
#include "rwcore.h"
#include "skeleton.h"
#include "platform.h"
#include "crossplatform.h"

#include "main.h"
#include "FileMgr.h"
#include "Text.h"
#include "Pad.h"
#include "Timer.h"
#include "DMAudio.h"
#include "ControllerConfig.h"
#include "Frontend.h"
#include "Game.h"
#include "PCSave.h"
#include "MemoryCard.h"
#include "Sprite2d.h"
#include "AnimViewer.h"
#include "Font.h"
#include "MemoryMgr.h"

// GBM/EGL surfaceless: no window system. The skeleton renders offscreen and
// reads the frame back with glReadPixels (GLES2). Where the frame goes and how
// input arrives are pluggable backends in separate files, selected at build
// time: sink_{fbdev,spi,sdl}.cpp and input_{evdev,gpio,sdl,null}.cpp.
// See docs/07, docs/08. This TU doesn't pull in librw's glad, so no clash.
#include <GLES2/gl2.h>
#include "output_sink.h"
#include "input_source.h"
#include "hud_overlay.h"

// librw GL3 draw call counter (defined in gl3render.cpp).
#ifdef RW_GL3
namespace rw
{
namespace gl3
{
int
gl3_get_and_reset_drawcalls(void);
}
} // namespace rw
#endif

#define MAX_SUBSYSTEMS (16)

rw::EngineOpenParams openParams;

static RwBool ForegroundApp = TRUE;
static RwBool WindowIconified = FALSE;
static RwBool WindowFocused = TRUE;

static RwBool RwInitialised = FALSE;

static RwSubSystemInfo GsubSysInfo[MAX_SUBSYSTEMS];
static RwInt32 GnumSubSystems = 0;
static RwInt32 GcurSel = 0, GcurSelVM = 0;

static RwBool useDefault;

// What is that for anyway?
#ifndef IMPROVED_VIDEOMODE
static RwBool defaultFullscreenRes = TRUE;
#else
static RwBool defaultFullscreenRes = FALSE;
static RwInt32 bestWndMode = -1;
#endif

static psGlobalType PsGlobal;

// Mouse state shared with the crossplatform.h glfw* shim. An input source with
// a real pointer (input_sdl.cpp / input_evdev.cpp) updates these via
// GbmFeedMouse(); on spi they stay 0 (no mouse). See crossplatform.h.
double gGbmMouseX = 0.0, gGbmMouseY = 0.0;
int gGbmMouseButtons = 0;

// Feed absolute mouse position (in render/screen pixels), button bitmask
// (indexed by GLFW_MOUSE_BUTTON_*), and wheel delta into re3. Updates the
// glfw* shim state (Pad.cpp reads it for in-game camera) and the frontend
// cursor position (FrontEndMenuManager). Call once per frame from an input
// source that has a pointer. Defined here so all input backends can share it.
void
GbmFeedMouse(double x, double y, int buttons, int wheel, bool inWindow)
{
	gGbmMouseX = x;
	gGbmMouseY = y;
	gGbmMouseButtons = buttons;
	PSGLOBAL(cursorIsInWindow) = inWindow ? TRUE : FALSE;
	if(wheel != 0) PSGLOBAL(mouseWheel) = (double)wheel;

	// Frontend cursor (menu hit-testing) reads m_nMousePosX/Y, copied from
	// these temp fields each frame (see CMenuManager). Scale window coords to
	// the menu's screen space.
	FrontEndMenuManager.m_nMouseTempPosX = (int)x;
	FrontEndMenuManager.m_nMouseTempPosY = (int)y;
}

#define PSGLOBAL(var) (((psGlobalType *)(RsGlobal.ps))->var)

size_t _dwMemAvailPhys;
RwUInt32 gGameState;

#ifdef DETECT_JOYSTICK_MENU
char gSelectedJoystickName[128] = "";
#endif

/*
 *****************************************************************************
 */
void
_psCreateFolder(const char *path)
{
#ifdef _WIN32
	HANDLE hfle = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, nil, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_ATTRIBUTE_NORMAL, nil);

	if(hfle == INVALID_HANDLE_VALUE)
		CreateDirectory(path, nil);
	else
		CloseHandle(hfle);
#else
	struct stat info;
	char fullpath[PATH_MAX];
	if(!realpath(path, fullpath)) fullpath[0] = '\0'; /* error: use empty path, lstat will fail gracefully */

	if(lstat(fullpath, &info) != 0) {
		if(errno == ENOENT || (errno != EACCES && !S_ISDIR(info.st_mode))) { mkdir(fullpath, 0755); }
	}
#endif
}

/*
 *****************************************************************************
 */
const char *
_psGetUserFilesFolder()
{
#if defined USE_MY_DOCUMENTS && defined _WIN32
	HKEY hKey = NULL;

	static CHAR szUserFiles[256];

	if(RegOpenKeyEx(HKEY_CURRENT_USER, REGSTR_PATH_SPECIAL_FOLDERS, REG_OPTION_RESERVED, KEY_READ, &hKey) == ERROR_SUCCESS) {
		DWORD KeyType;
		DWORD KeycbData = sizeof(szUserFiles);
		if(RegQueryValueEx(hKey, "Personal", NULL, &KeyType, (LPBYTE)szUserFiles, &KeycbData) == ERROR_SUCCESS) {
			RegCloseKey(hKey);
			strcat(szUserFiles, "\\GTA3 User Files");
			_psCreateFolder(szUserFiles);
			return szUserFiles;
		}

		RegCloseKey(hKey);
	}

	strcpy(szUserFiles, "data");
	return szUserFiles;
#else
	static char szUserFiles[256];
	strcpy(szUserFiles, "userfiles");
	_psCreateFolder(szUserFiles);
	return szUserFiles;
#endif
}

/*
 *****************************************************************************
 */
RwBool
psCameraBeginUpdate(RwCamera *camera)
{
	(void)camera; /* GBM always renders to Scene.camera; parameter kept for API compat */
	if(!RwCameraBeginUpdate(Scene.camera)) {
		ForegroundApp = FALSE;
		RsEventHandler(rsACTIVATE, (void *)FALSE);
		return FALSE;
	}

	return TRUE;
}

/*
 *****************************************************************************
 * Output: the skeleton owns the glReadPixels; a pluggable OutputSink presents
 * the frame (fbdev / spi / sdl, selected at build time). See output_sink.h.
 */
static OutputSink *gSink = nil;
static uint16 *gReadback565 = nil; // glReadPixels dst (RGB565, display-native)
static int gReadbackW = 0, gReadbackH = 0;

static void
_psOpenOutput(void)
{
	gSink = OutputSink_Get();
	if(!gSink->init(RsGlobal.maximumWidth, RsGlobal.maximumHeight)) {
		printf("output sink '%s' init failed\n", gSink->name);
		gSink = nil;
		return;
	}
	printf("output sink: %s\n", gSink->name);
}

static void
_psCloseOutput(void)
{
	if(gSink != nil) {
		gSink->terminate();
		gSink = nil;
	}
	if(gReadback565 != nil) {
		free(gReadback565);
		gReadback565 = nil;
	}
	gReadbackW = gReadbackH = 0;
}

// Per-frame perf metrics, shared with the HUD overlay and the optional
// RE3_TIME_PRESENT log. gGpuMs / gCpuMs are filled elsewhere (showRaster /
// main loop); readback + present are timed here.
static double gGpuMs = 0.0, gCpuMs = 0.0, gReadMs = 0.0, gPresentMs = 0.0, gFrameMs = 0.0;
static int gDrawCalls = 0;

static void
_psPresent(void)
{
	int w = RsGlobal.maximumWidth;
	int h = RsGlobal.maximumHeight;
	if(gSink == nil || w <= 0 || h <= 0) return;

	if(w != gReadbackW || h != gReadbackH || gReadback565 == nil) {
		if(gReadback565 != nil) free(gReadback565);
		gReadback565 = (uint16 *)malloc(w * h * 2);
		gReadbackW = w;
		gReadbackH = h;
	}
	if(gReadback565 == nil) return;

	// Wall time between frames (for FPS).
	static double lastWall = 0.0;
	double tw = psTimer();
	gFrameMs = (lastWall != 0.0) ? (tw - lastWall) : 0.0;
	lastWall = tw;

	// librw's GBM camera renders into an RGB565 texture FBO (showRaster left it
	// bound). Read it back directly as RGB565: no software conversion, half the
	// bytes, already in the display's native format (fb0 16bpp / ST7789).
	double t0 = psTimer();
	glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, gReadback565);
	double t1 = psTimer();
	gReadMs = t1 - t0;

	// Toggle the metrics overlay on each pause (menu inactive -> active edge),
	// so the player can hide/show it with the pause button. Checked every frame
	// regardless of current HUD state.
	{
		static bool sPrevMenuActive = false;
		bool menuActive = !!FrontEndMenuManager.m_bMenuActive;
		if(menuActive && !sPrevMenuActive) Hud_Toggle();
		sPrevMenuActive = menuActive;
	}

	// Overlay perf metrics onto the frame (RE3_HUD=1) before it goes to the sink.
	if(Hud_Enabled()) {
		HudMetrics m = {}; // zero-init: caller only fills timing fields
		m.frameMs = gFrameMs;
		m.cpuMs = gCpuMs;
		m.gpuMs = gGpuMs;
		m.readMs = gReadMs;
		m.presentMs = gPresentMs; // presentMs = last frame's
		m.drawCalls = gDrawCalls;
		Hud_Update(&m);
		Hud_Draw(gReadback565, w, h);
	}

	double t2 = psTimer();
	gSink->present(gReadback565, w, h);
	gPresentMs = psTimer() - t2;

	// Optional periodic log.
	static int timeOn = -1;
	if(timeOn < 0) timeOn = getenv("RE3_TIME_PRESENT") ? 1 : 0;
	if(timeOn) {
		static double aR = 0, aP = 0, aG = 0, aC = 0, aW = 0;
		static int aDC = 0, n = 0;
		aR += gReadMs;
		aP += gPresentMs;
		aG += gGpuMs;
		aC += gCpuMs;
		aW += gFrameMs;
		aDC += gDrawCalls;
		if(++n >= 120) {
			printf("[perf] %dx%d cpu=%.2f gpu=%.2f read=%.2f present=%.2f dc=%d | frame=%.2fms %.0ffps\n", w, h, aC / n, aG / n, aR / n, aP / n,
			       aDC / n, aW / n, aW > 0 ? 1000.0 / (aW / n) : 0.0);
			aR = aP = aG = aC = aW = 0;
			aDC = 0;
			n = 0;
		}
	}
}

/*
 *****************************************************************************
 */
void
psCameraShowRaster(RwCamera *camera)
{
	// librw's GBM showRaster is just glFinish (no swap). Time it: this is where
	// asynchronous GPU scene rendering is actually waited on, isolating
	// GPU-bound cost from CPU submit and readback.
	double t0 = psTimer();
	RwCameraShowRaster(camera, PSGLOBAL(window), rwRASTERFLIPDONTWAIT);
	gGpuMs = psTimer() - t0;
	// Snapshot draw call counter after GPU sync; reset for next frame.
#ifdef RW_GL3
	gDrawCalls = rw::gl3::gl3_get_and_reset_drawcalls();
#endif

	_psPresent();

	return;
}

/*
 *****************************************************************************
 */
RwImage *
psGrabScreen(RwCamera *pCamera)
{
#ifndef LIBRW
	RwRaster *pRaster = RwCameraGetRaster(pCamera);
	if(RwImage *pImage = RwImageCreate(pRaster->width, pRaster->height, 32)) {
		RwImageAllocatePixels(pImage);
		RwImageSetFromRaster(pImage, pRaster);
		return pImage;
	}
#else
	rw::Image *image = RwCameraGetRaster(pCamera)->toImage();
	image->removeMask();
	if(image) return image;
#endif
	return nil;
}

/*
 *****************************************************************************
 */
#ifdef _WIN32
#pragma comment(lib, "Winmm.lib") // Needed for time
RwUInt32
psTimer(void)
{
	RwUInt32 time;

	TIMECAPS TimeCaps;

	timeGetDevCaps(&TimeCaps, sizeof(TIMECAPS));

	timeBeginPeriod(TimeCaps.wPeriodMin);

	time = (RwUInt32)timeGetTime();

	timeEndPeriod(TimeCaps.wPeriodMin);

	return time;
}
#else
double
psTimer(void)
{
	struct timespec start;
#if defined(CLOCK_MONOTONIC_RAW)
	clock_gettime(CLOCK_MONOTONIC_RAW, &start);
#elif defined(CLOCK_MONOTONIC_FAST)
	clock_gettime(CLOCK_MONOTONIC_FAST, &start);
#else
	clock_gettime(CLOCK_MONOTONIC, &start);
#endif
	return start.tv_sec * 1000.0 + start.tv_nsec / 1000000.0;
}
#endif

/*
 *****************************************************************************
 */
void
psMouseSetPos(RwV2d *pos)
{
	// No window system / no mouse cursor under GBM.
	PSGLOBAL(lastMousePos.x) = (RwInt32)pos->x;

	PSGLOBAL(lastMousePos.y) = (RwInt32)pos->y;

	return;
}

/*
 *****************************************************************************
 */
RwMemoryFunctions *
psGetMemoryFunctions(void)
{
#ifdef USE_CUSTOM_ALLOCATOR
	return &memFuncs;
#else
	return nil;
#endif
}

/*
 *****************************************************************************
 */
RwBool
psInstallFileSystem(void)
{
	return (TRUE);
}

/*
 *****************************************************************************
 */
RwBool
psNativeTextureSupport(void)
{
	return true;
}

/*
 *****************************************************************************
 */
#ifdef UNDER_CE
#define CMDSTR LPWSTR
#else
#define CMDSTR LPSTR
#endif

/*
 *****************************************************************************
 */

#ifdef __SWITCH__

static HidVibrationValue SwitchVibrationValues[2];
static HidVibrationDeviceHandle SwitchVibrationDeviceHandles[2][2];
static HidVibrationDeviceHandle SwitchVibrationDeviceGC;

static PadState SwitchPad;

static Result HidInitializationResult[2];
static Result HidInitializationGCResult;

static void
_psInitializeVibration()
{
	HidInitializationResult[0] = hidInitializeVibrationDevices(SwitchVibrationDeviceHandles[0], 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
	if(R_FAILED(HidInitializationResult[0])) { printf("Failed to initialize VibrationDevice for Handheld Mode\n"); }
	HidInitializationResult[1] = hidInitializeVibrationDevices(SwitchVibrationDeviceHandles[1], 2, HidNpadIdType_No1, HidNpadStyleSet_NpadFullCtrl);
	if(R_FAILED(HidInitializationResult[1])) { printf("Failed to initialize VibrationDevice for Detached Mode\n"); }
	HidInitializationGCResult = hidInitializeVibrationDevices(&SwitchVibrationDeviceGC, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadGc);
	if(R_FAILED(HidInitializationResult[1])) { printf("Failed to initialize VibrationDevice for GC Mode\n"); }

	SwitchVibrationValues[0].freq_low = 160.0f;
	SwitchVibrationValues[0].freq_high = 320.0f;

	padConfigureInput(1, HidNpadStyleSet_NpadFullCtrl);
	padInitializeDefault(&SwitchPad);
}

static void
_psHandleVibration()
{
	padUpdate(&SwitchPad);

	uint8 target_device = padIsHandheld(&SwitchPad) ? 0 : 1;

	if(R_SUCCEEDED(HidInitializationResult[target_device])) {
		CPad *pad = CPad::GetPad(0);

		// value conversion based on SDL2 switch port
		SwitchVibrationValues[0].amp_high = SwitchVibrationValues[0].amp_low = pad->ShakeFreq == 0 ? 0.0f : 320.0f;
		SwitchVibrationValues[0].freq_low = pad->ShakeFreq == 0.0 ? 160.0f : (float)pad->ShakeFreq * 1.26f;
		SwitchVibrationValues[0].freq_high = pad->ShakeFreq == 0.0 ? 320.0f : (float)pad->ShakeFreq * 1.26f;

		if(pad->ShakeDur < CTimer::GetTimeStepInMilliseconds())
			pad->ShakeDur = 0;
		else
			pad->ShakeDur -= CTimer::GetTimeStepInMilliseconds();
		if(pad->ShakeDur == 0) pad->ShakeFreq = 0;

		if(target_device == 1 && R_SUCCEEDED(HidInitializationGCResult)) {
			// gamecube rumble
			hidSendVibrationGcErmCommand(SwitchVibrationDeviceGC,
			                             pad->ShakeFreq > 0 ? HidVibrationGcErmCommand_Start : HidVibrationGcErmCommand_Stop);
		}

		memcpy(&SwitchVibrationValues[1], &SwitchVibrationValues[0], sizeof(HidVibrationValue));
		hidSendVibrationValues(SwitchVibrationDeviceHandles[target_device], SwitchVibrationValues, 2);
	}
}
#else
static void
_psInitializeVibration()
{
}
static __attribute__((unused)) void
_psHandleVibration()
{
}
#endif

/*
 *****************************************************************************
 */
RwBool
psInitialize(void)
{
	PsGlobal.lastMousePos.x = PsGlobal.lastMousePos.y = 0.0f;

	RsGlobal.ps = &PsGlobal;

	PsGlobal.fullScreen = FALSE;
	PsGlobal.cursorIsInWindow = FALSE;
	WindowFocused = TRUE;
	WindowIconified = FALSE;

	PsGlobal.joy1id = -1;
	PsGlobal.joy2id = -1;

	CFileMgr::Initialise();

#ifdef PS2_MENU
	CPad::Initialise();
	CPad::GetPad(0)->Mode = 0;

	CGame::frenchGame = false;
	CGame::germanGame = false;
	CGame::nastyGame = true;
	CMenuManager::m_PrefsAllowNastyGame = true;

#ifndef _WIN32
	// Mandatory for Linux(Unix? Posix?) to set lang. to environment lang.
	setlocale(LC_ALL, "");

	char *systemLang, *keyboardLang;

	systemLang = setlocale(LC_ALL, NULL);
	keyboardLang = setlocale(LC_CTYPE, NULL);

	short lang;
	lang = !strncmp(systemLang, "fr_", 3)   ? LANG_FRENCH
	       : !strncmp(systemLang, "de_", 3) ? LANG_GERMAN
	       : !strncmp(systemLang, "en_", 3) ? LANG_ENGLISH
	       : !strncmp(systemLang, "it_", 3) ? LANG_ITALIAN
	       : !strncmp(systemLang, "es_", 3) ? LANG_SPANISH
	                                        : LANG_OTHER;
#else
	WORD lang = PRIMARYLANGID(GetSystemDefaultLCID());
#endif

	if(lang == LANG_ITALIAN)
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_ITALIAN;
	else if(lang == LANG_SPANISH)
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_SPANISH;
	else if(lang == LANG_GERMAN) {
		CGame::germanGame = true;
		CGame::nastyGame = false;
		CMenuManager::m_PrefsAllowNastyGame = false;
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_GERMAN;
	} else if(lang == LANG_FRENCH) {
		CGame::frenchGame = true;
		CGame::nastyGame = false;
		CMenuManager::m_PrefsAllowNastyGame = false;
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_FRENCH;
	} else
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_AMERICAN;

	FrontEndMenuManager.InitialiseMenuContentsAfterLoadingGame();

	TheMemoryCard.Init();
#else
	C_PcSave::SetSaveDirectory(_psGetUserFilesFolder());

	InitialiseLanguage();

#if GTA_VERSION < GTA3_PC_11
	FrontEndMenuManager.LoadSettings();
#endif

#endif

	_psInitializeVibration();

	gGameState = GS_START_UP;
	TRACE("gGameState = GS_START_UP");
#ifdef _WIN32
	OSVERSIONINFO verInfo;
	verInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);

	GetVersionEx(&verInfo);

	_dwOperatingSystemVersion = OS_WIN95;

	if(verInfo.dwPlatformId == VER_PLATFORM_WIN32_NT) {
		if(verInfo.dwMajorVersion == 4) {
			debug("Operating System is WinNT\n");
			_dwOperatingSystemVersion = OS_WINNT;
		} else if(verInfo.dwMajorVersion == 5) {
			debug("Operating System is Win2000\n");
			_dwOperatingSystemVersion = OS_WIN2000;
		} else if(verInfo.dwMajorVersion > 5) {
			debug("Operating System is WinXP or greater\n");
			_dwOperatingSystemVersion = OS_WINXP;
		}
	} else if(verInfo.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS) {
		if(verInfo.dwMajorVersion > 4 || verInfo.dwMajorVersion == 4 && verInfo.dwMinorVersion != 0) {
			debug("Operating System is Win98\n");
			_dwOperatingSystemVersion = OS_WIN98;
		} else {
			debug("Operating System is Win95\n");
			_dwOperatingSystemVersion = OS_WIN95;
		}
	}
#else
	_dwOperatingSystemVersion = OS_WINXP; // To fool other classes
#endif

#ifndef PS2_MENU

#if GTA_VERSION >= GTA3_PC_11
	FrontEndMenuManager.LoadSettings();
#endif

#endif

#ifdef _WIN32
	MEMORYSTATUS memstats;
	GlobalMemoryStatus(&memstats);

	_dwMemAvailPhys = memstats.dwAvailPhys;

	debug("Physical memory size %u\n", memstats.dwTotalPhys);
	debug("Available physical memory %u\n", memstats.dwAvailPhys);
#elif defined(__APPLE__)
	uint64_t size = 0;
	uint64_t page_size = 0;
	size_t uint64_len = sizeof(uint64_t);
	size_t ull_len = sizeof(unsigned long long);
	sysctl((int[]){CTL_HW, HW_PAGESIZE}, 2, &page_size, &ull_len, NULL, 0);
	sysctl((int[]){CTL_HW, HW_MEMSIZE}, 2, &size, &uint64_len, NULL, 0);
	vm_statistics_data_t vm_stat;
	mach_msg_type_number_t count = HOST_VM_INFO_COUNT;
	host_statistics(mach_host_self(), HOST_VM_INFO, (host_info_t)&vm_stat, &count);
	_dwMemAvailPhys = (uint64_t)(vm_stat.free_count * page_size);
	debug("Physical memory size %llu\n", _dwMemAvailPhys);
	debug("Available physical memory %llu\n", size);
#elif defined(__SWITCH__)
	svcGetInfo(&_dwMemAvailPhys, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
	debug("Physical memory size %llu\n", _dwMemAvailPhys);
#else
	struct sysinfo systemInfo;
	sysinfo(&systemInfo);
	_dwMemAvailPhys = systemInfo.freeram;
	debug("Physical memory size %u\n", systemInfo.totalram);
	debug("Available physical memory %u\n", systemInfo.freeram);
#endif

	TheText.Unload();

	return TRUE;
}

/*
 *****************************************************************************
 */
void
psTerminate(void)
{
	_psCloseOutput();
	InputSource_TerminateAll();
	return;
}

/*
 *****************************************************************************
 */
static RwChar **_VMList;

RwInt32
_psGetNumVideModes()
{
	return RwEngineGetNumVideoModes();
}

/*
 *****************************************************************************
 */
RwBool
_psFreeVideoModeList()
{
	RwInt32 numModes;
	RwInt32 i;

	numModes = _psGetNumVideModes();

	if(_VMList == nil) return TRUE;

	for(i = 0; i < numModes; i++) { RwFree(_VMList[i]); }

	RwFree(_VMList);

	_VMList = nil;

	return TRUE;
}

/*
 *****************************************************************************
 */
RwChar **
_psGetVideoModeList()
{
	RwInt32 numModes;
	RwInt32 i;

	if(_VMList != nil) { return _VMList; }

	numModes = RwEngineGetNumVideoModes();

	_VMList = (RwChar **)RwCalloc(numModes, sizeof(RwChar *));

	for(i = 0; i < numModes; i++) {
		RwVideoMode vm;

		RwEngineGetVideoModeInfo(&vm, i);

		// GBM exposes a single offscreen mode with no rwVIDEOMODEEXCLUSIVE flag.
		// The desktop skeletons only list exclusive (fullscreen) modes and leave
		// windowed ones nil, but the Display Settings menu does
		// AsciiToUnicode(_psGetVideoModeList()[m_nDisplayVideoMode], ...) which
		// dereferences the entry -> null deref / crash. Always provide a string.
		_VMList[i] = (RwChar *)RwCalloc(100, sizeof(RwChar));
		rwsprintf(_VMList[i], "%d X %d X %d", vm.width, vm.height, vm.depth);
	}

	return _VMList;
}

/*
 *****************************************************************************
 */
void
_psSelectScreenVM(RwInt32 videoMode)
{
	RwTexDictionarySetCurrent(nil);

	FrontEndMenuManager.UnloadTextures();

	if(!_psSetVideoMode(RwEngineGetCurrentSubSystem(), videoMode)) {
		RsGlobal.quit = TRUE;

		printf("ERROR: Failed to select new screen resolution\n");
	} else
		FrontEndMenuManager.LoadAllTextures();
}

/*
 *****************************************************************************
 */

RwBool
IsForegroundApp()
{
	return !!ForegroundApp;
}
/*
UINT GetBestRefreshRate(UINT width, UINT height, UINT depth)
{
        LPDIRECT3D8 d3d = Direct3DCreate8(D3D_SDK_VERSION);

        ASSERT(d3d != nil);

        UINT refreshRate = INT_MAX;
        D3DFORMAT format;

        if ( depth == 32 )
                format = D3DFMT_X8R8G8B8;
        else if ( depth == 24 )
                format = D3DFMT_R8G8B8;
        else
                format = D3DFMT_R5G6B5;

        UINT modeCount = d3d->GetAdapterModeCount(GcurSel);

        for ( UINT i = 0; i < modeCount; i++ )
        {
                D3DDISPLAYMODE mode;

                d3d->EnumAdapterModes(GcurSel, i, &mode);

                if ( mode.Width == width && mode.Height == height && mode.Format == format )
                {
                        if ( mode.RefreshRate == 0 )
                                return 0;

                        if ( mode.RefreshRate < refreshRate && mode.RefreshRate >= 60 )
                                refreshRate = mode.RefreshRate;
                }
        }

#ifdef FIX_BUGS
        d3d->Release();
#endif

        if ( refreshRate == -1 )
                return -1;

        return refreshRate;
}
*/
/*
 *****************************************************************************
 */
RwBool
psSelectDevice()
{
	RwVideoMode vm;
	RwInt32 subSysNum;
	RwInt32 AutoRenderer = 0;
	(void)AutoRenderer; /* used in non-IMPROVED_VIDEOMODE path only */

	RwBool modeFound = FALSE;
	(void)modeFound; /* used in non-IMPROVED_VIDEOMODE path only */

	if(!useDefault) {
		GnumSubSystems = RwEngineGetNumSubSystems();
		if(!GnumSubSystems) { return FALSE; }

		/* Just to be sure ... */
		GnumSubSystems = (GnumSubSystems > MAX_SUBSYSTEMS) ? MAX_SUBSYSTEMS : GnumSubSystems;

		/* Get the names of all the sub systems */
		for(subSysNum = 0; subSysNum < GnumSubSystems; subSysNum++) { RwEngineGetSubSystemInfo(&GsubSysInfo[subSysNum], subSysNum); }

		/* Get the default selection */
		GcurSel = RwEngineGetCurrentSubSystem();
#ifdef IMPROVED_VIDEOMODE
		if(FrontEndMenuManager.m_nPrefsSubsystem < GnumSubSystems) GcurSel = FrontEndMenuManager.m_nPrefsSubsystem;
#endif
	}

	/* Set the driver to use the correct sub system */
	if(!RwEngineSetSubSystem(GcurSel)) { return FALSE; }

#ifdef IMPROVED_VIDEOMODE
	FrontEndMenuManager.m_nPrefsSubsystem = GcurSel;
#endif

#ifndef IMPROVED_VIDEOMODE
	if(!useDefault) {
		if(_psGetVideoModeList()[FrontEndMenuManager.m_nDisplayVideoMode] && FrontEndMenuManager.m_nDisplayVideoMode) {
			FrontEndMenuManager.m_nPrefsVideoMode = FrontEndMenuManager.m_nDisplayVideoMode;
			GcurSelVM = FrontEndMenuManager.m_nDisplayVideoMode;
		} else {
#ifdef DEFAULT_NATIVE_RESOLUTION
			// get the native video mode
			HDC hDevice = GetDC(NULL);
			int w = GetDeviceCaps(hDevice, HORZRES);
			int h = GetDeviceCaps(hDevice, VERTRES);
			int d = GetDeviceCaps(hDevice, BITSPIXEL);
#else
			const int w = 640;
			const int h = 480;
			const int d = 16;
#endif
			while(!modeFound && GcurSelVM < RwEngineGetNumVideoModes()) {
				RwEngineGetVideoModeInfo(&vm, GcurSelVM);
				if(defaultFullscreenRes && vm.width != w || vm.height != h || vm.depth != d || !(vm.flags & rwVIDEOMODEEXCLUSIVE))
					++GcurSelVM;
				else
					modeFound = TRUE;
			}

			if(!modeFound) {
#ifdef DEFAULT_NATIVE_RESOLUTION
				GcurSelVM = 1;
#else
				printf("WARNING: Cannot find 640x480 video mode, selecting device cancelled\n");
				return FALSE;
#endif
			}
		}
	}
#else
	if(!useDefault) {
		if(FrontEndMenuManager.m_nPrefsWidth == 0 || FrontEndMenuManager.m_nPrefsHeight == 0 || FrontEndMenuManager.m_nPrefsDepth == 0) {
			// Defaults if nothing specified. GBM has a single offscreen mode
			// (the render resolution), so fall back to RsGlobal defaults.
			FrontEndMenuManager.m_nPrefsWidth = RsGlobal.maximumWidth;
			FrontEndMenuManager.m_nPrefsHeight = RsGlobal.maximumHeight;
			FrontEndMenuManager.m_nPrefsDepth = 32;
			FrontEndMenuManager.m_nPrefsWindowed = 0;
		}

		// GBM exposes a single offscreen "video mode" (index 0) sized to the
		// render resolution, with no rwVIDEOMODEEXCLUSIVE flag. The upstream
		// best-fit search only accepts exclusive modes, so just select mode 0.
		GcurSelVM = 0;
		bestWndMode = 0;

		FrontEndMenuManager.m_nDisplayVideoMode = GcurSelVM;
		FrontEndMenuManager.m_nPrefsVideoMode = FrontEndMenuManager.m_nDisplayVideoMode;

		FrontEndMenuManager.m_nSelectedScreenMode = FrontEndMenuManager.m_nPrefsWindowed;
	}
#endif

	RwEngineGetVideoModeInfo(&vm, GcurSelVM);

#ifdef IMPROVED_VIDEOMODE
	if(FrontEndMenuManager.m_nPrefsWindowed) GcurSelVM = bestWndMode;

	// Now GcurSelVM is 0 but vm has sizes(and fullscreen flag) of the video mode we want, that's why we changed the rwVIDEOMODEEXCLUSIVE conditions below
	FrontEndMenuManager.m_nPrefsWidth = vm.width;
	FrontEndMenuManager.m_nPrefsHeight = vm.height;
	FrontEndMenuManager.m_nPrefsDepth = vm.depth;
#endif

#ifndef PS2_MENU
	FrontEndMenuManager.m_nCurrOption = 0;
#endif

	/* Set up the video mode and set the apps window
	 * dimensions to match */
	if(!RwEngineSetVideoMode(GcurSelVM)) { return FALSE; }
	/*
	TODO
	if (vm.flags & rwVIDEOMODEEXCLUSIVE)
	{
	        debug("%dx%dx%d", vm.width, vm.height, vm.depth);

	        UINT refresh = GetBestRefreshRate(vm.width, vm.height, vm.depth);

	        if ( refresh != (UINT)-1 )
	        {
	                debug("refresh %d", refresh);
	                RwD3D8EngineSetRefreshRate((RwUInt32)refresh);
	        }
	}
	*/
#ifndef IMPROVED_VIDEOMODE
	if(vm.flags & rwVIDEOMODEEXCLUSIVE) {
		RsGlobal.maximumWidth = vm.width;
		RsGlobal.maximumHeight = vm.height;
		RsGlobal.width = vm.width;
		RsGlobal.height = vm.height;

		PSGLOBAL(fullScreen) = TRUE;
	}
#else
	RsGlobal.maximumWidth = FrontEndMenuManager.m_nPrefsWidth;
	RsGlobal.maximumHeight = FrontEndMenuManager.m_nPrefsHeight;
	RsGlobal.width = FrontEndMenuManager.m_nPrefsWidth;
	RsGlobal.height = FrontEndMenuManager.m_nPrefsHeight;

	PSGLOBAL(fullScreen) = !FrontEndMenuManager.m_nPrefsWindowed;
#endif

#ifdef MULTISAMPLING
	RwD3D8EngineSetMultiSamplingLevels(1 << FrontEndMenuManager.m_nPrefsMSAALevel);
#endif
	return TRUE;
}

// No window system under GBM: there are no input/window callbacks to register.
// Joystick/keyboard input will later come from a pluggable source (GPIO, see
// docs/08 §5). For the first milestone input is empty.

bool
IsThisJoystickBlacklisted(int i)
{
	(void)i;
	return true; // no joysticks enumerated under GBM
}

void
_InputInitialiseJoys()
{
	// No GLFW joystick enumeration; GPIO input source is a later task (docs/08 §5).
	PSGLOBAL(joy1id) = -1;
	PSGLOBAL(joy2id) = -1;
}

long
_InputInitialiseMouse()
{
	// No mouse / no cursor under GBM.
	return 0;
}

void
psPostRWinit(void)
{
	// No window system: no callbacks, no window resize. Open the output sink
	// (fbdev/spi/sdl) and input source (evdev/gpio/sdl/null), then clear pads.
	_psOpenOutput();

	_InputInitialiseJoys();
	_InputInitialiseMouse();
	InputSource_InitAll();

	// Make sure all keys are released
	CPad::GetPad(0)->Clear(true);
	CPad::GetPad(1)->Clear(true);
}

/*
 *****************************************************************************
 */
RwBool
_psSetVideoMode(RwInt32 subSystem, RwInt32 videoMode)
{
	RwInitialised = FALSE;

	RsEventHandler(rsRWTERMINATE, nil);

	GcurSel = subSystem;
	GcurSelVM = videoMode;

	useDefault = TRUE;

	if(RsEventHandler(rsRWINITIALIZE, &openParams) == rsEVENTERROR) return FALSE;

	RwInitialised = TRUE;
	useDefault = FALSE;

	RwRect r;

	r.x = 0;
	r.y = 0;
	r.w = RsGlobal.maximumWidth;
	r.h = RsGlobal.maximumHeight;

	RsEventHandler(rsCAMERASIZE, &r);

	psPostRWinit();

	return TRUE;
}

/*
 *****************************************************************************
 */
static __attribute__((unused)) RwChar **
CommandLineToArgv(RwChar *cmdLine, RwInt32 *argCount)
{
	RwInt32 numArgs = 0;
	RwBool inArg, inString;
	RwInt32 i, len;
	RwChar *res, *str, **aptr;

	len = strlen(cmdLine);

	/*
	 * Count the number of arguments...
	 */
	inString = FALSE;
	inArg = FALSE;

	for(i = 0; i <= len; i++) {
		if(cmdLine[i] == '"') { inString = !inString; }

		if((cmdLine[i] <= ' ' && !inString) || i == len) {
			if(inArg) {
				inArg = FALSE;

				numArgs++;
			}
		} else if(!inArg) {
			inArg = TRUE;
		}
	}

	/*
	 * Allocate memory for result...
	 */
	res = (RwChar *)malloc(sizeof(RwChar *) * numArgs + len + 1);
	str = res + sizeof(RwChar *) * numArgs;
	aptr = (RwChar **)res;

	strcpy(str, cmdLine);

	/*
	 * Walk through cmdLine again this time setting pointer to each arg...
	 */
	inArg = FALSE;
	inString = FALSE;

	for(i = 0; i <= len; i++) {
		if(cmdLine[i] == '"') { inString = !inString; }

		if((cmdLine[i] <= ' ' && !inString) || i == len) {
			if(inArg) {
				if(str[i - 1] == '"') {
					str[i - 1] = '\0';
				} else {
					str[i] = '\0';
				}

				inArg = FALSE;
			}
		} else if(!inArg && cmdLine[i] != '"') {
			inArg = TRUE;

			*aptr++ = &str[i];
		}
	}

	*argCount = numArgs;

	return (RwChar **)res;
}

/*
 *****************************************************************************
 */
void
InitialiseLanguage()
{
#ifndef _WIN32
	// Mandatory for Linux(Unix? Posix?) to set lang. to environment lang.
	setlocale(LC_ALL, "");

	char *systemLang, *keyboardLang;

	systemLang = setlocale(LC_ALL, NULL);
	keyboardLang = setlocale(LC_CTYPE, NULL);

	short primUserLCID, primSystemLCID;
	primUserLCID = primSystemLCID = !strncmp(systemLang, "fr_", 3)   ? LANG_FRENCH
	                                : !strncmp(systemLang, "de_", 3) ? LANG_GERMAN
	                                : !strncmp(systemLang, "en_", 3) ? LANG_ENGLISH
	                                : !strncmp(systemLang, "it_", 3) ? LANG_ITALIAN
	                                : !strncmp(systemLang, "es_", 3) ? LANG_SPANISH
	                                                                 : LANG_OTHER;

	short primLayout = !strncmp(keyboardLang, "fr_", 3) ? LANG_FRENCH : (!strncmp(keyboardLang, "de_", 3) ? LANG_GERMAN : LANG_ENGLISH);

	short subUserLCID, subSystemLCID;
	subUserLCID = subSystemLCID = !strncmp(systemLang, "en_AU", 5) ? SUBLANG_ENGLISH_AUS : SUBLANG_OTHER;
	short subLayout = !strncmp(keyboardLang, "en_AU", 5) ? SUBLANG_ENGLISH_AUS : SUBLANG_OTHER;

#else
	WORD primUserLCID = PRIMARYLANGID(GetSystemDefaultLCID());
	WORD primSystemLCID = PRIMARYLANGID(GetUserDefaultLCID());
	WORD primLayout = PRIMARYLANGID((DWORD)GetKeyboardLayout(0));

	WORD subUserLCID = SUBLANGID(GetSystemDefaultLCID());
	WORD subSystemLCID = SUBLANGID(GetUserDefaultLCID());
	WORD subLayout = SUBLANGID((DWORD)GetKeyboardLayout(0));
#endif
	if(primUserLCID == LANG_GERMAN || primSystemLCID == LANG_GERMAN || primLayout == LANG_GERMAN) {
		CGame::nastyGame = false;
		CMenuManager::m_PrefsAllowNastyGame = false;
		CGame::germanGame = true;
	}

	if(primUserLCID == LANG_FRENCH || primSystemLCID == LANG_FRENCH || primLayout == LANG_FRENCH) {
		CGame::nastyGame = false;
		CMenuManager::m_PrefsAllowNastyGame = false;
		CGame::frenchGame = true;
	}

	if(subUserLCID == SUBLANG_ENGLISH_AUS || subSystemLCID == SUBLANG_ENGLISH_AUS || subLayout == SUBLANG_ENGLISH_AUS) CGame::noProstitutes = true;

#ifdef NASTY_GAME
	CGame::nastyGame = true;
	CMenuManager::m_PrefsAllowNastyGame = true;
	CGame::noProstitutes = false;
#endif

	int32 lang;

	switch(primSystemLCID) {
	case LANG_GERMAN: {
		lang = LANG_GERMAN;
		break;
	}
	case LANG_FRENCH: {
		lang = LANG_FRENCH;
		break;
	}
	case LANG_SPANISH: {
		lang = LANG_SPANISH;
		break;
	}
	case LANG_ITALIAN: {
		lang = LANG_ITALIAN;
		break;
	}
	default: {
		lang = (subSystemLCID == SUBLANG_ENGLISH_AUS) ? -99 : LANG_ENGLISH;
		break;
	}
	}

	CMenuManager::OS_Language = primUserLCID;

	switch(lang) {
	case LANG_GERMAN: {
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_GERMAN;
		break;
	}
	case LANG_SPANISH: {
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_SPANISH;
		break;
	}
	case LANG_FRENCH: {
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_FRENCH;
		break;
	}
	case LANG_ITALIAN: {
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_ITALIAN;
		break;
	}
	default: {
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_AMERICAN;
		break;
	}
	}

#ifndef _WIN32
	// TODO this is needed for strcasecmp to work correctly across all languages, but can these cause other problems??
	setlocale(LC_CTYPE, "C");
	setlocale(LC_COLLATE, "C");
	setlocale(LC_NUMERIC, "C");
#endif

	TheText.Unload();
	TheText.Load();
}

/*
 *****************************************************************************
 */

void
HandleExit()
{
#ifdef _WIN32
	MSG message;
	while(PeekMessage(&message, nil, 0U, 0U, PM_REMOVE | PM_NOYIELD)) {
		if(message.message == WM_QUIT) {
			RsGlobal.quit = TRUE;
		} else {
			TranslateMessage(&message);
			DispatchMessage(&message);
		}
	}
#else
	// We now handle terminate message always, why handle on some cases?
	return;
#endif
}

#ifndef _WIN32
void
terminateHandler(int sig, siginfo_t *info, void *ucontext)
{
	(void)sig;
	(void)info;
	(void)ucontext;
	RsGlobal.quit = TRUE;
}

#ifdef FLUSHABLE_STREAMING
void
dummyHandler(int sig)
{
	(void)sig;
	// Don't kill the app pls
}
#endif
#endif

// No window system under GBM: window resize / scroll callbacks are gone.

bool lshiftStatus = false;
bool rshiftStatus = false;

// GBM: no window system, so no keyboard callbacks / keymap. Provide a no-op
// initkeymap(); real input comes from a pluggable source later (docs/08 §5).
static void
initkeymap(void)
{
}

// R* calls that in ControllerConfig, idk why. Under GBM there's no keyboard
// input yet, so keep the shift status flags at their (false) defaults.
void
_InputTranslateShiftKeyUpDown(RsKeyCodes *rs)
{
	RsKeyboardEventHandler(lshiftStatus ? rsKEYDOWN : rsKEYUP, &(*rs = rsLSHIFT));
	RsKeyboardEventHandler(rshiftStatus ? rsKEYDOWN : rsKEYUP, &(*rs = rsRSHIFT));
}

/*
 *****************************************************************************
 */
#ifdef _WIN32
int PASCAL
WinMain(HINSTANCE instance, HINSTANCE prevInstance __RWUNUSED__, CMDSTR cmdLine, int cmdShow)
{

	RwInt32 argc;
	RwChar **argv;
	SystemParametersInfo(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, nil, SPIF_SENDCHANGE);

#ifndef MASTER
	if(strstr(cmdLine, "-console")) {
		AllocConsole();
		freopen("CONIN$", "r", stdin);
		freopen("CONOUT$", "w", stdout);
		freopen("CONOUT$", "w", stderr);
	}
#endif

#else
int
main(int argc, char *argv[])
{
#endif
	RwV2d pos;
	RwInt32 i;

	// Headless: stdout/stderr are pipes to a log file (block-buffered by libc),
	// which hides progress and makes crashes look like they happen elsewhere.
	// Force line buffering so the log reflects real-time progress.
	setvbuf(stdout, nil, _IOLBF, 0);
	setvbuf(stderr, nil, _IONBF, 0);

#ifdef USE_CUSTOM_ALLOCATOR
	InitMemoryMgr();
#endif

#if !defined(_WIN32) && !defined(__SWITCH__)
	struct sigaction act;
	act.sa_sigaction = terminateHandler;
	act.sa_flags = SA_SIGINFO;
	sigaction(SIGTERM, &act, NULL);
#ifdef FLUSHABLE_STREAMING
	struct sigaction sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = dummyHandler;
	sa.sa_flags = 0;
	sigaction(SIGUSR1, &sa, NULL);
#endif
#endif

	/*
	 * Initialize the platform independent data.
	 * This will in turn initialize the platform specific data...
	 */
	if(RsEventHandler(rsINITIALIZE, nil) == rsEVENTERROR) { return FALSE; }

#ifdef _WIN32
	/*
	 * Get proper command line params, cmdLine passed to us does not
	 * work properly under all circumstances...
	 */
	cmdLine = GetCommandLine();

	/*
	 * Parse command line into standard (argv, argc) parameters...
	 */
	argv = CommandLineToArgv(cmdLine, &argc);

	/*
	 * Parse command line parameters (except program name) one at
	 * a time BEFORE RenderWare initialization...
	 */
#endif
	for(i = 1; i < argc; i++) { RsEventHandler(rsPREINITCOMMANDLINE, argv[i]); }

	/*
	 * Parameters to be used in RwEngineOpen / rsRWINITIALISE event
	 */

	// GBM has a single offscreen video mode sized to openParams at RW-open
	// time; psSelectDevice can't resize it afterwards. So pick the render
	// resolution here from the saved settings (re3.ini, loaded in psInitialize),
	// falling back to the RsGlobal defaults. This is what makes the ini
	// Width/Height actually change the GBM render (and readback) size.
#ifdef IMPROVED_VIDEOMODE
	if(FrontEndMenuManager.m_nPrefsWidth > 0 && FrontEndMenuManager.m_nPrefsHeight > 0) {
		RsGlobal.maximumWidth = RsGlobal.width = FrontEndMenuManager.m_nPrefsWidth;
		RsGlobal.maximumHeight = RsGlobal.height = FrontEndMenuManager.m_nPrefsHeight;
	}
#endif

	openParams.width = RsGlobal.maximumWidth;
	openParams.height = RsGlobal.maximumHeight;
	openParams.windowtitle = RsGlobal.appName;
	openParams.window = &PSGLOBAL(window);

	ControlsManager.MakeControllerActionsBlank();
	ControlsManager.InitDefaultControlConfiguration();

	/*
	 * Initialize the 3D (RenderWare) components of the app...
	 */
	if(rsEVENTERROR == RsEventHandler(rsRWINITIALIZE, &openParams)) {
		RsEventHandler(rsTERMINATE, nil);

		return 0;
	}

#ifdef _WIN32
	HWND wnd = glfwGetWin32Window(PSGLOBAL(window));

	HICON icon = LoadIcon(instance, MAKEINTRESOURCE(IDI_MAIN_ICON));

	SendMessage(wnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
	SendMessage(wnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
#endif

	psPostRWinit();

	ControlsManager.InitDefaultControlConfigMouse(MousePointerStateHelper.GetMouseSetUp());

	//	glfwSetWindowPos(PSGLOBAL(window), 0, 0);

	/*
	 * Parse command line parameters (except program name) one at
	 * a time AFTER RenderWare initialization...
	 */
	for(i = 1; i < argc; i++) { RsEventHandler(rsCOMMANDLINE, argv[i]); }

	/*
	 * Force a camera resize event...
	 */
	{
		RwRect r;

		r.x = 0;
		r.y = 0;
		r.w = RsGlobal.maximumWidth;
		r.h = RsGlobal.maximumHeight;

		RsEventHandler(rsCAMERASIZE, &r);
	}
#ifdef _WIN32
	SystemParametersInfo(SPI_SETPOWEROFFACTIVE, FALSE, nil, SPIF_SENDCHANGE);
	SystemParametersInfo(SPI_SETLOWPOWERACTIVE, FALSE, nil, SPIF_SENDCHANGE);

	STICKYKEYS SavedStickyKeys;
	SavedStickyKeys.cbSize = sizeof(STICKYKEYS);

	SystemParametersInfo(SPI_GETSTICKYKEYS, sizeof(STICKYKEYS), &SavedStickyKeys, SPIF_SENDCHANGE);

	STICKYKEYS NewStickyKeys;
	NewStickyKeys.cbSize = sizeof(STICKYKEYS);
	NewStickyKeys.dwFlags = SKF_TWOKEYSOFF;

	SystemParametersInfo(SPI_SETSTICKYKEYS, sizeof(STICKYKEYS), &NewStickyKeys, SPIF_SENDCHANGE);
#endif

	{
		CFileMgr::SetDirMyDocuments();

#ifdef LOAD_INI_SETTINGS
		// At this point InitDefaultControlConfigJoyPad must have set all bindings to default and ms_padButtonsInited to number of detected buttons.
		// We will load stored bindings below, but let's cache ms_padButtonsInited before LoadINIControllerSettings and LoadSettings clears it,
		// so we can add new joy bindings **on top of** stored bindings.
		int connectedPadButtons = ControlsManager.ms_padButtonsInited;
#endif

		int32 gta3set = CFileMgr::OpenFile("gta3.set", "r");

		if(gta3set) {
			ControlsManager.LoadSettings(gta3set);
			CFileMgr::CloseFile(gta3set);
		}

		CFileMgr::SetDir("");

#ifdef LOAD_INI_SETTINGS
		LoadINIControllerSettings();
		if(connectedPadButtons != 0)
			ControlsManager.InitDefaultControlConfigJoyPad(
			    connectedPadButtons); // add (connected-saved) amount of new button assignments on top of ours

		// these have 2 purposes: creating .ini at the start, and adding newly introduced settings to old .ini at the start
		SaveINISettings();
		SaveINIControllerSettings();
#endif
	}

#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS);
#endif

#ifdef PS2_MENU
	int32 r = TheMemoryCard.CheckCardStateAtGameStartUp(CARD_ONE);
	if(r == CMemoryCard::ERR_DIRNOENTRY || r == CMemoryCard::ERR_NOFORMAT && r != CMemoryCard::ERR_OPENNOENTRY && r != CMemoryCard::ERR_NONE) {
		LoadingScreen(nil, nil, "loadsc0");

		TheText.Unload();
		TheText.Load();

		CFont::Initialise();

		FrontEndMenuManager.DrawMemoryCardStartUpMenus();
	}
#endif

	initkeymap();

	while(TRUE) {
		RwInitialised = TRUE;

		/*
		 * Set the initial mouse position...
		 */
		pos.x = RsGlobal.maximumWidth * 0.5f;
		pos.y = RsGlobal.maximumHeight * 0.5f;

		RsMouseSetPos(&pos);

		/*
		 * Enter the message processing loop...
		 */

#ifndef MASTER
		if(gbModelViewer) {
			// This is TheModelViewer in LCS, but not compiled on III Mobile.
			LoadingScreen("Loading the ModelViewer", NULL, GetRandomSplashScreen());
			CAnimViewer::Initialise();
			CTimer::Update();
#ifndef PS2_MENU
			FrontEndMenuManager.m_bGameNotLoaded = false;
#endif
		}
#endif

#ifdef PS2_MENU
		if(TheMemoryCard.m_bWantToLoad) LoadSplash(GetLevelSplashScreen(CGame::currLevel));

		TheMemoryCard.m_bWantToLoad = false;

		CTimer::Update();

		while(!RsGlobal.quit && !(FrontEndMenuManager.m_bWantToRestart || TheMemoryCard.b_FoundRecentSavedGameWantToLoad))
#else
		while(!RsGlobal.quit && !FrontEndMenuManager.m_bWantToRestart)
#endif
		{
			// No window system: no event pump. Quit is driven by RsGlobal.quit
			// Poll all registered input sources each frame.
			InputSource_PollAll();
#ifndef MASTER
			if(gbModelViewer) {
				// This is TheModelViewerCore in LCS, but TheModelViewer on other state-machine III-VCs.
				TheModelViewer();
			} else
#endif
			    if(ForegroundApp) {
				switch(gGameState) {
				case GS_START_UP: {
#ifdef NO_MOVIES
					gGameState = GS_INIT_ONCE;
#else
					gGameState = GS_INIT_LOGO_MPEG;
#endif
					TRACE("gGameState = GS_INIT_ONCE");
					break;
				}

				case GS_INIT_LOGO_MPEG: {
					// if (!startupDeactivate)
					//     PlayMovieInWindow(cmdShow, "movies\\Logo.mpg");
					gGameState = GS_LOGO_MPEG;
					TRACE("gGameState = GS_LOGO_MPEG;");
					break;
				}

				case GS_LOGO_MPEG: {
					//					    CPad::UpdatePads();

					//					    if (startupDeactivate || ControlsManager.GetJoyButtonJustDown() != 0)
					++gGameState;
					//					    else if (CPad::GetPad(0)->GetLeftMouseJustDown())
					//						    ++gGameState;
					//					    else if (CPad::GetPad(0)->GetEnterJustDown())
					//						    ++gGameState;
					//					    else if (CPad::GetPad(0)->GetCharJustDown(' '))
					//						    ++gGameState;
					//					    else if (CPad::GetPad(0)->GetAltJustDown())
					//						    ++gGameState;
					//					    else if (CPad::GetPad(0)->GetTabJustDown())
					//						    ++gGameState;

					break;
				}

				case GS_INIT_INTRO_MPEG: {
					// #ifndef NO_MOVIES
					//					    CloseClip();
					//					    CoUninitialize();
					// #endif
					//
					//					    if (CMenuManager::OS_Language == LANG_FRENCH || CMenuManager::OS_Language ==
					// LANG_GERMAN) 						    PlayMovieInWindow(cmdShow,
					// "movies\\GTAtitlesGER.mpg"); 					    else
					// PlayMovieInWindow(cmdShow, "movies\\GTAtitles.mpg");

					gGameState = GS_INTRO_MPEG;
					TRACE("gGameState = GS_INTRO_MPEG;");
					break;
				}

				case GS_INTRO_MPEG: {
					//					    CPad::UpdatePads();
					//
					//					    if (startupDeactivate || ControlsManager.GetJoyButtonJustDown() != 0)
					++gGameState;
					//					    else if (CPad::GetPad(0)->GetLeftMouseJustDown())
					//						    ++gGameState;
					//					    else if (CPad::GetPad(0)->GetEnterJustDown())
					//						    ++gGameState;
					//					    else if (CPad::GetPad(0)->GetCharJustDown(' '))
					//						    ++gGameState;
					//					    else if (CPad::GetPad(0)->GetAltJustDown())
					//						    ++gGameState;
					//					    else if (CPad::GetPad(0)->GetTabJustDown())
					//						    ++gGameState;

					break;
				}

				case GS_INIT_ONCE: {
					// CoUninitialize();

#ifdef PS2_MENU
					extern char version_name[64];
					if(CGame::frenchGame || CGame::germanGame)
						LoadingScreen(NULL, version_name, "loadsc24");
					else
						LoadingScreen(NULL, version_name, "loadsc0");

					printf("Into TheGame!!!\n");
#else
					LoadingScreen(nil, nil, "loadsc0");
#endif
					if(!CGame::InitialiseOnceAfterRW()) RsGlobal.quit = TRUE;

#ifdef PS2_MENU
					gGameState = GS_INIT_PLAYING_GAME;
#else
					gGameState = GS_INIT_FRONTEND;
					TRACE("gGameState = GS_INIT_FRONTEND;");
#endif
					break;
				}

#ifndef PS2_MENU
				case GS_INIT_FRONTEND: {
					LoadingScreen(nil, nil, "loadsc0");

					FrontEndMenuManager.m_bGameNotLoaded = true;

					CMenuManager::m_bStartUpFrontEndRequested = true;

					if(defaultFullscreenRes) {
						defaultFullscreenRes = FALSE;
						FrontEndMenuManager.m_nPrefsVideoMode = GcurSelVM;
						FrontEndMenuManager.m_nDisplayVideoMode = GcurSelVM;
					}

					gGameState = GS_FRONTEND;
					TRACE("gGameState = GS_FRONTEND;");
					break;
				}

				case GS_FRONTEND: {
					if(!WindowIconified) RsEventHandler(rsFRONTENDIDLE, nil);

#ifdef PS2_MENU
					if(!FrontEndMenuManager.m_bMenuActive || TheMemoryCard.m_bWantToLoad)
#else
					if(!FrontEndMenuManager.m_bMenuActive || FrontEndMenuManager.m_bWantToLoad)
#endif
					{
						gGameState = GS_INIT_PLAYING_GAME;
						TRACE("gGameState = GS_INIT_PLAYING_GAME;");
					}

#ifdef PS2_MENU
					if(TheMemoryCard.m_bWantToLoad)
#else
					if(FrontEndMenuManager.m_bWantToLoad)
#endif
					{
						InitialiseGame();
						FrontEndMenuManager.m_bGameNotLoaded = false;
						gGameState = GS_PLAYING_GAME;
						TRACE("gGameState = GS_PLAYING_GAME;");
					}
					break;
				}
#endif

				case GS_INIT_PLAYING_GAME: {
#ifdef PS2_MENU
					CGame::Initialise("DATA\\GTA3.DAT");

					// LoadingScreen("Starting Game", NULL, GetRandomSplashScreen());

					if(TheMemoryCard.CheckCardInserted(CARD_ONE) == CMemoryCard::NO_ERR_SUCCESS &&
					   TheMemoryCard.ChangeDirectory(CARD_ONE, TheMemoryCard.Cards[CARD_ONE].dir) &&
					   TheMemoryCard.FindMostRecentFileName(CARD_ONE, TheMemoryCard.MostRecentFile) == true &&
					   TheMemoryCard.CheckDataNotCorrupt(TheMemoryCard.MostRecentFile)) {
						strcpy(TheMemoryCard.LoadFileName, TheMemoryCard.MostRecentFile);
						TheMemoryCard.b_FoundRecentSavedGameWantToLoad = true;

						if(CMenuManager::m_PrefsLanguage != TheMemoryCard.GetLanguageToLoad()) {
							CMenuManager::m_PrefsLanguage = TheMemoryCard.GetLanguageToLoad();
							TheText.Unload();
							TheText.Load();
						}

						CGame::currLevel = (eLevelName)TheMemoryCard.GetLevelToLoad();
					}
#else
					InitialiseGame();

					FrontEndMenuManager.m_bGameNotLoaded = false;
#endif
					gGameState = GS_PLAYING_GAME;
					TRACE("gGameState = GS_PLAYING_GAME;");
					break;
				}

				case GS_PLAYING_GAME: {
					float ms = (float)CTimer::GetCurrentTimeInCycles() / (float)CTimer::GetCyclesPerMillisecond();
					if(RwInitialised) {
						if(!CMenuManager::m_PrefsFrameLimiter || (1000.0f / (float)RsGlobal.maxFPS) < ms) {
							// rsIDLE runs the whole frame (update + render +
							// showRaster + present). CPU-only work = total minus
							// the GPU wait and readback/present measured in
							// psCameraShowRaster (feeds the HUD, docs/03).
							double idle0 = psTimer();
							RsEventHandler(rsIDLE, (void *)TRUE);
							gCpuMs = (psTimer() - idle0) - gGpuMs - gReadMs - gPresentMs;
							if(gCpuMs < 0.0) gCpuMs = 0.0;
						}
					}
					break;
				}
				}
			} else {
				if(RwCameraBeginUpdate(Scene.camera)) {
					RwCameraEndUpdate(Scene.camera);
					ForegroundApp = TRUE;
					RsEventHandler(rsACTIVATE, (void *)TRUE);
				}
			}
		}

		/*
		 * About to shut down - block resize events again...
		 */
		RwInitialised = FALSE;

		FrontEndMenuManager.UnloadTextures();
#ifdef PS2_MENU
		if(!(FrontEndMenuManager.m_bWantToRestart || TheMemoryCard.b_FoundRecentSavedGameWantToLoad)) break;
#else
		if(!FrontEndMenuManager.m_bWantToRestart) break;
#endif

		CPad::ResetCheats();
		CPad::StopPadsShaking();

		DMAudio.ChangeMusicMode(MUSICMODE_DISABLE);

#ifdef PS2_MENU
		CGame::ShutDownForRestart();
#endif

		CTimer::Stop();

#ifdef PS2_MENU
		if(FrontEndMenuManager.m_bWantToRestart || TheMemoryCard.b_FoundRecentSavedGameWantToLoad) {
			if(TheMemoryCard.b_FoundRecentSavedGameWantToLoad) {
				FrontEndMenuManager.m_bWantToRestart = true;
				TheMemoryCard.m_bWantToLoad = true;
			}

			CGame::InitialiseWhenRestarting();
			DMAudio.ChangeMusicMode(MUSICMODE_GAME);
			FrontEndMenuManager.m_bWantToRestart = false;

			continue;
		}

		CGame::ShutDown();
		CTimer::Stop();

		break;
#else
		if(FrontEndMenuManager.m_bWantToLoad) {
			CGame::ShutDownForRestart();
			CGame::InitialiseWhenRestarting();
			DMAudio.ChangeMusicMode(MUSICMODE_GAME);
			LoadSplash(GetLevelSplashScreen(CGame::currLevel));
			FrontEndMenuManager.m_bWantToLoad = false;
		} else {
#ifndef MASTER
			if(gbModelViewer)
				CAnimViewer::Shutdown();
			else
#endif
			    if(gGameState == GS_PLAYING_GAME)
				CGame::ShutDown();

			CTimer::Stop();

			if(FrontEndMenuManager.m_bFirstTime == true) {
				gGameState = GS_INIT_FRONTEND;
				TRACE("gGameState = GS_INIT_FRONTEND;");
			} else {
				gGameState = GS_INIT_PLAYING_GAME;
				TRACE("gGameState = GS_INIT_PLAYING_GAME;");
			}
		}

		FrontEndMenuManager.m_bFirstTime = false;
		FrontEndMenuManager.m_bWantToRestart = false;
#endif
	}

#ifndef MASTER
	if(gbModelViewer)
		CAnimViewer::Shutdown();
	else
#endif
	    if(gGameState == GS_PLAYING_GAME)
		CGame::ShutDown();

	DMAudio.Terminate();

	_psFreeVideoModeList();

	/*
	 * Tidy up the 3D (RenderWare) components of the application...
	 */
	RsEventHandler(rsRWTERMINATE, nil);

	/*
	 * Free the platform dependent data...
	 */
	RsEventHandler(rsTERMINATE, nil);

#ifdef _WIN32
	/*
	 * Free the argv strings...
	 */
	free(argv);

	SystemParametersInfo(SPI_SETSTICKYKEYS, sizeof(STICKYKEYS), &SavedStickyKeys, SPIF_SENDCHANGE);
	SystemParametersInfo(SPI_SETPOWEROFFACTIVE, TRUE, nil, SPIF_SENDCHANGE);
	SystemParametersInfo(SPI_SETLOWPOWERACTIVE, TRUE, nil, SPIF_SENDCHANGE);
	SetErrorMode(0);
#endif

	return 0;
}

/*
 *****************************************************************************
 */

RwV2d leftStickPos;
RwV2d rightStickPos;

// CPad::UpdatePads calls this every frame. Delegate to the active input
// source's optional gamepad hook (e.g. GPIO writes PCTempJoyState directly,
// docs/08 method A). Keyboard sources (evdev/sdl) inject via the event chain
// and leave this hook null.
void
CapturePad(RwInt32 padID)
{
	InputSource_CapturePadAll((int)padID);
}
#endif
