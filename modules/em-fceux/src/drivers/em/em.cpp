/* FCE Ultra - NES/Famicom Emulator - Emscripten main
 *
 * Copyright notice for this file:
 *  Copyright (C) 2015 Valtteri "tsone" Heikkila
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include "em.h"
#include "../../fceu.h"
#include "../../version.h"
#include <emscripten.h>
#include <emscripten/bind.h>


// Number of frames to skip per regular frame when frameskipping.
#define THROTTLE_FRAMESKIPS 3
// Number of frames to hold Zapper trigger.
// Reason is async mouse events and game implementations.
#define ZAPPER_HOLD_FRAMES 3

extern bool replaceP2StartWithMicrophone; // From src/input.cpp.
extern int CurrentState; // From src/state.cpp.

static bool em_throttling = false;
static bool em_frame_advance = false;
int em_scanlines = 224; // Default is NTSC, 224.
std::string em_video_system = "auto";

static uint32 em_controller_bits = 0;
static uint32 em_zapper[3] = {};
static int em_zapper_hold_counter = 0;

static bool s_game_loaded = false;

EMUFILE_FILE* FCEUD_UTF8_fstream(const char *fn, const char *m)
{
// TODO: tsone: mode variable is not even used, remove?
    std::ios_base::openmode mode = std::ios_base::binary;
    if(!strcmp(m,"r") || !strcmp(m,"rb"))
        mode |= std::ios_base::in;
    else if(!strcmp(m,"w") || !strcmp(m,"wb"))
        mode |= std::ios_base::out | std::ios_base::trunc;
    else if(!strcmp(m,"a") || !strcmp(m,"ab"))
        mode |= std::ios_base::out | std::ios_base::app;
    else if(!strcmp(m,"r+") || !strcmp(m,"r+b"))
        mode |= std::ios_base::in | std::ios_base::out;
    else if(!strcmp(m,"w+") || !strcmp(m,"w+b"))
        mode |= std::ios_base::in | std::ios_base::out | std::ios_base::trunc;
    else if(!strcmp(m,"a+") || !strcmp(m,"a+b"))
        mode |= std::ios_base::in | std::ios_base::out | std::ios_base::app;
    return new EMUFILE_FILE(fn, m);
}

FILE *FCEUD_UTF8fopen(const char *fn, const char *mode)
{
    return fopen(fn, mode);
}

const char *FCEUD_GetCompilerString()
{
    // Required to identify compiler.
    static const char compiler_string[] = "emscripten " __VERSION__;
    return compiler_string;
}

const unsigned int *GetKeyboard()
{
    // Required for src/boards/transformer.cpp. Must return int[256].
    static const unsigned int keyboard[256] = {};
    return keyboard;
}

uint64 FCEUD_GetTime()
{
    return emscripten_get_now();
}

uint64 FCEUD_GetTimeFreq(void)
{
    return 1000; // emscripten_get_now() returns milliseconds.
}

void FCEUD_Message(const char *text)
{
    fputs(text, stdout);
}

void FCEUD_PrintError(const char *errormsg)
{
    fprintf(stderr, "%s\n", errormsg);
}

static int CloseGame()
{
    if (!s_game_loaded) {
        return 0;
    }
    s_game_loaded = false;

    FCEUI_CloseGame();

    // DriverKill();
    GameInfo = 0;
    return 1;
}

static int EmulateFrame(int frameskipmode)
{
    uint8 *gfx = 0;
    int32 *sound;
    int32 ssize;

    FCEUI_Emulate(&gfx, &sound, &ssize, frameskipmode);
    Audio_Write(sound, ssize);
    return ssize;
}

// Event

static void Event_dispatch(const char* name)
{
    EM_ASM({
        const name = UTF8ToString($0);
        if (Module.eventListeners.hasOwnProperty(name)) {
            Module.eventListeners[name]();
        }
    }, name);
}

// System

static void CreateDirs(const std::string &dir)
{
    const char *subs[] = { "fcs", "snaps", "gameinfo", "sav", "cheats", "movies", "cfg.d" };

    mkdir(dir.c_str(), S_IRWXU);
    for (int i = 0; i < sizeof(subs) / sizeof(*subs); i++) {
        const std::string subdir = dir + PSS + subs[i];
        mkdir(subdir.c_str(), S_IRWXU);
    }

    EM_ASM({
        const dir = '/tmp/sav';
        FS.mkdir(dir);
        FS.mount(MEMFS, {}, dir);
    });
}

static void SetPort(int port, const std::string& deviceName)
{
    uint32 *ptr;
    ESI peri;

    if (deviceName == "controller") {
        peri = SI_GAMEPAD;
        ptr = &em_controller_bits;
    } else if (deviceName == "zapper") {
        peri = SI_ZAPPER;
        ptr = em_zapper;
    } else {
        FCEU_PrintError("Invalid device name: '%s'", deviceName.c_str());
        return;
    }

    FCEUI_SetInput(port, peri, ptr, 0);
}

static bool System_Init(const std::string& canvasQuerySelector)
{
    const std::string base_dir = getenv("HOME") + std::string(PSS ".fceux");

    FCEUD_Message("Starting " FCEU_NAME_AND_VERSION "...\n");

    FCEUI_SetBaseDirectory(base_dir.c_str());
    CreateDirs(base_dir);

    FCEUI_Initialize();

    FCEUI_SetDirOverride(FCEUIOD_NV, "/tmp/sav");
    FCEUI_SetDirOverride(FCEUIOD_STATES, "/tmp/sav");

    FCEUI_SetVidSystem(0);
    FCEUI_SetGameGenie(0);
    FCEUI_SetLowPass(0);
    FCEUI_DisableSpriteLimitation(0);

    const int start = 0, end = 239;
    FCEUI_SetRenderedLines(start + 8, end - 8, start, end);

    SetPort(0, "controller");

    // TODO: Use the new PPU? Last time tried it didn't work...
    newppu = 0;

    Audio_Init();

    // Set default config and create Web Audio context.
    if (!EM_ASM_INT({
        for (let key in Module._defaultConfig) {
            Module.setConfig(key, Module._defaultConfig[key]);
        }
    })) {
        FCEUD_PrintError("Failed to create Web Audio context. You must call init() from an user event.");
        return false;
    }

    return true;
}

static void UpdateFrameAdvance()
{
    if (em_frame_advance) {
        FCEUI_FrameAdvanceEnd();
        em_frame_advance = false;
    }
}

static void UpdateZapper()
{
    if (em_zapper_hold_counter <= 0) {
        em_zapper[2] = 0;
    } else {
        --em_zapper_hold_counter;
    }
}

extern uint8 *XBuf;
extern void BlitScreen(uint8 *XBuf);

static int System_Update()
{
    int ret = 0;
    if (GameInfo) {
        ret = EmulateFrame(0);
	    if(XBuf) BlitScreen(XBuf);
    }

    UpdateFrameAdvance();
    UpdateZapper();

    return ret;
}

static bool System_SetConfig(const std::string& key, const emscripten::val& value)
{
    if (key == "system-port-2") {
        SetPort(1, value.as<std::string>());
    } else {
        return false;
    }
    return 1;
}

void System_LoadGameSave();
void System_SaveGameSave();

/**
 * Loads a game, given a full path/filename. The driver code must be
 * initialized after the game is loaded, because the emulator code
 * provides data necessary for the driver code(number of scanlines to
 * render, what virtual input devices to use, etc.).
 */
static bool System_LoadGame(const std::string &path)
{
    if (s_game_loaded) {
        CloseGame();
    }

    if(!FCEUI_LoadGame(path.c_str(), em_video_system == "auto")) {
        return false;
    }

    Audio_UpdateSoundRate();

    EM_ASM({ Module.deleteSaveFiles(); });
    s_game_loaded = true;
    Event_dispatch("game-loaded");
    return true;
}

static bool System_StartGame()
{
    return System_LoadGame("/tmp/rom");
}

static std::string System_GameMd5()
{
    return (s_game_loaded ? md5_asciistr(GameInfo->MD5) : "");
}

static void System_Reset()
{
    FCEUI_ResetNES();
}

static void System_SetControllerBits(uint32_t bits)
{
    em_controller_bits = bits;
}

static void System_TriggerZapper(int x, int y)
{
    em_zapper[0] = x;
    em_zapper[1] = y;
    em_zapper[2] = 1;

//    Video_CanvasToNESCoords(&em_zapper[0], &em_zapper[1]);

    em_zapper_hold_counter = ZAPPER_HOLD_FRAMES;
}

static void System_SetThrottling(bool throttling)
{
    em_throttling = throttling;
}

static bool System_Throttling()
{
    return em_throttling;
}

static void System_SetPaused(bool pause)
{
    EmulationPaused = pause;
}

static bool System_Paused()
{
    return EmulationPaused;
}

static void System_AdvanceFrame()
{
    if (!em_frame_advance) {
        FCEUI_FrameAdvance();
        em_frame_advance = true; // Toggled off at update().
    }
}

static void System_SetState(int index)
{
    FCEUI_SelectState(index, 1);
}

static void System_LoadState()
{
    if (GameInfo && GameInfo->type != GIT_NSF) {
        FCEUI_LoadState("/tmp/state.fcs");
    }
}

static void System_SaveState()
{
    if (GameInfo && GameInfo->type != GIT_NSF) {
        FCEUI_SaveState("/tmp/state.fcs");
    }
}

// Bindings

static bool SetConfig(const std::string& key, const emscripten::val& value)
{
    if (!Video_SetConfig(key, value) && !System_SetConfig(key, value)) {
        FCEU_PrintError("Invalid config: '%s'", key.c_str());
        return false;
    }
    return true;
}

EMSCRIPTEN_BINDINGS(fceux)
{
    emscripten::function("init", &System_Init);
    emscripten::function("update", &System_Update);

    emscripten::function("gameMd5", &System_GameMd5);

    emscripten::function("reset", &System_Reset);

    emscripten::function("setControllerBits", &System_SetControllerBits);

    emscripten::function("triggerZapper", &System_TriggerZapper);

    emscripten::function("setThrottling", &System_SetThrottling);
    emscripten::function("throttling", &System_Throttling);

    emscripten::function("setPaused", &System_SetPaused);
    emscripten::function("paused", &System_Paused);

    emscripten::function("advanceFrame", &System_AdvanceFrame);

    emscripten::function("setState", &System_SetState);
    emscripten::function("saveState", &System_SaveState);
    emscripten::function("loadState", &System_LoadState);

    emscripten::function("setConfig", &SetConfig);

    // For internal use.
    emscripten::function("_loadGameSave", &System_LoadGameSave);
    emscripten::function("_saveGameSave", &System_SaveGameSave);
    emscripten::function("_startGame", &System_StartGame);
    emscripten::function("_saveState", &System_SaveState);
    emscripten::function("_loadState", &System_LoadState);

}

// NOTE: Following are non-implemented "dummy" driver functions.
void LagCounterReset() {}
bool FCEUI_AviIsRecording(void) { return false; }
bool FCEUD_PauseAfterPlayback() { return false; }
// These are actually fine, but will be unused and overriden by the current UI code.
FCEUFILE* FCEUD_OpenArchiveIndex(ArchiveScanRecord& asr, std::string &fname, int innerIndex) { return 0; }
FCEUFILE* FCEUD_OpenArchive(ArchiveScanRecord& asr, std::string& fname, std::string* innerFilename) { return 0; }
ArchiveScanRecord FCEUD_ScanArchive(std::string fname) { return ArchiveScanRecord(); }
