/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2005 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2020-2021 Vittorio Romeo

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

#include "host.hpp"
#include "arch_def.hpp"
#include "quakedef.hpp"
#include "platform.hpp"
#include "quakeparms.hpp"
#include "input.hpp"
#include "progs.hpp"
#include "console.hpp"
#include "sys.hpp"
#include "vid.hpp"
#include "keys.hpp"
#include "common.hpp"
#include "qcvm.hpp"

#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#ifdef PLATFORM_OSX
#include <libgen.h> /* dirname() and basename() */
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <time.h>
#ifdef DO_USERDIRS
#include <pwd.h>
#endif

#include <SDL2/SDL.h>

// Boost temporarily disabled --f
//#include <boost/stacktrace.hpp>
#include <iostream>

bool isDedicated;
cvar_t sys_throttle = {"sys_throttle", "0.02", CVAR_ARCHIVE};

static size_t
    sys_handles_max; /* spike -- removed limit, was 32 (johnfitz -- was 10) */
static FILE** sys_handles;
static int findhandle()
{
    size_t i, n;

    for(i = 1; i < sys_handles_max; i++)
    {
        if(!sys_handles[i])
        {
            return i;
        }
    }
    n = sys_handles_max + 10;
    sys_handles = (FILE**)realloc(sys_handles, sizeof(*sys_handles) * n);
    if(!sys_handles)
    {
        Sys_Error("out of handles");
    }
    while(sys_handles_max < n)
    {
        sys_handles[sys_handles_max++] = nullptr;
    }
    return i;
}

long Sys_filelength(FILE* f)
{
    long pos, end;

    pos = ftell(f);
    fseek(f, 0, SEEK_END);
    end = ftell(f);
    fseek(f, pos, SEEK_SET);

    return end;
}

int Sys_FileOpenRead(const char* path, int* hndl)
{
    FILE* f;
    int i, retval;

    i = findhandle();
    f = fopen(path, "rb");

    if(!f)
    {
        *hndl = -1;
        retval = -1;
    }
    else
    {
        sys_handles[i] = f;
        *hndl = i;
        retval = Sys_filelength(f);
    }

    return retval;
}

int Sys_FileOpenWrite(const char* path)
{
    FILE* f;
    int i;

    i = findhandle();
    f = fopen(path, "wb");

    if(!f)
    {
        Sys_Error("Error opening %s: %s", path, strerror(errno));
    }

    sys_handles[i] = f;
    return i;
}

int Sys_FileOpenStdio(FILE* file)
{
    int i;
    i = findhandle();
    sys_handles[i] = file;
    return i;
}

void Sys_FileClose(int handle)
{
    fclose(sys_handles[handle]);
    sys_handles[handle] = nullptr;
}

void Sys_FileSeek(int handle, int position)
{
    fseek(sys_handles[handle], position, SEEK_SET);
}

int Sys_FileRead(int handle, void* dest, int count)
{
    return fread(dest, 1, count, sys_handles[handle]);
}

int Sys_FileWrite(int handle, const void* data, int count)
{
    return fwrite(data, 1, count, sys_handles[handle]);
}

int Sys_FileTime(const char* path)
{
    FILE* f;

    f = fopen(path, "rb");

    if(f)
    {
        fclose(f);
        return 1;
    }

    return -1;
}


#if defined(__linux__) || defined(__sun) || defined(sun) || defined(_AIX)
static int Sys_NumCPUs()
{
    int numcpus = sysconf(_SC_NPROCESSORS_ONLN);
    return (numcpus < 1) ? 1 : numcpus;
}

#elif defined(PLATFORM_OSX)
#include <sys/sysctl.h>
#if !defined(HW_AVAILCPU) /* using an ancient SDK? */
#define HW_AVAILCPU 25    /* needs >= 10.2 */
#endif
static int Sys_NumCPUs()
{
    int numcpus;
    int mib[2];
    size_t len;

#if defined(_SC_NPROCESSORS_ONLN) /* needs >= 10.5 */
    numcpus = sysconf(_SC_NPROCESSORS_ONLN);
    if(numcpus != -1) return (numcpus < 1) ? 1 : numcpus;
#endif
    len = sizeof(numcpus);
    mib[0] = CTL_HW;
    mib[1] = HW_AVAILCPU;
    sysctl(mib, 2, &numcpus, &len, nullptr, 0);
    if(sysctl(mib, 2, &numcpus, &len, nullptr, 0) == -1)
    {
        mib[1] = HW_NCPU;
        if(sysctl(mib, 2, &numcpus, &len, nullptr, 0) == -1) return 1;
    }
    return (numcpus < 1) ? 1 : numcpus;
}

#elif defined(__sgi) || defined(sgi) || defined(__sgi__) /* IRIX */
static int Sys_NumCPUs()
{
    int numcpus = sysconf(_SC_NPROC_ONLN);
    if(numcpus < 1) numcpus = 1;
    return numcpus;
}

#elif defined(PLATFORM_BSD)
#include <sys/sysctl.h>
static int Sys_NumCPUs()
{
    int numcpus;
    int mib[2];
    size_t len;

#if defined(_SC_NPROCESSORS_ONLN)
    numcpus = sysconf(_SC_NPROCESSORS_ONLN);
    if(numcpus != -1) return (numcpus < 1) ? 1 : numcpus;
#endif
    len = sizeof(numcpus);
    mib[0] = CTL_HW;
    mib[1] = HW_NCPU;
    if(sysctl(mib, 2, &numcpus, &len, nullptr, 0) == -1) return 1;
    return (numcpus < 1) ? 1 : numcpus;
}

#elif defined(__hpux) || defined(__hpux__) || defined(_hpux)
#include <sys/mpctl.h>
static int Sys_NumCPUs()
{
    int numcpus = mpctl(MPC_GETNUMSPUS, nullptr, nullptr);
    return numcpus;
}

#else /* unknown OS */
static int Sys_NumCPUs()
{
    return -2;
}
#endif

static char cwd[MAX_OSPATH];
#ifdef DO_USERDIRS
static char userdir[MAX_OSPATH];
#ifdef PLATFORM_OSX
#define SYS_USERDIR "Library/Application Support/QuakeSpasm"
#else
#define SYS_USERDIR ".quakespasm"
#endif

static void Sys_GetUserdir(char* dst, size_t dstsize)
{
    size_t n;
    const char* home_dir = nullptr;
    struct passwd* pwent;

    pwent = getpwuid(getuid());
    if(pwent == nullptr)
        perror("getpwuid");
    else
        home_dir = pwent->pw_dir;
    if(home_dir == nullptr) home_dir = getenv("HOME");
    if(home_dir == nullptr) Sys_Error("Couldn't determine userspace directory");

    /* what would be a maximum path for a file in the user's directory...
     * $HOME/SYS_USERDIR/game_dir/dirname1/dirname2/dirname3/filename.ext
     * still fits in the MAX_OSPATH == 256 definition, but just in case :
     */
    n = strlen(home_dir) + strlen(SYS_USERDIR) + 50;
    if(n >= dstsize)
        Sys_Error("Insufficient array size for userspace directory");

    q_snprintf(dst, dstsize, "%s/%s", home_dir, SYS_USERDIR);
}
#endif /* DO_USERDIRS */

#ifdef PLATFORM_OSX
static char* OSX_StripAppBundle(char* dir)
{ /* based on the ioquake3 project at icculus.org. */
    static char osx_path[MAX_OSPATH];

    q_strlcpy(osx_path, dir, sizeof(osx_path));
    if(strcmp(basename(osx_path), "MacOS")) return dir;
    q_strlcpy(osx_path, dirname(osx_path), sizeof(osx_path));
    if(strcmp(basename(osx_path), "Contents")) return dir;
    q_strlcpy(osx_path, dirname(osx_path), sizeof(osx_path));
    if(!strstr(basename(osx_path), ".app")) return dir;
    q_strlcpy(osx_path, dirname(osx_path), sizeof(osx_path));
    return osx_path;
}

static void Sys_GetBasedir(char* argv0, char* dst, size_t dstsize)
{
    char* tmp;

    if(realpath(argv0, dst) == nullptr)
    {
        perror("realpath");
        if(getcwd(dst, dstsize - 1) == nullptr)
        _fail:
            Sys_Error("Couldn't determine current directory");
    }
    else
    {
        /* strip off the binary name */
        if(!(tmp = strdup(dst))) goto _fail;
        q_strlcpy(dst, dirname(tmp), dstsize);
        free(tmp);
    }

    tmp = OSX_StripAppBundle(dst);
    if(tmp != dst) q_strlcpy(dst, tmp, dstsize);
}
#else
static void Sys_GetBasedir(char* argv0, char* dst, size_t dstsize)
{
    (void)argv0;

    char* tmp;

    if(getcwd(dst, dstsize - 1) == nullptr)
        Sys_Error("Couldn't determine current directory");

    tmp = dst;
    while(*tmp != 0)
    {
        tmp++;
    }
    while(*tmp == 0 && tmp != dst)
    {
        --tmp;
        if(tmp != dst && *tmp == '/')
        {
            *tmp = 0;
        }
    }
}
#endif

void Sys_Init()
{
    memset(cwd, 0, sizeof(cwd));
    Sys_GetBasedir(host_parms->argv[0], cwd, sizeof(cwd));
    host_parms->basedir = cwd;
#ifndef DO_USERDIRS
    host_parms->userdir =
        host_parms->basedir; /* code elsewhere relies on this ! */
#else
    memset(userdir, 0, sizeof(userdir));
    Sys_GetUserdir(userdir, sizeof(userdir));
    Sys_mkdir(userdir);
    host_parms->userdir = userdir;
#endif
    host_parms->numcpus = Sys_NumCPUs();
    Sys_Printf("Detected %d CPUs.\n", host_parms->numcpus);
}

void Sys_mkdir(const char* path)
{
    int rc = mkdir(path, 0777);
    if(rc != 0 && errno == EEXIST)
    {
        struct stat st;
        if(stat(path, &st) == 0 && S_ISDIR(st.st_mode)) rc = 0;
    }
    if(rc != 0)
    {
        rc = errno;
        Sys_Error("Unable to create directory %s: %s", path, strerror(rc));
    }
}

static const char errortxt1[] = "\nERROR-OUT BEGIN\n\n";
static const char errortxt2[] = "\nQUAKE ERROR: ";

void Sys_Error(const char* error, ...)
{
//    std::cout << "Stacktrace:\n"
//              << boost::stacktrace::stacktrace() << std::endl;

    va_list argptr;
    char text[1024];

    Con_Redirect(nullptr);
    PR_SwitchQCVM(nullptr);
    host_parms->errstate++;

    va_start(argptr, error);
    q_vsnprintf(text, sizeof(text), error, argptr);
    va_end(argptr);

    fputs(errortxt1, stderr);
    Host_Shutdown();
    fputs(errortxt2, stderr);
    fputs(text, stderr);
    fputs("\n\n", stderr);

    if(!isDedicated)
    {
        PL_ErrorDialog(text);
    }

    exit(1);
}

void Sys_Printf(const char* fmt, ...)
{
    va_list argptr;

    va_start(argptr, fmt);
    vprintf(fmt, argptr);
    va_end(argptr);
}

void Sys_Quit()
{
    Host_Shutdown();

    exit(0);
}

double Sys_DoubleTime()
{
    return SDL_GetPerformanceCounter() /
           (long double)SDL_GetPerformanceFrequency();
}

const char* Sys_ConsoleInput()
{
    static char con_text[256];
    static int textlen;
    char c;
    fd_set set;
    struct timeval timeout;

    FD_ZERO(&set);
    FD_SET(0, &set); // stdin
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    while(select(1, &set, nullptr, nullptr, &timeout))
    {
        read(0, &c, 1);
        if(c == '\n' || c == '\r')
        {
            con_text[textlen] = '\0';
            textlen = 0;
            return con_text;
        }
        else if(c == 8)
        {
            if(textlen)
            {
                textlen--;
                con_text[textlen] = '\0';
            }
            continue;
        }
        con_text[textlen] = c;
        textlen++;
        if(textlen < (int)sizeof(con_text))
            con_text[textlen] = '\0';
        else
        {
            // buffer is full
            textlen = 0;
            con_text[0] = '\0';
            Sys_Printf("\nConsole input too long!\n");
            break;
        }
    }

    return nullptr;
}

void Sys_Sleep(unsigned long msecs)
{
    /*	usleep (msecs * 1000);*/
    SDL_Delay(msecs);
}

void Sys_SendKeyEvents()
{
    IN_Commands(); // ericw -- allow joysticks to add keys so they can be used
                   // to confirm SCR_ModalMessage
    IN_SendKeyEvents();
}
