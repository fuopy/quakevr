/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2019 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#ifndef QUAKEDEFS_H
#define QUAKEDEFS_H

#include "vr_macros.hpp"

// quakedef.h -- primary header for client

#define QUAKE_GAME // as opposed to utilities

#define VERSION 1.09
#define GLQUAKE_VERSION 1.00
#define D3DQUAKE_VERSION 0.01
#define WINQUAKE_VERSION 0.996
#define LINUX_VERSION 1.30
#define X11_VERSION 1.10

#define FITZQUAKE_VERSION 0.85 // johnfitz
#define QUAKESPASM_VERSION 0.93
#define QUAKEVR_VERSION "0.0.5"
#define QUAKESPASM_VER_PATCH 2 // helper to print a string like 0.93.2
#ifndef QUAKESPASM_VER_SUFFIX
#define QUAKESPASM_VER_SUFFIX // optional version suffix string literal like
                              // "-beta1"
#endif

#define QS_STRINGIFY_(x) #x
#define QS_STRINGIFY(x) QS_STRINGIFY_(x)

// combined version string like "0.92.1-beta1"
#define QUAKESPASM_VER_STRING                                    \
    QS_STRINGIFY(QUAKESPASM_VERSION)                             \
    "." QS_STRINGIFY(QUAKESPASM_VER_PATCH) QUAKESPASM_VER_SUFFIX \
        " | Quake VR " QUAKEVR_VERSION

// #define PARANOID // speed sapping error checking

#define GAMENAME "id1" // directory to look in by default

#include "q_stdinc.hpp"

// !!! if this is changed, it must be changed in d_ifacea.h too !!!
#define CACHE_SIZE 32 // used to align key data structures

#define Q_UNUSED(x) (x = x) // for pesky compiler / lint warnings

#define MINIMUM_MEMORY 1'048'576 // 16 MB
#define MINIMUM_MEMORY_LEVELPAK (MINIMUM_MEMORY + 0x100000)

#define MAX_NUM_ARGVS 50

// up / down
#define PITCH 0

// left / right
#define YAW 1

// fall over
#define ROLL 2


#define MAX_QPATH 128 // max length of a quake game pathname

#define ON_EPSILON 0.1 // point on plane side epsilon

#define DIST_EPSILON \
    (0.03125f) // 1/32 epsilon to keep floating point happy (moved from world.c)

#define MAX_MSGLEN \
    96000 // max length of a reliable message //ericw -- was 32000
#define MAX_DATAGRAM \
    64000 // max length of unreliable message //johnfitz -- was 1024

#define DATAGRAM_MTU \
    1400 // johnfitz -- actual limit for unreliable messages to nonlocal clients

//
// per-level limits
//
#define MIN_EDICTS 256 // johnfitz -- lowest allowed value for max_edicts cvar
#define MAX_EDICTS \
    32000 // johnfitz -- highest allowed value for max_edicts cvar
          // ents past 8192 can't play sounds in the standard protocol
#define MAX_LIGHTSTYLES 64
#define MAX_MODELS 2048 // johnfitz -- was 256
#define MAX_SOUNDS 2048 // johnfitz -- was 256

#define SAVEGAME_COMMENT_LENGTH 39

#define MAX_STYLESTRING 64

//
// stats are integers communicated to the client by the server
//
#define MAX_CL_STATS 64

#define STAT_HEALTH 0
#define STAT_FRAGS 1
#define STAT_WEAPON 2
#define STAT_AMMO 3
#define STAT_ARMOR 4
#define STAT_WEAPONFRAME 5
#define STAT_SHELLS 6
#define STAT_NAILS 7
#define STAT_ROCKETS 8
#define STAT_CELLS 9
#define STAT_ACTIVEWEAPON 10
#define STAT_TOTALSECRETS 11
#define STAT_TOTALMONSTERS 12
#define STAT_SECRETS 13  // bumped on client side by svc_foundsecret
#define STAT_MONSTERS 14 // bumped by svc_killedmonster
#define STAT_WEAPON2 15
#define STAT_WEAPONMODEL2 16
#define STAT_WEAPONFRAME2 17
#define STAT_HOLSTERWEAPON0 18
#define STAT_HOLSTERWEAPON1 19
#define STAT_HOLSTERWEAPON2 20
#define STAT_HOLSTERWEAPON3 21
#define STAT_HOLSTERWEAPONMODEL0 22
#define STAT_HOLSTERWEAPONMODEL1 23
#define STAT_HOLSTERWEAPONMODEL2 24
#define STAT_HOLSTERWEAPONMODEL3 25
#define STAT_AMMO2 26
#define STAT_AMMOCOUNTER 27
#define STAT_AMMOCOUNTER2 28
#define STAT_HOLSTERWEAPON4 29
#define STAT_HOLSTERWEAPON5 30
#define STAT_HOLSTERWEAPONMODEL4 31
#define STAT_HOLSTERWEAPONMODEL5 32
#define STAT_MAINHAND_WID 33
#define STAT_OFFHAND_WID 34
#define STAT_WEAPONFLAGS 35
#define STAT_WEAPONFLAGS2 36
#define STAT_HOLSTERWEAPONFLAGS0 37
#define STAT_HOLSTERWEAPONFLAGS1 38
#define STAT_HOLSTERWEAPONFLAGS2 39
#define STAT_HOLSTERWEAPONFLAGS3 40
#define STAT_HOLSTERWEAPONFLAGS4 41
#define STAT_HOLSTERWEAPONFLAGS5 42

// stock defines
//
// clang-format off
#define IT_SHOTGUN          VRUTIL_POWER_OF_TWO(0)
#define IT_SUPER_SHOTGUN    VRUTIL_POWER_OF_TWO(1)
#define IT_NAILGUN          VRUTIL_POWER_OF_TWO(2)
#define IT_SUPER_NAILGUN    VRUTIL_POWER_OF_TWO(3)
#define IT_GRENADE_LAUNCHER VRUTIL_POWER_OF_TWO(4)
#define IT_ROCKET_LAUNCHER  VRUTIL_POWER_OF_TWO(5)
#define IT_LIGHTNING        VRUTIL_POWER_OF_TWO(6)
#define IT_SUPER_LIGHTNING  VRUTIL_POWER_OF_TWO(7)
#define IT_SHELLS           VRUTIL_POWER_OF_TWO(8)
#define IT_NAILS            VRUTIL_POWER_OF_TWO(9)
#define IT_ROCKETS          VRUTIL_POWER_OF_TWO(10)
#define IT_CELLS            VRUTIL_POWER_OF_TWO(11)
#define IT_AXE              VRUTIL_POWER_OF_TWO(12)
#define IT_ARMOR1           VRUTIL_POWER_OF_TWO(13)
#define IT_ARMOR2           VRUTIL_POWER_OF_TWO(14)
#define IT_ARMOR3           VRUTIL_POWER_OF_TWO(15)
#define IT_SUPERHEALTH      VRUTIL_POWER_OF_TWO(16)
#define IT_KEY1             VRUTIL_POWER_OF_TWO(17)
#define IT_KEY2             VRUTIL_POWER_OF_TWO(18)
#define IT_INVISIBILITY     VRUTIL_POWER_OF_TWO(19)
#define IT_INVULNERABILITY  VRUTIL_POWER_OF_TWO(20)
#define IT_SUIT             VRUTIL_POWER_OF_TWO(21)
#define IT_QUAD             VRUTIL_POWER_OF_TWO(22)
// clang-format on
#define IT_SIGIL1 (1 << 28)
#define IT_SIGIL2 (1 << 29)
#define IT_SIGIL3 (1 << 30)
#define IT_SIGIL4 (1 << 31)

//===========================================
// rogue changed and added defines

#define RIT_SHELLS 128
#define RIT_NAILS 256
#define RIT_ROCKETS 512
#define RIT_CELLS 1024
#define RIT_AXE 2048
#define RIT_LAVA_NAILGUN 4096
#define RIT_LAVA_SUPER_NAILGUN 8192
#define RIT_MULTI_GRENADE 16384
#define RIT_MULTI_ROCKET 32768
#define RIT_PLASMA_GUN 65536
#define RIT_ARMOR1 8388608
#define RIT_ARMOR2 16777216
#define RIT_ARMOR3 33554432
#define RIT_LAVA_NAILS 67108864
#define RIT_PLASMA_AMMO 134217728
#define RIT_MULTI_ROCKETS 268435456
#define RIT_SHIELD 536870912
#define RIT_ANTIGRAV 1073741824
#define RIT_SUPERHEALTH 2147483648

// MED 01/04/97 added hipnotic defines
//===========================================
// hipnotic added defines
#define HIT_PROXIMITY_GUN_BIT 16
#define HIT_MJOLNIR_BIT 7
#define HIT_LASER_CANNON_BIT 23
#define HIT_PROXIMITY_GUN (1 << HIT_PROXIMITY_GUN_BIT) // 65536
#define HIT_MJOLNIR (1 << HIT_MJOLNIR_BIT)
#define HIT_LASER_CANNON (1 << HIT_LASER_CANNON_BIT)
#define HIT_WETSUIT (1 << (23 + 2))
#define HIT_EMPATHY_SHIELDS (1 << (23 + 3))

//===========================================

// weapon ids
#define WID_FIST 0
#define WID_GRAPPLE 1
#define WID_AXE 2
#define WID_MJOLNIR 3
#define WID_SHOTGUN 4
#define WID_SUPER_SHOTGUN 5
#define WID_NAILGUN 6
#define WID_SUPER_NAILGUN 7
#define WID_GRENADE_LAUNCHER 8
#define WID_PROXIMITY_GUN 9
#define WID_ROCKET_LAUNCHER 10
#define WID_LIGHTNING 11
#define WID_LASER_CANNON 12

// ammo ids
#define AID_NONE 0
#define AID_SHELLS 1
#define AID_NAILS 2
#define AID_ROCKETS 3
#define AID_CELLS 4
#define AID_LAVA_NAILS 5
#define AID_MULTI_ROCKETS 6
#define AID_PLASMA 7

// Quake VR hotspots
#define QVR_HS_NONE 0
#define QVR_HS_OFFHAND_2H_GRAB 1  // 2H grab - helper offhand
#define QVR_HS_MAINHAND_2H_GRAB 2 // 2H grab - helper mainhand
#define QVR_HS_LEFT_SHOULDER_HOLSTER 3
#define QVR_HS_RIGHT_SHOULDER_HOLSTER 4
#define QVR_HS_LEFT_HIP_HOLSTER 5
#define QVR_HS_RIGHT_HIP_HOLSTER 6
#define QVR_HS_HAND_SWITCH 7
#define QVR_HS_LEFT_UPPER_HOLSTER 8
#define QVR_HS_RIGHT_UPPER_HOLSTER 9

// Quake VR - vrbits0 bits
// clang-format off
#define QVR_VRBITS0_TELEPORTING           VRUTIL_POWER_OF_TWO(0)
#define QVR_VRBITS0_OFFHAND_GRABBING      VRUTIL_POWER_OF_TWO(1)
#define QVR_VRBITS0_OFFHAND_PREVGRABBING  VRUTIL_POWER_OF_TWO(2)
#define QVR_VRBITS0_MAINHAND_GRABBING     VRUTIL_POWER_OF_TWO(3)
#define QVR_VRBITS0_MAINHAND_PREVGRABBING VRUTIL_POWER_OF_TWO(4)
#define QVR_VRBITS0_2H_AIMING             VRUTIL_POWER_OF_TWO(5)
// clang-format on

#define MAX_SCOREBOARD 16
#define MAX_SCOREBOARDNAME 32

#define SOUND_CHANNELS 8

struct quakeparms_t
{
    const char* basedir;
    const char* userdir; // user's directory on UNIX platforms.
                         // if user directories are enabled, basedir
                         // and userdir will point to different
                         // memory locations, otherwise to the same.
    int argc;
    char** argv;
    void* membase;
    int memsize;
    int numcpus;
    int errstate;
};

#include "common.hpp"
#include "bspfile.hpp"
#include "sys.hpp"
#include "zone.hpp"
#include "mathlib.hpp"
#include "cvar.hpp"

#include "protocol.hpp"
#include "net.hpp"

#include "cmd.hpp"
#include "crc.hpp"

#include "progs.hpp"
#include "server.hpp"

#include "platform.hpp"

#include <GL/glew.h>

// #define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL.h>
// #include <SDL2/SDL_opengl.h>
// #include <SDL2/SDL_opengl_glext.h>
// #include <SDL2/SDL_opengles2.h>

#include "console.hpp"
#include "wad.hpp"
#include "vid.hpp"
#include "screen.hpp"
#include "draw.hpp"
#include "render.hpp"
#include "view.hpp"
#include "sbar.hpp"
#include "q_sound.hpp"
#include "client.hpp"

#include "gl_model.hpp"
#include "world.hpp"

#include "image.hpp"     //johnfitz
#include "gl_texmgr.hpp" //johnfitz
#include "input.hpp"
#include "keys.hpp"
#include "menu.hpp"
#include "cdaudio.hpp"
#include "glquake.hpp"


//=============================================================================

// the host system specifies the base of the directory tree, the
// command line parms passed to the program, and the amount of memory
// available for the program to use

extern bool noclip_anglehack;

//
// host
//
extern quakeparms_t* host_parms;

extern cvar_t sys_ticrate;
extern cvar_t sys_throttle;
extern cvar_t sys_nostdout;
extern cvar_t developer;
extern cvar_t max_edicts; // johnfitz

extern bool host_initialized; // true if into command execution
extern double host_frametime;
extern byte* host_colormap;
extern int host_framecount; // incremented every frame, never reset
extern double realtime;     // not bounded in any way, changed at
                            // start of every frame, never reset

typedef struct filelist_item_s
{
    char name[32];
    struct filelist_item_s* next;
} filelist_item_t;

extern filelist_item_t* modlist;
extern filelist_item_t* extralevels;
extern filelist_item_t* demolist;

void Host_ClearMemory();
void Host_ServerFrame();
void Host_InitCommands();
void Host_Init();
void Host_Shutdown();
void Host_Callback_Notify(cvar_t* var); /* callback function for CVAR_NOTIFY */
void Host_Warn(const char* error, ...) FUNC_PRINTF(1, 2);
[[noreturn]] void Host_Error(const char* error, ...) FUNC_PRINTF(1, 2);
[[noreturn]] void Host_EndGame(const char* message, ...) FUNC_PRINTF(1, 2);
#ifdef __WATCOMC__
#pragma aux Host_Error aborts;
#pragma aux Host_EndGame aborts;
#endif
void Host_Frame(float time);
void Host_Quit_f();
void Host_ClientCommands(const char* fmt, ...) FUNC_PRINTF(1, 2);
void Host_ShutdownServer(bool crash);
void Host_WriteConfiguration();

void ExtraMaps_Init();
void Modlist_Init();
void DemoList_Init();

void DemoList_Rebuild();

extern int current_skill; // skill level for currently loaded level (in case
                          //  the user changes the cvar while the level is
                          //  running, this reflects the level actually in use)

extern bool isDedicated;

extern int minimum_memory;

// johnfitz -- struct for passing lerp information to drawing functions
struct lerpdata_t
{
    short pose1;
    short pose2;
    float blend;
    qvec3 origin;
    qvec3 angles;
};
// johnfitz


#endif /* QUAKEDEFS_H */
