/* vim: set tabstop=4: */
/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2016      Spike

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

// provides a few convienience extensions, primarily builtins, but also
// autocvars. Also note the set+seta features.

#include "progs.hpp"
#include "progs_utils.hpp"
#include "qcvm.hpp"
#include "cvar.hpp"
#include "client.hpp"
#include "server.hpp"
#include "console.hpp"
#include "mathlib.hpp"
#include "quakeglm_qvec3.hpp"
#include "quakeglm_qvec4.hpp"
#include "quakeglm_qvec3_togl.hpp"
#include "quakeglm_qvec4_togl.hpp"
#include "util.hpp"
#include "developer.hpp"
#include "q_ctype.hpp"
#include "msg.hpp"
#include "sys.hpp"
#include "cmd.hpp"
#include "q_sound.hpp"
#include "server.hpp"
#include "crc.hpp"
#include "net.hpp"
#include "draw.hpp"
#include "glquake.hpp"
#include "gl_texmgr.hpp"
#include "gl_model.hpp"
#include "qpic.hpp"

#include <GL/glew.h>

#include <cstddef>
#include <cstdint>

#define countof(x) (sizeof(x) / sizeof((x)[0]))

struct qpic_t;

// there's a few different aproaches to tempstrings...
// the lame way is to just have a single one (vanilla).
// the only slightly less lame way is to just cycle between 16 or so (most
// engines). one funky way is to allocate a single large buffer and just
// concatenate it for more tempstring space. don't forget to resize (dp).
// alternatively, just allocate them persistently and purge them only when there
// appear to be no more references to it (fte). makes strzone redundant.

extern cvar_t sv_gameplayfix_setmodelrealbox;
cvar_t pr_checkextension = {"pr_checkextension", "1",
    CVAR_NONE}; // spike - enables qc extensions. if 0 then they're ALL BLOCKED!
                // MWAHAHAHA! *cough* *splutter*
int pr_ext_warned_particleeffectnum; // so these only spam once per map

static void* PR_FindExtGlobal(int type, const char* name);
void SV_CheckVelocity(edict_t* ent);

typedef enum multicast_e
{
    MULTICAST_ALL_U,
    MULTICAST_PHS_U,
    MULTICAST_PVS_U,
    MULTICAST_ALL_R,
    MULTICAST_PHS_R,
    MULTICAST_PVS_R,

    MULTICAST_ONE_U,
    MULTICAST_ONE_R,
    MULTICAST_INIT
} multicast_t;
static void SV_Multicast(
    multicast_t to, const qvec3& org, int msg_entity, unsigned int requireext2);

#define Z_StrDup(s) strcpy((char*)Z_Malloc(strlen(s) + 1), s)
#define RETURN_EDICT(e) (((int*)qcvm->globals)[OFS_RETURN] = EDICT_TO_PROG(e))

int PR_MakeTempString(const char* val)
{
    char* tmp = PR_GetTempString();
    q_strlcpy(tmp, val, STRINGTEMP_LENGTH);
    return PR_SetEngineString(tmp);
}

#define ishex(c) \
    ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
static int dehex(char c)
{
    if(c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if(c >= 'A' && c <= 'F')
    {
        return c - ('A' - 10);
    }
    if(c >= 'a' && c <= 'f')
    {
        return c - ('a' - 10);
    }
    return 0;
}
// returns the next char...
struct markup_s
{
    const unsigned char* txt;
    qvec4 tint;   // predefined colour that applies to the entire string
    qvec4 colour; // colour for the specific glyph in question
    unsigned char mask;
};
void PR_Markup_Begin(
    struct markup_s* mu, const char* text, const qvec3& rgb, float alpha)
{
    if(*text == '\1' || *text == '\2')
    {
        mu->mask = 128;
        text++;
    }
    else
    {
        mu->mask = 0;
    }
    mu->txt = (const unsigned char*)text;
    mu->tint.xyz = rgb;
    mu->tint[3] = alpha;
    mu->colour.xyz = rgb;
    mu->colour[3] = alpha;
}
int PR_Markup_Parse(struct markup_s* mu)
{
    static const qvec4 q3rgb[10] = {{0.00, 0.00, 0.00, 1.0},
        {1.00, 0.33, 0.33, 1.0}, {0.00, 1.00, 0.33, 1.0},
        {1.00, 1.00, 0.33, 1.0}, {0.33, 0.33, 1.00, 1.0},
        {0.33, 1.00, 1.00, 1.0}, {1.00, 0.33, 1.00, 1.0},
        {1.00, 1.00, 1.00, 1.0}, {1.00, 1.00, 1.00, 0.5},
        {0.50, 0.50, 0.50, 1.0}};
    unsigned int c;
    const qvec4* f;
    while((c = *mu->txt))
    {
        if(c == '^' && pr_checkextension.value)
        { // parse markup like FTE/DP might.
            switch(mu->txt[1])
            {
                case '^': // doubled up char for escaping.
                    mu->txt++;
                    break;
                case '0': // black
                case '1': // red
                case '2': // green
                case '3': // yellow
                case '4': // blue
                case '5': // cyan
                case '6': // magenta
                case '7': // white
                case '8': // white+half-alpha
                case '9': // grey
                    f = &q3rgb[mu->txt[1] - '0'];
                    mu->colour[0] = mu->tint[0] * (*f)[0];
                    mu->colour[1] = mu->tint[1] * (*f)[1];
                    mu->colour[2] = mu->tint[2] * (*f)[2];
                    mu->colour[3] = mu->tint[3] * (*f)[3];
                    mu->txt += 2;
                    continue;
                case 'h': // toggle half-alpha
                    if(mu->colour[3] != mu->tint[3] * 0.5)
                    {
                        mu->colour[3] = mu->tint[3] * 0.5;
                    }
                    else
                    {
                        mu->colour[3] = mu->tint[3];
                    }
                    mu->txt += 2;
                    continue;
                case 'd': // reset to defaults (fixme: should reset ^m without
                          // resetting \1)
                    mu->colour[0] = mu->tint[0];
                    mu->colour[1] = mu->tint[1];
                    mu->colour[2] = mu->tint[2];
                    mu->colour[3] = mu->tint[3];
                    mu->mask = 0;
                    mu->txt += 2;
                    break;
                case 'b': // blink
                case 's': // modstack push
                case 'r': // modstack restore
                    mu->txt += 2;
                    continue;
                case 'x': // RGB 12-bit colour
                    if(ishex(mu->txt[2]) && ishex(mu->txt[3]) &&
                        ishex(mu->txt[4]))
                    {
                        mu->colour[0] = mu->tint[0] * dehex(mu->txt[2]) / 15.0;
                        mu->colour[1] = mu->tint[1] * dehex(mu->txt[3]) / 15.0;
                        mu->colour[2] = mu->tint[2] * dehex(mu->txt[4]) / 15.0;
                        mu->txt += 5;
                        continue;
                    }
                    break; // malformed
                case '[':  // start fte's ^[text\key\value\key\value^] links
                case ']':  // end link
                    break; // fixme... skip the keys, recolour properly, etc
                    //				txt+=2;
                    //				continue;
                case '&':
                    if((ishex(mu->txt[2]) || mu->txt[2] == '-') &&
                        (ishex(mu->txt[3]) || mu->txt[3] == '-'))
                    { // ignore fte's fore/back ansi colours
                        mu->txt += 4;
                        continue;
                    }
                    break; // malformed
                case 'm':  // toggle masking.
                    mu->txt += 2;
                    mu->mask ^= 128;
                    continue;
                case 'U': // ucs-2 unicode codepoint
                    if(ishex(mu->txt[2]) && ishex(mu->txt[3]) &&
                        ishex(mu->txt[4]) && ishex(mu->txt[5]))
                    {
                        c = (dehex(mu->txt[2]) << 12) |
                            (dehex(mu->txt[3]) << 8) |
                            (dehex(mu->txt[4]) << 4) | dehex(mu->txt[5]);
                        mu->txt += 6;

                        if(c >= 0xe000 && c <= 0xe0ff)
                        {
                            c &= 0xff; // private-use 0xE0XX maps to quake's
                                       // chars
                        }
                        else if(c >= 0x20 && c <= 0x7f)
                        {
                            c &= 0x7f; // ascii is okay too.
                        }
                        else
                        {
                            c = '?'; // otherwise its some unicode char that we
                        }
                        // don't know how to handle.
                        return c;
                    }
                    break; // malformed
                case '{':  // full unicode codepoint, for chars up to 0x10ffff
                    mu->txt += 2;
                    c = 0; // no idea
                    while(*mu->txt)
                    {
                        if(*mu->txt == '}')
                        {
                            mu->txt++;
                            break;
                        }
                        if(!ishex(*mu->txt))
                        {
                            break;
                        }
                        c <<= 4;
                        c |= dehex(*mu->txt++);
                    }

                    if(c >= 0xe000 && c <= 0xe0ff)
                    {
                        c &= 0xff; // private-use 0xE0XX maps to quake's chars
                    }
                    else if(c >= 0x20 && c <= 0x7f)
                    {
                        c &= 0x7f; // ascii is okay too.
                        // it would be nice to include a table to de-accent
                        // latin scripts, as well as translate cyrilic somehow,
                        // but not really necessary.
                    }
                    else
                    {
                        c = '?'; // otherwise its some unicode char that we
                    }
                    // don't know how to handle.
                    return c;
            }
        }

        // regular char
        mu->txt++;
        return c | mu->mask;
    }
    return 0;
}

#define D(typestr, desc) typestr, desc

//#define fixme

// maths stuff
static void PF_Sin()
{
    G_FLOAT(OFS_RETURN) = sin(G_FLOAT(OFS_PARM0));
}
static void PF_asin()
{
    G_FLOAT(OFS_RETURN) = asin(G_FLOAT(OFS_PARM0));
}
static void PF_Cos()
{
    G_FLOAT(OFS_RETURN) = cos(G_FLOAT(OFS_PARM0));
}
static void PF_acos()
{
    G_FLOAT(OFS_RETURN) = acos(G_FLOAT(OFS_PARM0));
}
static void PF_tan()
{
    G_FLOAT(OFS_RETURN) = tan(G_FLOAT(OFS_PARM0));
}
static void PF_atan()
{
    G_FLOAT(OFS_RETURN) = atan(G_FLOAT(OFS_PARM0));
}
static void PF_atan2()
{
    G_FLOAT(OFS_RETURN) = atan2(G_FLOAT(OFS_PARM0), G_FLOAT(OFS_PARM1));
}
static void PF_Sqrt()
{
    G_FLOAT(OFS_RETURN) = sqrt(G_FLOAT(OFS_PARM0));
}
static void PF_pow()
{
    G_FLOAT(OFS_RETURN) = pow(G_FLOAT(OFS_PARM0), G_FLOAT(OFS_PARM1));
}
static void PF_Logarithm()
{
    // log2(v) = ln(v)/ln(2)
    double r;
    r = log(G_FLOAT(OFS_PARM0));
    if(qcvm->argc > 1)
    {
        r /= log(G_FLOAT(OFS_PARM1));
    }
    G_FLOAT(OFS_RETURN) = r;
}
static void PF_mod()
{
    float a = G_FLOAT(OFS_PARM0);
    float n = G_FLOAT(OFS_PARM1);

    if(n == 0)
    {
        Con_DWarning("PF_mod: mod by zero\n");
        G_FLOAT(OFS_RETURN) = 0;
    }
    else
    {
        // because QC is inherantly floaty, lets use floats.
        G_FLOAT(OFS_RETURN) = a - (n * (int)(a / n));
    }
}
static void PF_min()
{
    float r = G_FLOAT(OFS_PARM0);
    int i;
    for(i = 1; i < qcvm->argc; i++)
    {
        if(r > G_FLOAT(OFS_PARM0 + i * 3))
        {
            r = G_FLOAT(OFS_PARM0 + i * 3);
        }
    }
    G_FLOAT(OFS_RETURN) = r;
}
static void PF_max()
{
    float r = G_FLOAT(OFS_PARM0);
    int i;
    for(i = 1; i < qcvm->argc; i++)
    {
        if(r < G_FLOAT(OFS_PARM0 + i * 3))
        {
            r = G_FLOAT(OFS_PARM0 + i * 3);
        }
    }
    G_FLOAT(OFS_RETURN) = r;
}
static void PF_bound()
{
    float minval = G_FLOAT(OFS_PARM0);
    float curval = G_FLOAT(OFS_PARM1);
    float maxval = G_FLOAT(OFS_PARM2);
    if(curval > maxval)
    {
        curval = maxval;
    }
    if(curval < minval)
    {
        curval = minval;
    }
    G_FLOAT(OFS_RETURN) = curval;
}
static void PF_anglemod()
{
    float v = G_FLOAT(OFS_PARM0);

    while(v >= 360)
    {
        v = v - 360;
    }
    while(v < 0)
    {
        v = v + 360;
    }

    G_FLOAT(OFS_RETURN) = v;
}
static void PF_bitshift()
{
    int bitmask = G_FLOAT(OFS_PARM0);
    int shift = G_FLOAT(OFS_PARM1);
    if(shift < 0)
    {
        bitmask >>= -shift;
    }
    else
    {
        bitmask <<= shift;
    }
    G_FLOAT(OFS_RETURN) = bitmask;
}
static void PF_crossproduct()
{
    returnVector(
        CrossProduct(extractVector(OFS_PARM0), extractVector(OFS_PARM1)));
}
static void PF_vectorvectors()
{
    pr_global_struct->v_forward = glm::normalize(extractVector(OFS_PARM0));
    if(!pr_global_struct->v_forward[0] && !pr_global_struct->v_forward[1])
    {
        if(pr_global_struct->v_forward[2])
        {
            pr_global_struct->v_right[1] = -1;
        }
        else
        {
            pr_global_struct->v_right[1] = 0;
        }
        pr_global_struct->v_right[0] = pr_global_struct->v_right[2] = 0;
    }
    else
    {
        pr_global_struct->v_right[0] = pr_global_struct->v_forward[1];
        pr_global_struct->v_right[1] = -pr_global_struct->v_forward[0];
        pr_global_struct->v_right[2] = 0;
        pr_global_struct->v_right = glm::normalize(pr_global_struct->v_right);
    }

    pr_global_struct->v_up =
        CrossProduct(pr_global_struct->v_right, pr_global_struct->v_forward);
}
static void PF_ext_vectoangles()
{
    // alternative version of the original builtin, that can deal with roll
    // angles too, by accepting an optional second argument for 'up'.

    const qvec3 value1 = extractVector(OFS_PARM0);

    if(qcvm->argc >= 2)
    {
        const qvec3 up = extractVector(OFS_PARM1);

        qvec3 result = quake::util::pitchYawRollFromDirectionVector(up, value1);
        result[PITCH] *= -1;
        returnVector(result);
    }
    else
    {
        qvec3 result = quake::util::pitchYawRollFromDirectionVector(
            qvec3{0, 0, 1}, value1);
        result[PITCH] *= -1;
        returnVector(result);
    }

    // this builtin is for use with models. models have an inverted
    // pitch. consistency with makevectors would never do!
}

// string stuff
static void PF_strlen()
{ // FIXME: doesn't try to handle utf-8
    const char* s = G_STRING(OFS_PARM0);
    G_FLOAT(OFS_RETURN) = strlen(s);
}
static void PF_strcat()
{
    int i;
    char* out = PR_GetTempString();
    size_t s;

    out[0] = 0;
    s = 0;
    for(i = 0; i < qcvm->argc; i++)
    {
        s = q_strlcat(out, G_STRING((OFS_PARM0 + i * 3)), STRINGTEMP_LENGTH);
        if(s >= STRINGTEMP_LENGTH)
        {
            Con_Warning("PF_strcat: overflow (string truncated)\n");
            break;
        }
    }

    G_INT(OFS_RETURN) = PR_SetEngineString(out);
}
static void PF_substring()
{
    int start, length, slen;
    const char* s;
    char* string;

    s = G_STRING(OFS_PARM0);
    start = G_FLOAT(OFS_PARM1);
    length = G_FLOAT(OFS_PARM2);

    slen = strlen(s); // utf-8 should use chars, not bytes.

    if(start < 0)
    {
        start = slen + start;
    }
    if(length < 0)
    {
        length = slen - start + (length + 1);
    }
    if(start < 0)
    {
        //	length += start;
        start = 0;
    }

    if(start >= slen || length <= 0)
    {
        G_INT(OFS_RETURN) = PR_SetEngineString("");
        return;
    }

    slen -= start;
    if(length > slen)
    {
        length = slen;
    }
    // utf-8 should switch to bytes now.
    s += start;

    if(length >= STRINGTEMP_LENGTH)
    {
        length = STRINGTEMP_LENGTH - 1;
        Con_Warning("PF_substring: truncation\n");
    }

    string = PR_GetTempString();
    memcpy(string, s, length);
    string[length] = '\0';
    G_INT(OFS_RETURN) = PR_SetEngineString(string);
}
/*our zoned strings implementation is somewhat specific to quakespasm, so good
 * luck porting*/
static void PF_strzone()
{
    char* buf;
    size_t len = 0;
    const char* s[8];
    size_t l[8];
    int i;
    size_t id;

    for(i = 0; i < qcvm->argc; i++)
    {
        s[i] = G_STRING(OFS_PARM0 + i * 3);
        l[i] = strlen(s[i]);
        len += l[i];
    }
    len++; /*for the null*/

    buf = (char*)Z_Malloc(len);
    G_INT(OFS_RETURN) = PR_SetEngineString(buf);
    id = -1 - G_INT(OFS_RETURN);
    if(id >= qcvm->knownzonesize)
    {
        qcvm->knownzonesize = (id + 32) & ~7;
        qcvm->knownzone = (unsigned char*)Z_Realloc(
            qcvm->knownzone, qcvm->knownzonesize >> 3);
    }
    qcvm->knownzone[id >> 3] |= 1u << (id & 7);

    for(i = 0; i < qcvm->argc; i++)
    {
        memcpy(buf, s[i], l[i]);
        buf += l[i];
    }
    *buf = '\0';
}
static void PF_strunzone()
{
    size_t id;
    const char* foo = G_STRING(OFS_PARM0);

    if(!G_INT(OFS_PARM0))
    {
        return; // don't bug out if they gave a null string
    }
    id = -1 - G_INT(OFS_PARM0);
    if(id < qcvm->knownzonesize &&
        (qcvm->knownzone[id >> 3] & (1u << (id & 7))))
    {
        qcvm->knownzone[id >> 3] &= ~(1u << (id & 7));
        PR_ClearEngineString(G_INT(OFS_PARM0));
        Z_Free((void*)foo);
    }
    else
    {
        Con_Warning("PF_strunzone: string wasn't strzoned\n");
    }
}
static void PR_UnzoneAll()
{ // called to clean up all zoned strings.
    while(qcvm->knownzonesize-- > 0)
    {
        size_t id = qcvm->knownzonesize;
        if(qcvm->knownzone[id >> 3] & (1u << (id & 7)))
        {
            string_t s = -1 - (int)id;
            char* ptr = (char*)PR_GetString(s);
            PR_ClearEngineString(s);
            Z_Free(ptr);
        }
    }
    if(qcvm->knownzone)
    {
        Z_Free(qcvm->knownzone);
    }
    qcvm->knownzonesize = 0;
    qcvm->knownzone = nullptr;
}
static bool qc_isascii(unsigned int u)
{
    if(u < 256)
    { // should be just \n and 32-127, but we don't actually support any
        // actual unicode and we don't really want to make things worse.
        return true;
    }
    return false;
}
static void PF_str2chr()
{
    const char* instr = G_STRING(OFS_PARM0);
    int ofs = (qcvm->argc > 1) ? G_FLOAT(OFS_PARM1) : 0;

    if(ofs < 0)
    {
        ofs = strlen(instr) + ofs;
    }

    if(ofs && (ofs < 0 || ofs > (int)strlen(instr)))
    {
        G_FLOAT(OFS_RETURN) = '\0';
    }
    else
    {
        G_FLOAT(OFS_RETURN) = (unsigned char)instr[ofs];
    }
}
static void PF_chr2str()
{
    char *ret = PR_GetTempString(), *out;
    int i;
    for(i = 0, out = ret; out - ret < STRINGTEMP_LENGTH - 6 && i < qcvm->argc;
        i++)
    {
        unsigned int u = G_FLOAT(OFS_PARM0 + i * 3);
        if(u >= 0xe000 && u < 0xe100)
        {
            *out++ = (unsigned char)u; // quake chars.
        }
        else if(qc_isascii(u))
        {
            *out++ = u;
        }
        else
        {
            *out++ = '?'; // no unicode support
        }
    }
    *out = 0;
    G_INT(OFS_RETURN) = PR_SetEngineString(ret);
}
// part of PF_strconv
static int chrconv_number(int i, int base, int conv)
{
    i -= base;
    switch(conv)
    {
        default:
        case 5:
        case 6:
        case 0: break;
        case 1: base = '0'; break;
        case 2: base = '0' + 128; break;
        case 3: base = '0' - 30; break;
        case 4: base = '0' + 128 - 30; break;
    }
    return i + base;
}
// part of PF_strconv
static int chrconv_punct(int i, int base, int conv)
{
    i -= base;
    switch(conv)
    {
        default:
        case 0: break;
        case 1: base = 0; break;
        case 2: base = 128; break;
    }
    return i + base;
}
// part of PF_strconv
static int chrchar_alpha(
    int i, int basec, int baset, int convc, int convt, int charnum)
{
    // convert case and colour seperatly...

    i -= baset + basec;
    switch(convt)
    {
        default:
        case 0: break;
        case 1: baset = 0; break;
        case 2: baset = 128; break;

        case 5:
        case 6: baset = 128 * ((charnum & 1) == (convt - 5)); break;
    }

    switch(convc)
    {
        default:
        case 0: break;
        case 1: basec = 'a'; break;
        case 2: basec = 'A'; break;
    }
    return i + basec + baset;
}
// FTE_STRINGS
// bulk convert a string. change case or colouring.
static void PF_strconv()
{
    int ccase = G_FLOAT(OFS_PARM0);    // 0 same, 1 lower, 2 upper
    int redalpha = G_FLOAT(OFS_PARM1); // 0 same, 1 white, 2 red,  5 alternate,
                                       // 6 alternate-alternate
    int rednum =
        G_FLOAT(OFS_PARM2); // 0 same, 1 white, 2 red, 3 redspecial, 4
                            // whitespecial, 5 alternate, 6 alternate-alternate
    const unsigned char* string = (const unsigned char*)PF_VarString(3);
    int len = strlen((const char*)string);
    int i;
    unsigned char* resbuf = (unsigned char*)PR_GetTempString();
    unsigned char* result = resbuf;

    // UTF-8-FIXME: cope with utf+^U etc

    if(len >= STRINGTEMP_LENGTH)
    {
        len = STRINGTEMP_LENGTH - 1;
    }

    for(i = 0; i < len;
        i++, string++, result++) // should this be done backwards?
    {
        if(*string >= '0' && *string <= '9')
        { // normal numbers...
            *result = chrconv_number(*string, '0', rednum);
        }
        else if(*string >= '0' + 128 && *string <= '9' + 128)
        {
            *result = chrconv_number(*string, '0' + 128, rednum);
        }
        else if(*string >= '0' + 128 - 30 && *string <= '9' + 128 - 30)
        {
            *result = chrconv_number(*string, '0' + 128 - 30, rednum);
        }
        else if(*string >= '0' - 30 && *string <= '9' - 30)
        {
            *result = chrconv_number(*string, '0' - 30, rednum);
        }
        else if(*string >= 'a' && *string <= 'z')
        { // normal numbers...
            *result = chrchar_alpha(*string, 'a', 0, ccase, redalpha, i);
        }
        else if(*string >= 'A' && *string <= 'Z')
        { // normal numbers...
            *result = chrchar_alpha(*string, 'A', 0, ccase, redalpha, i);
        }
        else if(*string >= 'a' + 128 && *string <= 'z' + 128)
        { // normal numbers...
            *result = chrchar_alpha(*string, 'a', 128, ccase, redalpha, i);
        }
        else if(*string >= 'A' + 128 && *string <= 'Z' + 128)
        { // normal numbers...
            *result = chrchar_alpha(*string, 'A', 128, ccase, redalpha, i);
        }
        else if((*string & 127) < 16 || !redalpha)
        { // special chars..
            *result = *string;
        }
        else if(*string < 128)
        {
            *result = chrconv_punct(*string, 0, redalpha);
        }
        else
        {
            *result = chrconv_punct(*string, 128, redalpha);
        }
    }
    *result = '\0';

    G_INT(OFS_RETURN) = PR_SetEngineString((char*)resbuf);
}
static void PF_strpad()
{
    char* destbuf = PR_GetTempString();
    char* dest = destbuf;
    int pad = G_FLOAT(OFS_PARM0);
    const char* src = PF_VarString(1);

    // UTF-8-FIXME: pad is chars not bytes...

    if(pad < 0)
    { // pad left
        pad = -pad - strlen(src);
        if(pad >= STRINGTEMP_LENGTH)
        {
            pad = STRINGTEMP_LENGTH - 1;
        }
        if(pad < 0)
        {
            pad = 0;
        }

        q_strlcpy(dest + pad, src, STRINGTEMP_LENGTH - pad);
        while(pad)
        {
            dest[--pad] = ' ';
        }
    }
    else
    { // pad right
        if(pad >= STRINGTEMP_LENGTH)
        {
            pad = STRINGTEMP_LENGTH - 1;
        }
        pad -= strlen(src);
        if(pad < 0)
        {
            pad = 0;
        }

        q_strlcpy(dest, src, STRINGTEMP_LENGTH);
        dest += strlen(dest);

        while(pad-- > 0)
        {
            *dest++ = ' ';
        }
        *dest = '\0';
    }

    G_INT(OFS_RETURN) = PR_SetEngineString(destbuf);
}
static void PF_infoadd()
{
    const char* info = G_STRING(OFS_PARM0);
    const char* key = G_STRING(OFS_PARM1);
    const char* value = PF_VarString(2);
    char *destbuf = PR_GetTempString(), *o = destbuf,
         *e = destbuf + STRINGTEMP_LENGTH - 1;

    size_t keylen = strlen(key);
    size_t valuelen = strlen(value);
    if(!*key)
    { // error
        G_INT(OFS_RETURN) = G_INT(OFS_PARM0);
        return;
    }

    // copy the string to the output, stripping the named key
    while(*info)
    {
        const char* l = info;
        if(*info++ != '\\')
        {
            break; // error / end-of-string
        }

        if(!strncmp(info, key, keylen) && info[keylen] == '\\')
        {
            // skip the key name
            info += keylen + 1;
            // this is the old value for the key. skip over it
            while(*info && *info != '\\')
            {
                info++;
            }
        }
        else
        {
            // skip the key
            while(*info && *info != '\\')
            {
                info++;
            }

            // validate that its a value now
            if(*info++ != '\\')
            {
                break; // error
            }
            // skip the value
            while(*info && *info != '\\')
            {
                info++;
            }

            // copy them over
            if(o + (info - l) >= e)
            {
                break; // exceeds maximum length
            }
            while(l < info)
            {
                *o++ = *l++;
            }
        }
    }

    if(*info)
    {
        Con_Warning("PF_infoadd: invalid source info\n");
    }
    else if(!*value)
    {
        ; // nothing needed
    }
    else if(!*key || strchr(key, '\\') || strchr(value, '\\'))
    {
        Con_Warning("PF_infoadd: invalid key/value\n");
    }
    else if(o + 2 + keylen + valuelen >= e)
    {
        Con_Warning("PF_infoadd: length exceeds max\n");
    }
    else
    {
        *o++ = '\\';
        memcpy(o, key, keylen);
        o += keylen;
        *o++ = '\\';
        memcpy(o, value, valuelen);
        o += keylen;
    }

    *o = 0;
    G_INT(OFS_RETURN) = PR_SetEngineString(destbuf);
}
static void PF_infoget()
{
    const char* info = G_STRING(OFS_PARM0);
    const char* key = G_STRING(OFS_PARM1);
    size_t keylen = strlen(key);
    while(*info)
    {
        if(*info++ != '\\')
        {
            break; // error / end-of-string
        }

        if(!strncmp(info, key, keylen) && info[keylen] == '\\')
        {
            char *destbuf = PR_GetTempString(), *o = destbuf,
                 *e = destbuf + STRINGTEMP_LENGTH - 1;

            // skip the key name
            info += keylen + 1;
            // this is the old value for the key. copy it to the result
            while(*info && *info != '\\' && o < e)
            {
                *o++ = *info++;
            }
            *o++ = 0;

            // success!
            G_INT(OFS_RETURN) = PR_SetEngineString(destbuf);
            return;
        }
        else
        {
            // skip the key
            while(*info && *info != '\\')
            {
                info++;
            }

            // validate that its a value now
            if(*info++ != '\\')
            {
                break; // error
            }
            // skip the value
            while(*info && *info != '\\')
            {
                info++;
            }
        }
    }
    G_INT(OFS_RETURN) = 0;
}
static void PF_strncmp()
{
    const char* a = G_STRING(OFS_PARM0);
    const char* b = G_STRING(OFS_PARM1);

    if(qcvm->argc > 2)
    {
        int len = G_FLOAT(OFS_PARM2);
        int aofs = qcvm->argc > 3 ? G_FLOAT(OFS_PARM3) : 0;
        int bofs = qcvm->argc > 4 ? G_FLOAT(OFS_PARM4) : 0;
        if(aofs < 0 || (aofs && aofs > (int)strlen(a)))
        {
            aofs = strlen(a);
        }
        if(bofs < 0 || (bofs && bofs > (int)strlen(b)))
        {
            bofs = strlen(b);
        }
        G_FLOAT(OFS_RETURN) = Q_strncmp(a + aofs, b, len);
    }
    else
    {
        G_FLOAT(OFS_RETURN) = Q_strcmp(a, b);
    }
}
static void PF_strncasecmp()
{
    const char* a = G_STRING(OFS_PARM0);
    const char* b = G_STRING(OFS_PARM1);

    if(qcvm->argc > 2)
    {
        int len = G_FLOAT(OFS_PARM2);
        int aofs = qcvm->argc > 3 ? G_FLOAT(OFS_PARM3) : 0;
        int bofs = qcvm->argc > 4 ? G_FLOAT(OFS_PARM4) : 0;
        if(aofs < 0 || (aofs && aofs > (int)strlen(a)))
        {
            aofs = strlen(a);
        }
        if(bofs < 0 || (bofs && bofs > (int)strlen(b)))
        {
            bofs = strlen(b);
        }
        G_FLOAT(OFS_RETURN) = q_strncasecmp(a + aofs, b, len);
    }
    else
    {
        G_FLOAT(OFS_RETURN) = q_strcasecmp(a, b);
    }
}
static void PF_strstrofs()
{
    const char* instr = G_STRING(OFS_PARM0);
    const char* match = G_STRING(OFS_PARM1);
    int firstofs = (qcvm->argc > 2) ? G_FLOAT(OFS_PARM2) : 0;

    if(firstofs && (firstofs < 0 || firstofs > (int)strlen(instr)))
    {
        G_FLOAT(OFS_RETURN) = -1;
        return;
    }

    match = strstr(instr + firstofs, match);
    if(!match)
    {
        G_FLOAT(OFS_RETURN) = -1;
    }
    else
    {
        G_FLOAT(OFS_RETURN) = match - instr;
    }
}
static void PF_strtrim()
{
    const char* str = G_STRING(OFS_PARM0);
    const char* end;
    char* news;
    size_t len;

    // figure out the new start
    while(*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r')
    {
        str++;
    }

    // figure out the new end.
    end = str + strlen(str);
    while(end > str && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                           end[-1] == '\r'))
    {
        end--;
    }

    // copy that substring into a tempstring.
    len = end - str;
    if(len >= STRINGTEMP_LENGTH)
    {
        len = STRINGTEMP_LENGTH - 1;
    }

    news = PR_GetTempString();
    memcpy(news, str, len);
    news[len] = 0;

    G_INT(OFS_RETURN) = PR_SetEngineString(news);
}
static void PF_strreplace()
{
    char* resultbuf = PR_GetTempString();
    char* result = resultbuf;
    const char* search = G_STRING(OFS_PARM0);
    const char* replace = G_STRING(OFS_PARM1);
    const char* subject = G_STRING(OFS_PARM2);
    int searchlen = strlen(search);
    int replacelen = strlen(replace);

    if(searchlen)
    {
        while(
            *subject && result < resultbuf + sizeof(resultbuf) - replacelen - 2)
        {
            if(!strncmp(subject, search, searchlen))
            {
                subject += searchlen;
                memcpy(result, replace, replacelen);
                result += replacelen;
            }
            else
            {
                *result++ = *subject++;
            }
        }
        *result = 0;
        G_INT(OFS_RETURN) = PR_SetEngineString(resultbuf);
    }
    else
    {
        G_INT(OFS_RETURN) = PR_SetEngineString(subject);
    }
}
static void PF_strireplace()
{
    char* resultbuf = PR_GetTempString();
    char* result = resultbuf;
    const char* search = G_STRING(OFS_PARM0);
    const char* replace = G_STRING(OFS_PARM1);
    const char* subject = G_STRING(OFS_PARM2);
    int searchlen = strlen(search);
    int replacelen = strlen(replace);

    if(searchlen)
    {
        while(
            *subject && result < resultbuf + sizeof(resultbuf) - replacelen - 2)
        {
            // UTF-8-FIXME: case insensitivity is awkward...
            if(!q_strncasecmp(subject, search, searchlen))
            {
                subject += searchlen;
                memcpy(result, replace, replacelen);
                result += replacelen;
            }
            else
            {
                *result++ = *subject++;
            }
        }
        *result = 0;
        G_INT(OFS_RETURN) = PR_SetEngineString(resultbuf);
    }
    else
    {
        G_INT(OFS_RETURN) = PR_SetEngineString(subject);
    }
}


static void PF_sprintf_internal(
    const char* s, int firstarg, char* outbuf, int outbuflen)
{
    const char* s0;
    char *o = outbuf, *end = outbuf + outbuflen, *err;
    int width, precision, thisarg, flags;
    char formatbuf[16];
    char* f;
    int argpos = firstarg;
    int isfloat;
    static int dummyivec[3] = {0, 0, 0};
    static float dummyvec[3] = {0, 0, 0};

#define PRINTF_ALTERNATE 1
#define PRINTF_ZEROPAD 2
#define PRINTF_LEFT 4
#define PRINTF_SPACEPOSITIVE 8
#define PRINTF_SIGNPOSITIVE 16

    formatbuf[0] = '%';

#define GETARG_FLOAT(a) \
    (((a) >= firstarg && (a) < qcvm->argc) ? (G_FLOAT(OFS_PARM0 + 3 * (a))) : 0)
#define GETARG_VECTOR(a)                                                     \
    (((a) >= firstarg && (a) < qcvm->argc) ? (G_VECTOR(OFS_PARM0 + 3 * (a))) \
                                           : dummyvec)
#define GETARG_INT(a) \
    (((a) >= firstarg && (a) < qcvm->argc) ? (G_INT(OFS_PARM0 + 3 * (a))) : 0)
#define GETARG_INTVECTOR(a)                         \
    (((a) >= firstarg && (a) < qcvm->argc)          \
            ? ((int*)G_VECTOR(OFS_PARM0 + 3 * (a))) \
            : dummyivec)
#define GETARG_STRING(a)                                                     \
    (((a) >= firstarg && (a) < qcvm->argc) ? (G_STRING(OFS_PARM0 + 3 * (a))) \
                                           : "")

    for(;;)
    {
        s0 = s;
        switch(*s)
        {
            case 0: goto finished;
            case '%':
                ++s;

                if(*s == '%')
                {
                    goto verbatim;
                }

                // complete directive format:
                // %3$*1$.*2$ld

                width = -1;
                precision = -1;
                thisarg = -1;
                flags = 0;
                isfloat = -1;

                // is number following?
                if(*s >= '0' && *s <= '9')
                {
                    width = strtol(s, &err, 10);
                    if(!err)
                    {
                        Con_Warning("PF_sprintf: bad format string: %s\n", s0);
                        goto finished;
                    }
                    if(*err == '$')
                    {
                        thisarg = width + (firstarg - 1);
                        width = -1;
                        s = err + 1;
                    }
                    else
                    {
                        if(*s == '0')
                        {
                            flags |= PRINTF_ZEROPAD;
                            if(width == 0)
                            {
                                width = -1; // it was just a flag
                            }
                        }
                        s = err;
                    }
                }

                if(width < 0)
                {
                    for(;;)
                    {
                        switch(*s)
                        {
                            case '#': flags |= PRINTF_ALTERNATE; break;
                            case '0': flags |= PRINTF_ZEROPAD; break;
                            case '-': flags |= PRINTF_LEFT; break;
                            case ' ': flags |= PRINTF_SPACEPOSITIVE; break;
                            case '+': flags |= PRINTF_SIGNPOSITIVE; break;
                            default: goto noflags;
                        }
                        ++s;
                    }
                noflags:
                    if(*s == '*')
                    {
                        ++s;
                        if(*s >= '0' && *s <= '9')
                        {
                            width = strtol(s, &err, 10);
                            if(!err || *err != '$')
                            {
                                Con_Warning(
                                    "PF_sprintf: invalid format string: %s\n",
                                    s0);
                                goto finished;
                            }
                            s = err + 1;
                        }
                        else
                        {
                            width = argpos++;
                        }
                        width = GETARG_FLOAT(width);
                        if(width < 0)
                        {
                            flags |= PRINTF_LEFT;
                            width = -width;
                        }
                    }
                    else if(*s >= '0' && *s <= '9')
                    {
                        width = strtol(s, &err, 10);
                        if(!err)
                        {
                            Con_Warning(
                                "PF_sprintf: invalid format string: %s\n", s0);
                            goto finished;
                        }
                        s = err;
                        if(width < 0)
                        {
                            flags |= PRINTF_LEFT;
                            width = -width;
                        }
                    }
                    // otherwise width stays -1
                }

                if(*s == '.')
                {
                    ++s;
                    if(*s == '*')
                    {
                        ++s;
                        if(*s >= '0' && *s <= '9')
                        {
                            precision = strtol(s, &err, 10);
                            if(!err || *err != '$')
                            {
                                Con_Warning(
                                    "PF_sprintf: invalid format string: %s\n",
                                    s0);
                                goto finished;
                            }
                            s = err + 1;
                        }
                        else
                        {
                            precision = argpos++;
                        }
                        precision = GETARG_FLOAT(precision);
                    }
                    else if(*s >= '0' && *s <= '9')
                    {
                        precision = strtol(s, &err, 10);
                        if(!err)
                        {
                            Con_Warning(
                                "PF_sprintf: invalid format string: %s\n", s0);
                            goto finished;
                        }
                        s = err;
                    }
                    else
                    {
                        Con_Warning(
                            "PF_sprintf: invalid format string: %s\n", s0);
                        goto finished;
                    }
                }

                for(;;)
                {
                    switch(*s)
                    {
                        case 'h': isfloat = 1; break;
                        case 'l': isfloat = 0; break;
                        case 'L': isfloat = 0; break;
                        case 'j': break;
                        case 'z': break;
                        case 't': break;
                        default: goto nolength;
                    }
                    ++s;
                }
            nolength:

                // now s points to the final directive char and is no longer
                // changed
                if(*s == 'p' || *s == 'P')
                {
                    //%p is slightly different from %x.
                    // always 8-bytes wide with 0 padding, always ints.
                    flags |= PRINTF_ZEROPAD;
                    if(width < 0)
                    {
                        width = 8;
                    }
                    if(isfloat < 0)
                    {
                        isfloat = 0;
                    }
                }
                else if(*s == 'i')
                {
                    //%i defaults to ints, not floats.
                    if(isfloat < 0)
                    {
                        isfloat = 0;
                    }
                }

                // assume floats, not ints.
                if(isfloat < 0)
                {
                    isfloat = 1;
                }

                if(thisarg < 0)
                {
                    thisarg = argpos++;
                }

                if(o < end - 1)
                {
                    f = &formatbuf[1];
                    if(*s != 's' && *s != 'c')
                    {
                        if(flags & PRINTF_ALTERNATE)
                        {
                            *f++ = '#';
                        }
                    }
                    if(flags & PRINTF_ZEROPAD)
                    {
                        *f++ = '0';
                    }
                    if(flags & PRINTF_LEFT)
                    {
                        *f++ = '-';
                    }
                    if(flags & PRINTF_SPACEPOSITIVE)
                    {
                        *f++ = ' ';
                    }
                    if(flags & PRINTF_SIGNPOSITIVE)
                    {
                        *f++ = '+';
                    }
                    *f++ = '*';
                    if(precision >= 0)
                    {
                        *f++ = '.';
                        *f++ = '*';
                    }
                    if(*s == 'p')
                    {
                        *f++ = 'x';
                    }
                    else if(*s == 'P')
                    {
                        *f++ = 'X';
                    }
                    else
                    {
                        *f++ = *s;
                    }
                    *f++ = 0;

                    if(width < 0)
                    { // not set
                        width = 0;
                    }

                    switch(*s)
                    {
                        case 'd':
                        case 'i':
                            if(precision < 0)
                            { // not set
                                q_snprintf(o, end - o, formatbuf, width,
                                    (isfloat ? (int)GETARG_FLOAT(thisarg)
                                             : (int)GETARG_INT(thisarg)));
                            }
                            else
                            {
                                q_snprintf(o, end - o, formatbuf, width,
                                    precision,
                                    (isfloat ? (int)GETARG_FLOAT(thisarg)
                                             : (int)GETARG_INT(thisarg)));
                            }
                            o += strlen(o);
                            break;
                        case 'o':
                        case 'u':
                        case 'x':
                        case 'X':
                        case 'p':
                        case 'P':
                            if(precision < 0)
                            { // not set
                                q_snprintf(o, end - o, formatbuf, width,
                                    (isfloat ? (unsigned int)GETARG_FLOAT(
                                                   thisarg)
                                             : (unsigned int)GETARG_INT(
                                                   thisarg)));
                            }
                            else
                            {
                                q_snprintf(o, end - o, formatbuf, width,
                                    precision,
                                    (isfloat ? (unsigned int)GETARG_FLOAT(
                                                   thisarg)
                                             : (unsigned int)GETARG_INT(
                                                   thisarg)));
                            }
                            o += strlen(o);
                            break;
                        case 'e':
                        case 'E':
                        case 'f':
                        case 'F':
                        case 'g':
                        case 'G':
                            if(precision < 0)
                            { // not set
                                q_snprintf(o, end - o, formatbuf, width,
                                    (isfloat ? (double)GETARG_FLOAT(thisarg)
                                             : (double)GETARG_INT(thisarg)));
                            }
                            else
                            {
                                q_snprintf(o, end - o, formatbuf, width,
                                    precision,
                                    (isfloat ? (double)GETARG_FLOAT(thisarg)
                                             : (double)GETARG_INT(thisarg)));
                            }
                            o += strlen(o);
                            break;
                        case 'v':
                        case 'V':
                            f[-2] += 'g' - 'v';
                            if(precision < 0)
                            { // not set
                                q_snprintf(o, end - o,
                                    va("%s %s %s",
                                        /* NESTED SPRINTF IS NESTED */
                                        formatbuf, formatbuf, formatbuf),
                                    width,
                                    (isfloat ? (double)GETARG_VECTOR(thisarg)[0]
                                             : (double)GETARG_INTVECTOR(
                                                   thisarg)[0]),
                                    width,
                                    (isfloat ? (double)GETARG_VECTOR(thisarg)[1]
                                             : (double)GETARG_INTVECTOR(
                                                   thisarg)[1]),
                                    width,
                                    (isfloat ? (double)GETARG_VECTOR(thisarg)[2]
                                             : (double)GETARG_INTVECTOR(
                                                   thisarg)[2]));
                            }
                            else
                            {
                                q_snprintf(o, end - o,
                                    va("%s %s %s",
                                        /* NESTED SPRINTF IS NESTED */
                                        formatbuf, formatbuf, formatbuf),
                                    width, precision,
                                    (isfloat ? (double)GETARG_VECTOR(thisarg)[0]
                                             : (double)GETARG_INTVECTOR(
                                                   thisarg)[0]),
                                    width, precision,
                                    (isfloat ? (double)GETARG_VECTOR(thisarg)[1]
                                             : (double)GETARG_INTVECTOR(
                                                   thisarg)[1]),
                                    width, precision,
                                    (isfloat ? (double)GETARG_VECTOR(thisarg)[2]
                                             : (double)GETARG_INTVECTOR(
                                                   thisarg)[2]));
                            }
                            o += strlen(o);
                            break;
                        case 'c':
                            // UTF-8-FIXME: figure it out yourself
                            //							if(flags &
                            // PRINTF_ALTERNATE)
                            {
                                if(precision < 0)
                                { // not set
                                    q_snprintf(o, end - o, formatbuf, width,
                                        (isfloat ? (unsigned int)GETARG_FLOAT(
                                                       thisarg)
                                                 : (unsigned int)GETARG_INT(
                                                       thisarg)));
                                }
                                else
                                {
                                    q_snprintf(o, end - o, formatbuf, width,
                                        precision,
                                        (isfloat ? (unsigned int)GETARG_FLOAT(
                                                       thisarg)
                                                 : (unsigned int)GETARG_INT(
                                                       thisarg)));
                                }
                                o += strlen(o);
                            }
                            /*							else
                                                        {
                                                            unsigned int c =
                               (isfloat ? (unsigned int) GETARG_FLOAT(thisarg) :
                               (unsigned int) GETARG_INT(thisarg)); char
                               charbuf16[16]; const char *buf = u8_encodech(c,
                               nullptr, charbuf16); if(!buf) buf = "";
                               if(precision < 0) // not set precision = end - o
                               - 1; o += u8_strpad(o, end - o, buf, (flags &
                               PRINTF_LEFT)
                               != 0, width, precision);
                                                        }
                            */
                            break;
                        case 's':
                            // UTF-8-FIXME: figure it out yourself
                            //							if(flags &
                            // PRINTF_ALTERNATE)
                            {
                                if(precision < 0)
                                { // not set
                                    q_snprintf(o, end - o, formatbuf, width,
                                        GETARG_STRING(thisarg));
                                }
                                else
                                {
                                    q_snprintf(o, end - o, formatbuf, width,
                                        precision, GETARG_STRING(thisarg));
                                }
                                o += strlen(o);
                            }
                            /*							else
                                                        {
                                                            if(precision < 0) //
                               not set precision = end - o - 1; o +=
                               u8_strpad(o, end - o, GETARG_STRING(thisarg),
                               (flags & PRINTF_LEFT) != 0, width, precision);
                                                        }
                            */
                            break;
                        default:
                            Con_Warning(
                                "PF_sprintf: invalid format string: %s\n", s0);
                            goto finished;
                    }
                }
                ++s;
                break;
            default:
            verbatim:
                if(o < end - 1)
                {
                    *o++ = *s;
                }
                s++;
                break;
        }
    }
finished:
    *o = 0;
}

static void PF_sprintf()
{
    char* outbuf = PR_GetTempString();
    PF_sprintf_internal(G_STRING(OFS_PARM0), 1, outbuf, STRINGTEMP_LENGTH);
    G_INT(OFS_RETURN) = PR_SetEngineString(outbuf);
}

// string tokenizing (gah)
#define MAXQCTOKENS 64
static struct
{
    char* token;
    unsigned int start;
    unsigned int end;
} qctoken[MAXQCTOKENS];
unsigned int qctoken_count;

static void tokenize_flush()
{
    while(qctoken_count > 0)
    {
        qctoken_count--;
        free(qctoken[qctoken_count].token);
    }
    qctoken_count = 0;
}

static void PF_ArgC()
{
    G_FLOAT(OFS_RETURN) = qctoken_count;
}

static int tokenizeqc(const char* str, bool dpfuckage)
{
    // FIXME: if dpfuckage, then we should handle punctuation specially, as well
    // as /*.
    const char* start = str;
    while(qctoken_count > 0)
    {
        qctoken_count--;
        free(qctoken[qctoken_count].token);
    }
    qctoken_count = 0;
    while(qctoken_count < MAXQCTOKENS)
    {
        /*skip whitespace here so the token's start is accurate*/
        while(*str && *(unsigned char*)str <= ' ')
        {
            str++;
        }

        if(!*str)
        {
            break;
        }

        qctoken[qctoken_count].start = str - start;
        //		if (dpfuckage)
        //			str = COM_ParseDPFuckage(str);
        //		else
        str = COM_Parse(str);
        if(!str)
        {
            break;
        }

        qctoken[qctoken_count].token = strdup(com_token);

        qctoken[qctoken_count].end = str - start;
        qctoken_count++;
    }
    return qctoken_count;
}

/*KRIMZON_SV_PARSECLIENTCOMMAND added these two - note that for compatibility
 * with DP, this tokenize builtin is veeery vauge and doesn't match the
 * console*/
static void PF_Tokenize()
{
    G_FLOAT(OFS_RETURN) = tokenizeqc(G_STRING(OFS_PARM0), true);
}

static void PF_tokenize_console()
{
    G_FLOAT(OFS_RETURN) = tokenizeqc(G_STRING(OFS_PARM0), false);
}

static void PF_tokenizebyseparator()
{
    const char* str = G_STRING(OFS_PARM0);
    const char* sep[7];
    int seplen[7];
    int seps = 0, s;
    const char* start = str;
    int tlen;
    bool found = true;

    while(seps < qcvm->argc - 1 && seps < 7)
    {
        sep[seps] = G_STRING(OFS_PARM1 + seps * 3);
        seplen[seps] = strlen(sep[seps]);
        seps++;
    }

    tokenize_flush();

    qctoken[qctoken_count].start = 0;
    if(*str)
    {
        for(;;)
        {
            found = false;
            /*see if its a separator*/
            if(!*str)
            {
                qctoken[qctoken_count].end = str - start;
                found = true;
            }
            else
            {
                for(s = 0; s < seps; s++)
                {
                    if(!strncmp(str, sep[s], seplen[s]))
                    {
                        qctoken[qctoken_count].end = str - start;
                        str += seplen[s];
                        found = true;
                        break;
                    }
                }
            }
            /*it was, split it out*/
            if(found)
            {
                tlen =
                    qctoken[qctoken_count].end - qctoken[qctoken_count].start;
                qctoken[qctoken_count].token = (char*)malloc(tlen + 1);
                memcpy(qctoken[qctoken_count].token,
                    start + qctoken[qctoken_count].start, tlen);
                qctoken[qctoken_count].token[tlen] = 0;

                qctoken_count++;

                if(*str && qctoken_count < MAXQCTOKENS)
                {
                    qctoken[qctoken_count].start = str - start;
                }
                else
                {
                    break;
                }
            }
            str++;
        }
    }
    G_FLOAT(OFS_RETURN) = qctoken_count;
}

static void PF_argv_start_index()
{
    int idx = G_FLOAT(OFS_PARM0);

    /*negative indexes are relative to the end*/
    if(idx < 0)
    {
        idx += qctoken_count;
    }

    if((unsigned int)idx >= qctoken_count)
    {
        G_FLOAT(OFS_RETURN) = -1;
    }
    else
    {
        G_FLOAT(OFS_RETURN) = qctoken[idx].start;
    }
}

static void PF_argv_end_index()
{
    int idx = G_FLOAT(OFS_PARM0);

    /*negative indexes are relative to the end*/
    if(idx < 0)
    {
        idx += qctoken_count;
    }

    if((unsigned int)idx >= qctoken_count)
    {
        G_FLOAT(OFS_RETURN) = -1;
    }
    else
    {
        G_FLOAT(OFS_RETURN) = qctoken[idx].end;
    }
}

static void PF_ArgV()
{
    int idx = G_FLOAT(OFS_PARM0);

    /*negative indexes are relative to the end*/
    if(idx < 0)
    {
        idx += qctoken_count;
    }

    if((unsigned int)idx >= qctoken_count)
    {
        G_INT(OFS_RETURN) = 0;
    }
    else
    {
        char* ret = PR_GetTempString();
        q_strlcpy(ret, qctoken[idx].token, STRINGTEMP_LENGTH);
        G_INT(OFS_RETURN) = PR_SetEngineString(ret);
    }
}

// conversions (mostly string)
static void PF_strtoupper()
{
    const char* in = G_STRING(OFS_PARM0);
    char *out, *result = PR_GetTempString();
    for(out = result; *in && out < result + STRINGTEMP_LENGTH - 1;)
    {
        *out++ = q_toupper(*in++);
    }
    *out = 0;
    G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_strtolower()
{
    const char* in = G_STRING(OFS_PARM0);
    char *out, *result = PR_GetTempString();
    for(out = result; *in && out < result + STRINGTEMP_LENGTH - 1;)
    {
        *out++ = q_tolower(*in++);
    }
    *out = 0;
    G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
#include <time.h>
static void PF_strftime()
{
    const char* in = G_STRING(OFS_PARM1);
    char* result = PR_GetTempString();

    time_t ctime;
    struct tm* tm;

    ctime = time(nullptr);

    if(G_FLOAT(OFS_PARM0))
    {
        tm = localtime(&ctime);
    }
    else
    {
        tm = gmtime(&ctime);
    }

#ifdef _WIN32
    // msvc sucks. this is a crappy workaround.
    if(!strcmp(in, "%R"))
    {
        in = "%H:%M";
    }
    else if(!strcmp(in, "%F"))
    {
        in = "%Y-%m-%d";
    }
#endif

    strftime(result, STRINGTEMP_LENGTH, in, tm);

    G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_stof()
{
    G_FLOAT(OFS_RETURN) = atof(G_STRING(OFS_PARM0));
}
static void PF_stov()
{
    const char* s = G_STRING(OFS_PARM0);
    s = COM_Parse(s);
    G_VECTOR(OFS_RETURN)[0] = atof(com_token);
    s = COM_Parse(s);
    G_VECTOR(OFS_RETURN)[1] = atof(com_token);
    s = COM_Parse(s);
    G_VECTOR(OFS_RETURN)[2] = atof(com_token);
}
static void PF_stoi()
{
    G_INT(OFS_RETURN) = atoi(G_STRING(OFS_PARM0));
}
static void PF_itos()
{
    char* result = PR_GetTempString();
    q_snprintf(result, STRINGTEMP_LENGTH, "%i", G_INT(OFS_PARM0));
    G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_etos()
{ // yes, this is lame
    char* result = PR_GetTempString();
    q_snprintf(result, STRINGTEMP_LENGTH, "entity %i", G_EDICTNUM(OFS_PARM0));
    G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_stoh()
{
    G_INT(OFS_RETURN) = strtoul(G_STRING(OFS_PARM0), nullptr, 16);
}
static void PF_htos()
{
    char* result = PR_GetTempString();
    q_snprintf(result, STRINGTEMP_LENGTH, "%x", G_INT(OFS_PARM0));
    G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_ftoi()
{
    G_INT(OFS_RETURN) = G_FLOAT(OFS_PARM0);
}
static void PF_itof()
{
    G_FLOAT(OFS_RETURN) = G_INT(OFS_PARM0);
}

// collision stuff
static void PF_tracebox()
{
    // alternative version of traceline that just passes on two extra args.
    // trivial really.

    qvec3 v1 = extractVector(OFS_PARM0);
    const qvec3 mins = extractVector(OFS_PARM1);
    const qvec3 maxs = extractVector(OFS_PARM2);
    qvec3 v2 = extractVector(OFS_PARM3);
    int nomonsters = G_FLOAT(OFS_PARM4);
    edict_t* ent = G_EDICT(OFS_PARM5);

    /* FIXME FIXME FIXME: Why do we hit this with certain progs.dat ?? */
    if(quake::vr::developerMode())
    {
        if(IS_NAN(v1[0]) || IS_NAN(v1[1]) || IS_NAN(v1[2]) || IS_NAN(v2[0]) ||
            IS_NAN(v2[1]) || IS_NAN(v2[2]))
        {
            Con_Warning(
                "NAN in traceline:\nv1(%f %f %f) v2(%f %f %f)\nentity %d\n",
                v1[0], v1[1], v1[2], v2[0], v2[1], v2[2], NUM_FOR_EDICT(ent));
        }
    }

    if(IS_NAN(v1[0]) || IS_NAN(v1[1]) || IS_NAN(v1[2]))
    {
        v1[0] = v1[1] = v1[2] = 0;
    }
    if(IS_NAN(v2[0]) || IS_NAN(v2[1]) || IS_NAN(v2[2]))
    {
        v2[0] = v2[1] = v2[2] = 0;
    }

    trace_t trace = SV_Move(v1, mins, maxs, v2, nomonsters, ent);

    pr_global_struct->trace_allsolid = trace.allsolid;
    pr_global_struct->trace_startsolid = trace.startsolid;
    pr_global_struct->trace_fraction = trace.fraction;
    pr_global_struct->trace_inwater = trace.inwater;
    pr_global_struct->trace_inopen = trace.inopen;
    pr_global_struct->trace_endpos = trace.endpos;
    pr_global_struct->trace_plane_normal = trace.plane.normal;
    pr_global_struct->trace_plane_dist = trace.plane.dist;

    if(trace.ent)
    {
        pr_global_struct->trace_ent = EDICT_TO_PROG(trace.ent);
    }
    else
    {
        pr_global_struct->trace_ent = EDICT_TO_PROG(qcvm->edicts);
    }
}

static void PF_TraceToss()
{
    extern cvar_t sv_maxvelocity, sv_gravity;
    int i;
    float gravity;
    qvec3 move, end;
    trace_t trace;
    eval_t* val;

    qvec3 origin, velocity;

    edict_t *tossent, *ignore;
    tossent = G_EDICT(OFS_PARM0);
    if(tossent == qcvm->edicts)
    {
        Con_Warning("tracetoss: can not use world entity\n");
    }
    ignore = G_EDICT(OFS_PARM1);

    val = GetEdictFieldValue(tossent, qcvm->extfields.gravity);
    if(val && val->_float)
    {
        gravity = val->_float;
    }
    else
    {
        gravity = 1;
    }
    gravity *= sv_gravity.value * 0.05;

    origin = tossent->v.origin;
    velocity = tossent->v.velocity;

    SV_CheckVelocity(tossent);

    for(i = 0; i < 200;
        i++) // LordHavoc: sanity check; never trace more than 10 seconds
    {
        velocity[2] -= gravity;
        move = velocity * 0.05f;
        end = origin + move;
        trace = SV_Move(origin, tossent->v.mins, tossent->v.maxs, end,
            MOVE_NORMAL, tossent);
        origin = trace.endpos;

        if(trace.fraction < 1 && trace.ent && trace.ent != ignore)
        {
            break;
        }

        if(glm::length(velocity) > sv_maxvelocity.value)
        {
            //			Con_DPrintf("Slowing %s\n", PR_GetString(w->progs,
            // tossent->v->classname));
            velocity *= sv_maxvelocity.value / glm::length(velocity);
        }
    }

    trace.fraction = 0; // not relevant

    // and return those as globals.
    pr_global_struct->trace_allsolid = trace.allsolid;
    pr_global_struct->trace_startsolid = trace.startsolid;
    pr_global_struct->trace_fraction = trace.fraction;
    pr_global_struct->trace_inwater = trace.inwater;
    pr_global_struct->trace_inopen = trace.inopen;
    pr_global_struct->trace_endpos = trace.endpos;
    pr_global_struct->trace_plane_normal = trace.plane.normal;
    pr_global_struct->trace_plane_dist = trace.plane.dist;
    if(trace.ent)
    {
        pr_global_struct->trace_ent = EDICT_TO_PROG(trace.ent);
    }
    else
    {
        pr_global_struct->trace_ent = EDICT_TO_PROG(qcvm->edicts);
    }
}

// model stuff
void SetMinMaxSize(
    edict_t* e, const qvec3& minvec, const qvec3& maxvec, bool rotate);

static void PF_sv_setmodelindex()
{
    edict_t* e = G_EDICT(OFS_PARM0);
    unsigned int newidx = G_FLOAT(OFS_PARM1);
    qmodel_t* mod = qcvm->GetModel(newidx);
    e->v.model = (newidx < MAX_MODELS)
                     ? PR_SetEngineString(sv.model_precache[newidx])
                     : 0;
    e->v.modelindex = newidx;

    if(mod)
    // johnfitz -- correct physics cullboxes for bmodels
    {
        if(mod->type == mod_brush || !sv_gameplayfix_setmodelrealbox.value)
        {
            SetMinMaxSize(e, mod->clipmins, mod->clipmaxs, true);
        }
        else
        {
            SetMinMaxSize(e, mod->mins, mod->maxs, true);
        }
    }
    // johnfitz
    else
    {
        SetMinMaxSize(e, vec3_zero, vec3_zero, true);
    }
}
static void PF_cl_setmodelindex()
{
    edict_t* e = G_EDICT(OFS_PARM0);
    int newidx = G_FLOAT(OFS_PARM1);
    qmodel_t* mod = qcvm->GetModel(newidx);
    e->v.model =
        mod ? PR_SetEngineString(mod->name)
            : 0; // FIXME: is this going to cause issues with vid_restart?
    e->v.modelindex = newidx;

    if(mod)
    // johnfitz -- correct physics cullboxes for bmodels
    {
        if(mod->type == mod_brush || !sv_gameplayfix_setmodelrealbox.value)
        {
            SetMinMaxSize(e, mod->clipmins, mod->clipmaxs, true);
        }
        else
        {
            SetMinMaxSize(e, mod->mins, mod->maxs, true);
        }
    }
    // johnfitz
    else
    {
        SetMinMaxSize(e, vec3_zero, vec3_zero, true);
    }
}

static void PF_frameforname()
{
    unsigned int modelindex = G_FLOAT(OFS_PARM0);
    const char* framename = G_STRING(OFS_PARM1);
    qmodel_t* mod = qcvm->GetModel(modelindex);
    aliashdr_t* alias;

    G_FLOAT(OFS_RETURN) = -1;
    if(mod && mod->type == mod_alias &&
        (alias = (aliashdr_t*)Mod_Extradata(mod)))
    {
        int i;
        for(i = 0; i < alias->numframes; i++)
        {
            if(!strcmp(alias->frames[i].name, framename))
            {
                G_FLOAT(OFS_RETURN) = i;
                break;
            }
        }
    }
}
static void PF_frametoname()
{
    unsigned int modelindex = G_FLOAT(OFS_PARM0);
    unsigned int framenum = G_FLOAT(OFS_PARM1);
    qmodel_t* mod = qcvm->GetModel(modelindex);
    aliashdr_t* alias;

    if(mod && mod->type == mod_alias &&
        (alias = (aliashdr_t*)Mod_Extradata(mod)) &&
        framenum < (unsigned int)alias->numframes)
        G_INT(OFS_RETURN) = PR_SetEngineString(alias->frames[framenum].name);
    else
        G_INT(OFS_RETURN) = 0;
}
static void PF_frameduration()
{
    unsigned int modelindex = G_FLOAT(OFS_PARM0);
    unsigned int framenum = G_FLOAT(OFS_PARM1);
    qmodel_t* mod = qcvm->GetModel(modelindex);
    aliashdr_t* alias;

    if(mod && mod->type == mod_alias &&
        (alias = (aliashdr_t*)Mod_Extradata(mod)) &&
        framenum < (unsigned int)alias->numframes)
        G_FLOAT(OFS_RETURN) =
            alias->frames[framenum].numposes * alias->frames[framenum].interval;
}
static void PF_getsurfacenumpoints()
{
    edict_t* ed = G_EDICT(OFS_PARM0);
    unsigned int surfidx = G_FLOAT(OFS_PARM1);
    unsigned int modelindex = ed->v.modelindex;
    qmodel_t* mod = qcvm->GetModel(modelindex);

    if(mod && mod->type == mod_brush && !mod->needload &&
        surfidx < (unsigned int)mod->nummodelsurfaces)
    {
        surfidx += mod->firstmodelsurface;
        G_FLOAT(OFS_RETURN) = mod->surfaces[surfidx].numedges;
    }
    else
    {
        G_FLOAT(OFS_RETURN) = 0;
    }
}
static mvertex_t* PF_getsurfacevertex(
    qmodel_t* mod, msurface_t* surf, unsigned int vert)
{
    signed int edge = mod->surfedges[vert + surf->firstedge];
    if(edge >= 0)
    {
        return &mod->vertexes[mod->edges[edge].v[0]];
    }
    else
    {
        return &mod->vertexes[mod->edges[-edge].v[1]];
    }
}
static void PF_getsurfacepoint()
{
    edict_t* ed = G_EDICT(OFS_PARM0);
    unsigned int surfidx = G_FLOAT(OFS_PARM1);
    unsigned int point = G_FLOAT(OFS_PARM2);
    qmodel_t* mod = qcvm->GetModel(ed->v.modelindex);

    if(mod && mod->type == mod_brush && !mod->needload &&
        surfidx < (unsigned int)mod->nummodelsurfaces &&
        point < (unsigned int)mod->surfaces[surfidx].numedges)
    {
        mvertex_t* v = PF_getsurfacevertex(
            mod, &mod->surfaces[surfidx + mod->firstmodelsurface], point);

        returnVector(v->position);
    }
    else
    {
        G_FLOAT(OFS_RETURN + 0) = 0;
        G_FLOAT(OFS_RETURN + 1) = 0;
        G_FLOAT(OFS_RETURN + 2) = 0;
    }
}
static void PF_getsurfacenumtriangles()
{ // for q3bsp compat (which this engine doesn't support, so its fairly simple)
    edict_t* ed = G_EDICT(OFS_PARM0);
    unsigned int surfidx = G_FLOAT(OFS_PARM1);
    qmodel_t* mod = qcvm->GetModel(ed->v.modelindex);

    if(mod && mod->type == mod_brush && !mod->needload &&
        surfidx < (unsigned int)mod->nummodelsurfaces)
    {
        G_FLOAT(OFS_RETURN) =
            (mod->surfaces[surfidx + mod->firstmodelsurface].numedges -
                2); // q1bsp is only triangle fans
    }
    else
    {
        G_FLOAT(OFS_RETURN) = 0;
    }
}
static void PF_getsurfacetriangle()
{ // for q3bsp compat (which this engine doesn't support, so its fairly simple)
    edict_t* ed = G_EDICT(OFS_PARM0);
    unsigned int surfidx = G_FLOAT(OFS_PARM1);
    unsigned int triangleidx = G_FLOAT(OFS_PARM2);
    qmodel_t* mod = qcvm->GetModel(ed->v.modelindex);

    if(mod && mod->type == mod_brush && !mod->needload &&
        surfidx < (unsigned int)mod->nummodelsurfaces &&
        triangleidx < (unsigned int)mod->surfaces[surfidx].numedges - 2)
    {
        G_FLOAT(OFS_RETURN + 0) = 0;
        G_FLOAT(OFS_RETURN + 1) = triangleidx + 1;
        G_FLOAT(OFS_RETURN + 2) = triangleidx + 2;
    }
    else
    {
        G_FLOAT(OFS_RETURN + 0) = 0;
        G_FLOAT(OFS_RETURN + 1) = 0;
        G_FLOAT(OFS_RETURN + 2) = 0;
    }
}
static void PF_getsurfacenormal()
{
    edict_t* ed = G_EDICT(OFS_PARM0);
    unsigned int surfidx = G_FLOAT(OFS_PARM1);
    qmodel_t* mod = qcvm->GetModel(ed->v.modelindex);

    if(mod && mod->type == mod_brush && !mod->needload &&
        surfidx < (unsigned int)mod->nummodelsurfaces)
    {
        surfidx += mod->firstmodelsurface;

        if(mod->surfaces[surfidx].flags & SURF_PLANEBACK)
        {
            returnVector(-mod->surfaces[surfidx].plane->normal);
        }
        else
        {
            returnVector(mod->surfaces[surfidx].plane->normal);
        }
    }
    else
    {
        G_FLOAT(OFS_RETURN) = 0;
    }
}
static void PF_getsurfacetexture()
{
    edict_t* ed = G_EDICT(OFS_PARM0);
    unsigned int surfidx = G_FLOAT(OFS_PARM1);
    qmodel_t* mod = qcvm->GetModel(ed->v.modelindex);

    if(mod && mod->type == mod_brush && !mod->needload &&
        surfidx < (unsigned int)mod->nummodelsurfaces)
    {
        surfidx += mod->firstmodelsurface;
        G_INT(OFS_RETURN) =
            PR_SetEngineString(mod->surfaces[surfidx].texinfo->texture->name);
    }
    else
    {
        G_INT(OFS_RETURN) = 0;
    }
}

#define TriangleNormal(a, b, c, n)                       \
    ((n)[0] = ((a)[1] - (b)[1]) * ((c)[2] - (b)[2]) -    \
              ((a)[2] - (b)[2]) * ((c)[1] - (b)[1]),     \
        (n)[1] = ((a)[2] - (b)[2]) * ((c)[0] - (b)[0]) - \
                 ((a)[0] - (b)[0]) * ((c)[2] - (b)[2]),  \
        (n)[2] = ((a)[0] - (b)[0]) * ((c)[1] - (b)[1]) - \
                 ((a)[1] - (b)[1]) * ((c)[0] - (b)[0]))
static float getsurface_clippointpoly(qmodel_t* model, msurface_t* surf,
    qvec3& point, qvec3& bestcpoint, float bestdist)
{
    int e, edge;
    qvec3 edgedir, edgenormal, cpoint, temp;
    mvertex_t *v1, *v2;
    float dist = DotProduct(point, surf->plane->normal) - surf->plane->dist;
    // don't care about SURF_PLANEBACK, the maths works out the same.

    if(dist * dist < bestdist)
    { // within a specific range
        // make sure it's within the poly
        cpoint = point + dist * surf->plane->normal;
        for(e = surf->firstedge + surf->numedges; e > surf->firstedge; edge++)
        {
            edge = model->surfedges[--e];
            if(edge < 0)
            {
                v1 = &model->vertexes[model->edges[-edge].v[0]];
                v2 = &model->vertexes[model->edges[-edge].v[1]];
            }
            else
            {
                v2 = &model->vertexes[model->edges[edge].v[0]];
                v1 = &model->vertexes[model->edges[edge].v[1]];
            }

            edgedir = v1->position - v2->position;
            edgenormal = CrossProduct(edgedir, surf->plane->normal);
            if(!(surf->flags & SURF_PLANEBACK))
            {
                edgenormal = -edgenormal;
            }
            edgenormal = glm::normalize(edgenormal);

            dist = DotProduct(v1->position, edgenormal) -
                   DotProduct(cpoint, edgenormal);
            if(dist < 0)
            {
                cpoint = cpoint + dist * edgenormal;
            }
        }

        temp = cpoint - point;
        dist = DotProduct(temp, temp);
        if(dist < bestdist)
        {
            bestdist = dist;
            bestcpoint = cpoint;
        }
    }
    return bestdist;
}

// #438 float(entity e, vector p) getsurfacenearpoint (DP_QC_GETSURFACE)
static void PF_getsurfacenearpoint()
{
    qmodel_t* model;
    edict_t* ent;
    msurface_t* surf;
    int i;
    qvec3 point;

    qvec3 cpoint = {0, 0, 0};
    float bestdist = 0x7fffffff, dist;
    int bestsurf = -1;

    ent = G_EDICT(OFS_PARM0);
    point = extractVector(OFS_PARM1);

    G_FLOAT(OFS_RETURN) = -1;

    model = qcvm->GetModel(ent->v.modelindex);

    if(!model || model->type != mod_brush || model->needload)
    {
        return;
    }

    bestdist = 256;

    // all polies, we can skip parts. special case.
    surf = model->surfaces + model->firstmodelsurface;
    for(i = 0; i < model->nummodelsurfaces; i++, surf++)
    {
        dist = getsurface_clippointpoly(model, surf, point, cpoint, bestdist);
        if(dist < bestdist)
        {
            bestdist = dist;
            bestsurf = i;
        }
    }
    G_FLOAT(OFS_RETURN) = bestsurf;
}

// #439 vector(entity e, float s, vector p) getsurfaceclippedpoint
// (DP_QC_GETSURFACE)
static void PF_getsurfaceclippedpoint()
{
    qmodel_t* model;
    edict_t* ent;
    msurface_t* surf;
    qvec3 point;
    int surfnum;

    qvec3 result;

    ent = G_EDICT(OFS_PARM0);
    surfnum = G_FLOAT(OFS_PARM1);
    point = extractVector(OFS_PARM2);

    result = point;


    model = qcvm->GetModel(ent->v.modelindex);

    if(!model || model->type != mod_brush || model->needload)
    {
        returnVector(result);
        return;
    }

    if(surfnum >= model->nummodelsurfaces)
    {
        returnVector(result);
        return;
    }

    // all polies, we can skip parts. special case.
    surf = model->surfaces + model->firstmodelsurface + surfnum;
    getsurface_clippointpoly(model, surf, point, result, 0x7fffffff);
    returnVector(result);
}

static void PF_getsurfacepointattribute()
{
    edict_t* ed = G_EDICT(OFS_PARM0);
    unsigned int surfidx = G_FLOAT(OFS_PARM1);
    unsigned int point = G_FLOAT(OFS_PARM2);
    unsigned int attribute = G_FLOAT(OFS_PARM3);

    qmodel_t* mod = qcvm->GetModel(ed->v.modelindex);

    if(mod && mod->type == mod_brush && !mod->needload &&
        surfidx < (unsigned int)mod->nummodelsurfaces &&
        point < (unsigned int)mod->surfaces[mod->firstmodelsurface + surfidx]
                    .numedges)
    {
        msurface_t* fa = &mod->surfaces[surfidx + mod->firstmodelsurface];
        mvertex_t* v = PF_getsurfacevertex(mod, fa, point);
        switch(attribute)
        {
            default:
                Con_Warning(
                    "PF_getsurfacepointattribute: attribute %u not supported\n",
                    attribute);
                G_FLOAT(OFS_RETURN + 0) = 0;
                G_FLOAT(OFS_RETURN + 1) = 0;
                G_FLOAT(OFS_RETURN + 2) = 0;
                break;
            case 0: // xyz coord
                returnVector(v->position);
                break;
            case 1: // s dir
            case 2: // t dir
            {
                // figure out how similar to the normal it is, and negate any
                // influence, so that its perpendicular
                float sc = -DotProduct(
                    fa->plane->normal, fa->texinfo->vecs[attribute - 1]);

                const auto& rawvec = fa->texinfo->vecs[attribute - 1];
                qvec3 vec(rawvec[0], rawvec[1], rawvec[2]);

                qvec3 result = glm::normalize(vec + sc * fa->plane->normal);

                returnVector(result);
            }
            break;
            case 3: // normal
                if(fa->flags & SURF_PLANEBACK)
                {
                    returnVector(-fa->plane->normal);
                }
                else
                {
                    returnVector(fa->plane->normal);
                }
                break;
            case 4: // st coord
                G_FLOAT(OFS_RETURN + 0) =
                    (DotProduct(v->position, fa->texinfo->vecs[0]) +
                        fa->texinfo->vecs[0][3]) /
                    fa->texinfo->texture->width;
                G_FLOAT(OFS_RETURN + 1) =
                    (DotProduct(v->position, fa->texinfo->vecs[1]) +
                        fa->texinfo->vecs[1][3]) /
                    fa->texinfo->texture->height;
                G_FLOAT(OFS_RETURN + 2) = 0;
                break;
            case 5: // lmst coord, not actually very useful
                G_FLOAT(OFS_RETURN + 0) =
                    (DotProduct(v->position, fa->texinfo->vecs[0]) +
                        fa->texinfo->vecs[0][3] - fa->texturemins[0] +
                        (fa->light_s + .5) * (1 << fa->lmshift)) /
                    (LMBLOCK_WIDTH * (1 << fa->lmshift));
                G_FLOAT(OFS_RETURN + 1) =
                    (DotProduct(v->position, fa->texinfo->vecs[1]) +
                        fa->texinfo->vecs[1][3] - fa->texturemins[1] +
                        (fa->light_t + .5) * (1 << fa->lmshift)) /
                    (LMBLOCK_HEIGHT * (1 << fa->lmshift));
                G_FLOAT(OFS_RETURN + 2) = 0;
                break;
            case 6: // colour
                G_FLOAT(OFS_RETURN + 0) = 1;
                G_FLOAT(OFS_RETURN + 1) = 1;
                G_FLOAT(OFS_RETURN + 2) = 1;
                break;
        }
    }
    else
    {
        G_FLOAT(OFS_RETURN + 0) = 0;
        G_FLOAT(OFS_RETURN + 1) = 0;
        G_FLOAT(OFS_RETURN + 2) = 0;
    }
}
static void PF_sv_getlight()
{
    qmodel_t* om = cl.worldmodel;
    const qvec3 point = extractVector(OFS_PARM0);

    cl.worldmodel =
        qcvm->worldmodel; // R_LightPoint is really clientside, so if its called
                          // from ssqc then try to make things work regardless
                          // FIXME: d_lightstylevalue isn't set on dedicated
                          // servers

    // FIXME: seems like quakespasm doesn't do lits for model lighting, so we
    // won't either.
    G_FLOAT(OFS_RETURN + 0) = G_FLOAT(OFS_RETURN + 1) =
        G_FLOAT(OFS_RETURN + 2) = R_LightPoint(point) / 255.0;

    cl.worldmodel = om;
}
#define PF_cl_getlight PF_sv_getlight

// server/client stuff
static void PF_checkcommand()
{
    const char* name = G_STRING(OFS_PARM0);
    if(Cmd_Exists(name))
    {
        G_FLOAT(OFS_RETURN) = 1;
    }
    else if(Cmd_AliasExists(name))
    {
        G_FLOAT(OFS_RETURN) = 2;
    }
    else if(Cvar_FindVar(name))
    {
        G_FLOAT(OFS_RETURN) = 3;
    }
    else
    {
        G_FLOAT(OFS_RETURN) = 0;
    }
}
static void PF_setcolors()
{
    edict_t* ed = G_EDICT(OFS_PARM0);
    int newcol = G_FLOAT(OFS_PARM1);
    unsigned int i = NUM_FOR_EDICT(ed) - 1;
    if(i >= (unsigned int)svs.maxclients)
    {
        Con_Printf("tried to setcolor a non-client\n");
        return;
    }
    // FIXME: should we allow this for inactive players?

    // update it
    svs.clients[i].colors = newcol;
    svs.clients[i].edict->v.team = (newcol & 15) + 1;

    // send notification to all clients
    MSG_WriteByte(&sv.reliable_datagram, svc_updatecolors);
    MSG_WriteByte(&sv.reliable_datagram, i);
    MSG_WriteByte(&sv.reliable_datagram, newcol);
}
static void PF_clientcommand()
{
    edict_t* ed = G_EDICT(OFS_PARM0);
    const char* str = G_STRING(OFS_PARM1);
    unsigned int i = NUM_FOR_EDICT(ed) - 1;
    if(i < (unsigned int)svs.maxclients && svs.clients[i].active)
    {
        client_t* ohc = host_client;
        host_client = &svs.clients[i];
        Cmd_ExecuteString(str, src_client);
        host_client = ohc;
    }
    else
    {
        Con_Printf("PF_clientcommand: not a client\n");
    }
}
static void PF_clienttype()
{
    edict_t* ed = G_EDICT(OFS_PARM0);
    unsigned int i = NUM_FOR_EDICT(ed) - 1;
    if(i >= (unsigned int)svs.maxclients)
    {
        G_FLOAT(OFS_RETURN) = 3; // CLIENTTYPE_NOTACLIENT
        return;
    }
    if(svs.clients[i].active)
    {
        if(svs.clients[i].netconnection)
        {
            G_FLOAT(OFS_RETURN) = 1; // CLIENTTYPE_REAL;
        }
        else
        {
            G_FLOAT(OFS_RETURN) = 2; // CLIENTTYPE_BOT;
        }
    }
    else
    {
        G_FLOAT(OFS_RETURN) = 0; // CLIENTTYPE_DISCONNECTED;
    }
}
static void PF_spawnclient()
{
    edict_t* ent;
    unsigned int i;
    if(svs.maxclients)
    {
        for(i = svs.maxclients; i-- > 0;)
        {
            if(!svs.clients[i].active)
            {
                svs.clients[i].netconnection =
                    nullptr; // botclients have no net connection, obviously.
                SV_ConnectClient(i);
                svs.clients[i].spawned = true;
                ent = svs.clients[i].edict;
                memset(&ent->v, 0, qcvm->progs->entityfields * 4);
                ent->v.colormap = NUM_FOR_EDICT(ent);
                ent->v.team = (svs.clients[i].colors & 15) + 1;
                ent->v.netname = PR_SetEngineString(svs.clients[i].name);
                RETURN_EDICT(ent);
                return;
            }
        }
    }
    RETURN_EDICT(qcvm->edicts);
}
static void PF_dropclient()
{
    edict_t* ed = G_EDICT(OFS_PARM0);
    unsigned int i = NUM_FOR_EDICT(ed) - 1;
    if(i < (unsigned int)svs.maxclients && svs.clients[i].active)
    { // FIXME: should really set a flag or something, to avoid recursion
      // issues.
        client_t* ohc = host_client;
        host_client = &svs.clients[i];
        SV_DropClient(false);
        host_client = ohc;
    }
}

// console/cvar stuff
static void PF_print()
{
    int i;
    for(i = 0; i < qcvm->argc; i++)
    {
        Con_Printf("%s", G_STRING(OFS_PARM0 + i * 3));
    }
}
static void PF_cvar_string()
{
    const char* name = G_STRING(OFS_PARM0);
    cvar_t* var = Cvar_FindVar(name);
    if(var && var->string)
    {
        // cvars can easily change values.
        // this would result in leaks/exploits/slowdowns if the qc spams calls
        // to cvar_string+changes. so keep performance consistent, even if this
        // is going to be slower.
        char* temp = PR_GetTempString();
        q_strlcpy(temp, var->string, STRINGTEMP_LENGTH);
        G_INT(OFS_RETURN) = PR_SetEngineString(temp);
    }
    else
    {
        G_INT(OFS_RETURN) = 0;
    }
}
static void PF_cvar_defstring()
{
    const char* name = G_STRING(OFS_PARM0);
    cvar_t* var = Cvar_FindVar(name);
    if(var && var->default_string)
    {
        G_INT(OFS_RETURN) = PR_SetEngineString(var->default_string);
    }
    else
    {
        G_INT(OFS_RETURN) = 0;
    }
}
static void PF_cvar_type()
{
    const char* str = G_STRING(OFS_PARM0);
    int ret = 0;
    cvar_t* v;

    v = Cvar_FindVar(str);
    if(v)
    {
        ret |= 1; // CVAR_EXISTS
        if(v->flags & CVAR_ARCHIVE)
        {
            ret |= 2; // CVAR_TYPE_SAVED
        }
        //		if(v->flags & CVAR_NOTFROMSERVER)
        //			ret |= 4; // CVAR_TYPE_PRIVATE
        if(!(v->flags & CVAR_USERDEFINED))
        {
            ret |= 8; // CVAR_TYPE_ENGINE
        }
        //		if (v->description)
        //			ret |= 16; // CVAR_TYPE_HASDESCRIPTION
    }
    G_FLOAT(OFS_RETURN) = ret;
}
static void PF_cvar_description()
{ // quakespasm does not support cvar descriptions. we provide this stub to
  // avoid crashes.
    G_INT(OFS_RETURN) = 0;
}
static void PF_registercvar()
{
    const char* name = G_STRING(OFS_PARM0);
    const char* value = (qcvm->argc > 1) ? G_STRING(OFS_PARM0) : "";
    Cvar_Create(name, value);
}

// temp entities + networking
static void PF_WriteString2()
{ // writes a string without the null. a poor-man's strcat.
    const char* string = G_STRING(OFS_PARM0);
    SZ_Write(WriteDest(), string, Q_strlen(string));
}
static void PF_WriteFloat()
{ // curiously, this was missing in vanilla.
    MSG_WriteFloat(WriteDest(), G_FLOAT(OFS_PARM0));
}
static void PF_sv_te_blooddp()
{ // blood is common enough that we should emulate it for when engines do
  // actually support it.
    const qvec3 org = extractVector(OFS_PARM0);
    const qvec3 dir = extractVector(OFS_PARM1);
    float color = 73;
    float count = G_FLOAT(OFS_PARM2);
    SV_StartParticle(org, dir, color, count);
}
static void PF_sv_te_bloodqw()
{ // qw tried to strip a lot.
    const qvec3 org = extractVector(OFS_PARM0);
    float color = 73;
    float count = G_FLOAT(OFS_PARM1) * 20;
    SV_StartParticle(org, vec3_zero, color, count);
}
static void PF_sv_te_lightningblood()
{ // a qw builtin, to replace particle.
    const qvec3 org = extractVector(OFS_PARM0);
    qvec3 dir = {0, 0, -100};
    float color = 20;
    float count = 225;
    SV_StartParticle(org, dir, color, count);
}
static void PF_sv_te_spike()
{
    const qvec3 org = extractVector(OFS_PARM0);
    MSG_WriteByte(&sv.datagram, svc_temp_entity);
    MSG_WriteByte(&sv.datagram, TE_SPIKE);
    MSG_WriteVec3(&sv.datagram, org, sv.protocolflags);
    SV_Multicast(MULTICAST_PVS_U, org, 0, 0);
}
static void PF_sv_te_superspike()
{
    const qvec3 org = extractVector(OFS_PARM0);
    MSG_WriteByte(&sv.datagram, svc_temp_entity);
    MSG_WriteByte(&sv.datagram, TE_SUPERSPIKE);
    MSG_WriteVec3(&sv.datagram, org, sv.protocolflags);
    SV_Multicast(MULTICAST_PVS_U, org, 0, 0);
}
static void PF_sv_te_gunshot()
{
    const qvec3 org = extractVector(OFS_PARM0);
    // float count = G_FLOAT(OFS_PARM1)*20;
    MSG_WriteByte(&sv.datagram, svc_temp_entity);
    MSG_WriteByte(&sv.datagram, TE_GUNSHOT);
    MSG_WriteVec3(&sv.datagram, org, sv.protocolflags);
    SV_Multicast(MULTICAST_PVS_U, org, 0, 0);
}
static void PF_sv_te_explosion()
{
    const qvec3 org = extractVector(OFS_PARM0);
    MSG_WriteByte(&sv.datagram, svc_temp_entity);
    MSG_WriteByte(&sv.datagram, TE_EXPLOSION);
    MSG_WriteVec3(&sv.datagram, org, sv.protocolflags);
    SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_sv_te_tarexplosion()
{
    const qvec3 org = extractVector(OFS_PARM0);
    MSG_WriteByte(&sv.datagram, svc_temp_entity);
    MSG_WriteByte(&sv.datagram, TE_TAREXPLOSION);
    MSG_WriteVec3(&sv.datagram, org, sv.protocolflags);
    SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_sv_te_lightning1()
{
    edict_t* ed = G_EDICT(OFS_PARM0);
    const qvec3 start = extractVector(OFS_PARM1);
    const qvec3 end = extractVector(OFS_PARM2);
    MSG_WriteByte(&sv.datagram, svc_temp_entity);
    MSG_WriteByte(&sv.datagram, TE_LIGHTNING1);
    MSG_WriteShort(&sv.datagram, NUM_FOR_EDICT(ed));
    MSG_WriteVec3(&sv.datagram, start, sv.protocolflags);
    MSG_WriteVec3(&sv.datagram, end, sv.protocolflags);
    SV_Multicast(MULTICAST_PHS_U, start, 0, 0);
}
static void PF_sv_te_lightning2()
{
    edict_t* ed = G_EDICT(OFS_PARM0);
    const qvec3 start = extractVector(OFS_PARM1);
    const qvec3 end = extractVector(OFS_PARM2);
    MSG_WriteByte(&sv.datagram, svc_temp_entity);
    MSG_WriteByte(&sv.datagram, TE_LIGHTNING2);
    MSG_WriteShort(&sv.datagram, NUM_FOR_EDICT(ed));
    MSG_WriteVec3(&sv.datagram, start, sv.protocolflags);
    MSG_WriteVec3(&sv.datagram, end, sv.protocolflags);
    SV_Multicast(MULTICAST_PHS_U, start, 0, 0);
}
static void PF_sv_te_wizspike()
{
    const qvec3 org = extractVector(OFS_PARM0);
    MSG_WriteByte(&sv.datagram, svc_temp_entity);
    MSG_WriteByte(&sv.datagram, TE_WIZSPIKE);
    MSG_WriteVec3(&sv.datagram, org, sv.protocolflags);
    SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_sv_te_knightspike()
{
    const qvec3 org = extractVector(OFS_PARM0);
    MSG_WriteByte(&sv.datagram, svc_temp_entity);
    MSG_WriteByte(&sv.datagram, TE_KNIGHTSPIKE);
    MSG_WriteVec3(&sv.datagram, org, sv.protocolflags);
    SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_sv_te_lightning3()
{
    edict_t* ed = G_EDICT(OFS_PARM0);
    const qvec3 start = extractVector(OFS_PARM1);
    const qvec3 end = extractVector(OFS_PARM2);
    MSG_WriteByte(&sv.datagram, svc_temp_entity);
    MSG_WriteByte(&sv.datagram, TE_LIGHTNING3);
    MSG_WriteShort(&sv.datagram, NUM_FOR_EDICT(ed));
    MSG_WriteVec3(&sv.datagram, start, sv.protocolflags);
    MSG_WriteVec3(&sv.datagram, end, sv.protocolflags);
    SV_Multicast(MULTICAST_PHS_U, start, 0, 0);
}
static void PF_sv_te_lavasplash()
{
    const qvec3 org = extractVector(OFS_PARM0);
    MSG_WriteByte(&sv.datagram, svc_temp_entity);
    MSG_WriteByte(&sv.datagram, TE_LAVASPLASH);
    MSG_WriteVec3(&sv.datagram, org, sv.protocolflags);
    SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_sv_te_teleport()
{
    const qvec3 org = extractVector(OFS_PARM0);
    MSG_WriteByte(&sv.multicast, svc_temp_entity);
    MSG_WriteByte(&sv.multicast, TE_TELEPORT);
    MSG_WriteCoord(&sv.multicast, org[0], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, org[1], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, org[2], sv.protocolflags);
    SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_sv_te_explosion2()
{
    const qvec3 org = extractVector(OFS_PARM0);
    int palstart = G_FLOAT(OFS_PARM1);
    int palcount = G_FLOAT(OFS_PARM1);
    MSG_WriteByte(&sv.multicast, svc_temp_entity);
    MSG_WriteByte(&sv.multicast, TE_EXPLOSION2);
    MSG_WriteCoord(&sv.multicast, org[0], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, org[1], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, org[2], sv.protocolflags);
    MSG_WriteByte(&sv.multicast, palstart);
    MSG_WriteByte(&sv.multicast, palcount);
    SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_sv_te_beam()
{
    edict_t* ed = G_EDICT(OFS_PARM0);
    const qvec3 start = extractVector(OFS_PARM1);
    const qvec3 end = extractVector(OFS_PARM2);
    MSG_WriteByte(&sv.multicast, svc_temp_entity);
    MSG_WriteByte(&sv.multicast, TE_BEAM);
    MSG_WriteShort(&sv.multicast, NUM_FOR_EDICT(ed));
    MSG_WriteCoord(&sv.multicast, start[0], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, start[1], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, start[2], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, end[0], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, end[1], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, end[2], sv.protocolflags);
    SV_Multicast(MULTICAST_PHS_U, start, 0, 0);
}
#ifdef PSET_SCRIPT
static void PF_sv_te_particlerain()
{
    float* min = G_VECTOR(OFS_PARM0);
    float* max = G_VECTOR(OFS_PARM1);
    float* velocity = G_VECTOR(OFS_PARM2);
    float count = G_FLOAT(OFS_PARM3);
    float colour = G_FLOAT(OFS_PARM4);

    if(count < 1)
    {
        return;
    }
    if(count > 65535)
    {
        count = 65535;
    }

    MSG_WriteByte(&sv.multicast, svc_temp_entity);
    MSG_WriteByte(&sv.multicast, TEDP_PARTICLERAIN);
    MSG_WriteCoord(&sv.multicast, min[0], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, min[1], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, min[2], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, max[0], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, max[1], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, max[2], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, velocity[0], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, velocity[1], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, velocity[2], sv.protocolflags);
    MSG_WriteShort(&sv.multicast, count);
    MSG_WriteByte(&sv.multicast, colour);

    SV_Multicast(MULTICAST_ALL_U, nullptr, 0, PEXT2_REPLACEMENTDELTAS);
}
static void PF_sv_te_particlesnow()
{
    float* min = G_VECTOR(OFS_PARM0);
    float* max = G_VECTOR(OFS_PARM1);
    float* velocity = G_VECTOR(OFS_PARM2);
    float count = G_FLOAT(OFS_PARM3);
    float colour = G_FLOAT(OFS_PARM4);

    if(count < 1)
    {
        return;
    }
    if(count > 65535)
    {
        count = 65535;
    }

    MSG_WriteByte(&sv.multicast, svc_temp_entity);
    MSG_WriteByte(&sv.multicast, TEDP_PARTICLESNOW);
    MSG_WriteCoord(&sv.multicast, min[0], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, min[1], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, min[2], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, max[0], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, max[1], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, max[2], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, velocity[0], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, velocity[1], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, velocity[2], sv.protocolflags);
    MSG_WriteShort(&sv.multicast, count);
    MSG_WriteByte(&sv.multicast, colour);

    SV_Multicast(MULTICAST_ALL_U, nullptr, 0, PEXT2_REPLACEMENTDELTAS);
}
#else
#define PF_sv_te_particlerain PF_void_stub
#define PF_sv_te_particlesnow PF_void_stub
#endif
#define PF_sv_te_bloodshower PF_void_stub
#define PF_sv_te_explosionrgb PF_void_stub
#define PF_sv_te_particlecube PF_void_stub
#define PF_sv_te_spark PF_void_stub
#define PF_sv_te_gunshotquad PF_sv_te_gunshot
#define PF_sv_te_spikequad PF_sv_te_spike
#define PF_sv_te_superspikequad PF_sv_te_superspike
#define PF_sv_te_explosionquad PF_sv_te_explosion
#define PF_sv_te_smallflash PF_void_stub
#define PF_sv_te_customflash PF_void_stub
#define PF_sv_te_plasmaburn PF_sv_te_tarexplosion
#define PF_sv_effect PF_void_stub

static void PF_sv_pointsound()
{
    const qvec3 origin = extractVector(OFS_PARM0);
    const char* sample = G_STRING(OFS_PARM1);
    float volume = G_FLOAT(OFS_PARM2);
    float attenuation = G_FLOAT(OFS_PARM3);
    SV_StartSound(qcvm->edicts, &origin, 0, sample, volume, attenuation);
}
static void PF_cl_pointsound()
{
    const qvec3 origin = extractVector(OFS_PARM0);
    const char* sample = G_STRING(OFS_PARM1);
    float volume = G_FLOAT(OFS_PARM2);
    float attenuation = G_FLOAT(OFS_PARM3);
    S_StartSound(0, 0, S_PrecacheSound(sample), origin, volume, attenuation);
}
static void PF_cl_soundlength()
{
    const char* sample = G_STRING(OFS_PARM0);
    sfx_t* sfx = S_PrecacheSound(sample);
    sfxcache_t* sc;
    G_FLOAT(OFS_RETURN) = 0;
    if(sfx)
    {
        sc = S_LoadSound(sfx);
        if(sc)
        {
            G_FLOAT(OFS_RETURN) = (double)sc->length / sc->speed;
        }
    }
}

// file stuff

// returns false if the file is denied.
// fallbackread can be nullptr, if the qc is not allowed to read that (original)
// file at all.
static bool QC_FixFileName(
    const char* name, const char** result, const char** fallbackread)
{
    if(!*name ||              // blank names are bad
        strchr(name, ':') ||  // dos/win absolute path, ntfs ADS, amiga drives.
                              // reject them all.
        strchr(name, '\\') || // windows-only paths.
        *name == '/' ||       // absolute path was given - reject
        strstr(name, ".."))   // someone tried to be clever.
    {
        return false;
    }

    *fallbackread = name;
    // if its a user config, ban any fallback locations so that csqc can't read
    // passwords or whatever.
    if((!strchr(name, '/') || q_strncasecmp(name, "configs/", 8)) &&
        !q_strcasecmp(COM_FileGetExtension(name), "cfg") &&
        q_strncasecmp(name, "particles/", 10) &&
        q_strncasecmp(name, "huds/", 5) && q_strncasecmp(name, "models/", 7))
    {
        *fallbackread = nullptr;
    }
    *result = va("data/%s", name);
    return true;
}

// small note on access modes:
// when reading, we fopen files inside paks, for compat with (crappy
// non-zip-compatible) filesystem code when writing, we directly fopen the file
// such that it can never be inside a pak. this means that we need to take care
// when reading in order to detect EOF properly. writing doesn't need anything
// like that, so it can just dump stuff out, but we do need to ensure that the
// modes don't get mixed up, because trying to read from a writable file will
// not do what you would expect. even libc mandates a seek between
// reading+writing, so no great loss there.
static struct qcfile_s
{
    qcvm_t* owningvm;
    char cache[1024];
    int cacheoffset, cachesize;
    FILE* file;
    int fileoffset;
    int filesize;
    int filebase; // the offset of the file inside a pak
    int mode;
} * qcfiles;
static size_t qcfiles_max;
#define QC_FILE_BASE 1
static void PF_fopen()
{
    const char* fname = G_STRING(OFS_PARM0);
    int fmode = G_FLOAT(OFS_PARM1);
    const char* fallback;
    FILE* file;
    size_t i;
    char name[MAX_OSPATH], *sl;
    int filesize = 0;

    G_FLOAT(OFS_RETURN) = -1; // assume failure

    if(!QC_FixFileName(fname, &fname, &fallback))
    {
        Con_Printf("qcfopen: Access denied: %s\n", fname);
        return;
    }
    // if we were told to use 'foo.txt'
    // fname is now 'data/foo.txt'
    // fallback is now 'foo.txt', and must ONLY be read.

    switch(fmode)
    {
        case 0: // read
            filesize = COM_FOpenFile(fname, &file, nullptr);
            if(!file && fallback)
            {
                filesize = COM_FOpenFile(fallback, &file, nullptr);
            }
            break;
        case 1: // append
            q_snprintf(name, sizeof(name), "%s/%s", com_gamedir, fname);
            Sys_mkdir(name);
            file = fopen(name, "w+b");
            if(file)
            {
                fseek(file, 0, SEEK_END);
            }
            break;
        case 2: // write
            q_snprintf(name, sizeof(name), "%s/%s", com_gamedir, fname);
            sl = name;
            while(*sl)
            {
                if(*sl == '/')
                {
                    *sl = 0;
                    Sys_mkdir(name); // make sure each part of the path exists.
                    *sl = '/';
                }
                sl++;
            }
            file = fopen(name, "wb");
            break;
    }
    if(!file)
    {
        return;
    }

    for(i = 0;; i++)
    {
        if(i == qcfiles_max)
        {
            qcfiles_max++;
            qcfiles = (struct qcfile_s*)Z_Realloc(
                qcfiles, sizeof(*qcfiles) * qcfiles_max);
        }
        if(!qcfiles[i].file)
        {
            break;
        }
    }
    qcfiles[i].filebase = ftell(file);
    qcfiles[i].owningvm = qcvm;
    qcfiles[i].file = file;
    qcfiles[i].mode = fmode;
    // reading needs size info
    qcfiles[i].filesize = filesize;
    // clear the read cache.
    qcfiles[i].fileoffset = qcfiles[i].cacheoffset = qcfiles[i].cachesize = 0;

    G_FLOAT(OFS_RETURN) = i + QC_FILE_BASE;
}
static void PF_fgets()
{
    size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
    G_INT(OFS_RETURN) = 0;
    if(fileid >= qcfiles_max)
    {
        Con_Warning("PF_fgets: invalid file handle\n");
    }
    else if(!qcfiles[fileid].file)
    {
        Con_Warning("PF_fgets: file not open\n");
    }
    else if(qcfiles[fileid].mode != 0)
    {
        Con_Warning("PF_fgets: file not open for reading\n");
    }
    else
    {
        struct qcfile_s* f = &qcfiles[fileid];
        char* ret = PR_GetTempString();
        char* s = ret;
        char* end = ret + STRINGTEMP_LENGTH;
        for(;;)
        {
            if(f->cacheoffset == f->cachesize)
            {
                // figure out how much we can try to cache.
                int sz = f->filesize - f->fileoffset;
                if(sz < 0 || f->fileoffset < 0)
                { //... maybe we shouldn't have
                    // implemented seek support.
                    sz = 0;
                }
                else if((size_t)sz > sizeof(f->cache))
                {
                    sz = sizeof(f->cache);
                }
                // read a chunk
                f->cacheoffset = 0;
                f->cachesize = fread(f->cache, 1, sz, f->file);
                f->fileoffset += f->cachesize;
                if(!f->cachesize)
                {
                    if(s == ret)
                    { // absolutely nothing to spew
                        G_INT(OFS_RETURN) = 0;
                        return;
                    }
                    // classic eof...
                    break;
                }
            }
            *s = f->cache[f->cacheoffset++];
            if(*s == '\n')
            { // new line, yay!
                break;
            }
            s++;
            if(s == end)
            {
                s--; // rewind if we're overflowing, such that we truncate the
            }
            // string.
        }
        if(s > ret && s[-1] == '\r')
        {
            s--; // terminate it on the \r of a \r\n pair.
        }
        *s = 0; // terminate it
        G_INT(OFS_RETURN) = PR_SetEngineString(ret);
    }
}
static void PF_fputs()
{
    size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
    const char* str = PF_VarString(1);
    if(fileid >= qcfiles_max)
    {
        Con_Warning("PF_fputs: invalid file handle\n");
    }
    else if(!qcfiles[fileid].file)
    {
        Con_Warning("PF_fputs: file not open\n");
    }
    else if(qcfiles[fileid].mode == 0)
    {
        Con_Warning("PF_fgets: file not open for writing\n");
    }
    else
    {
        fputs(str, qcfiles[fileid].file);
    }
}
static void PF_fclose()
{
    size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
    if(fileid >= qcfiles_max)
    {
        Con_Warning("PF_fclose: invalid file handle\n");
    }
    else if(!qcfiles[fileid].file)
    {
        Con_Warning("PF_fclose: file not open\n");
    }
    else
    {
        fclose(qcfiles[fileid].file);
        qcfiles[fileid].file = nullptr;
        qcfiles[fileid].owningvm = nullptr;
    }
}
static void PF_frikfile_shutdown()
{
    size_t i;
    for(i = 0; i < qcfiles_max; i++)
    {
        if(qcfiles[i].owningvm == qcvm)
        {
            fclose(qcfiles[i].file);
            qcfiles[i].file = nullptr;
            qcfiles[i].owningvm = nullptr;
        }
    }
}
static void PF_fseek()
{ // returns current position. or changes that position.
    size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
    G_INT(OFS_RETURN) = 0;
    if(fileid >= qcfiles_max)
    {
        Con_Warning("PF_fread: invalid file handle\n");
    }
    else if(!qcfiles[fileid].file)
    {
        Con_Warning("PF_fread: file not open\n");
    }
    else
    {
        if(qcfiles[fileid].mode == 0)
        {
            G_INT(OFS_RETURN) =
                qcfiles[fileid].fileoffset; // when we're reading, use the
                                            // cached read offset
        }
        else
        {
            G_INT(OFS_RETURN) =
                ftell(qcfiles[fileid].file) - qcfiles[fileid].filebase;
        }
        if(qcvm->argc > 1)
        {
            qcfiles[fileid].fileoffset = G_INT(OFS_PARM1);
            fseek(qcfiles[fileid].file,
                qcfiles[fileid].filebase + qcfiles[fileid].fileoffset,
                SEEK_SET);
            qcfiles[fileid].cachesize = qcfiles[fileid].cacheoffset = 0;
        }
    }
}
#if 0
static void PF_fread()
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	int qcptr = G_INT(OFS_PARM1);
	size_t size = G_INT(OFS_PARM2);

	//FIXME: validate. make usable.
	char *nativeptr = (char*)sv.edicts + qcptr;

	G_INT(OFS_RETURN) = 0;
	if (fileid >= maxfiles)
		Con_Warning("PF_fread: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fread: file not open\n");
	else
		G_INT(OFS_RETURN) = fread(nativeptr, 1, size, qcfiles[fileid].file);
}
static void PF_fwrite()
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	int qcptr = G_INT(OFS_PARM1);
	size_t size = G_INT(OFS_PARM2);

	//FIXME: validate. make usable.
	const char *nativeptr = (const char*)sv.edicts + qcptr;

	G_INT(OFS_RETURN) = 0;
	if (fileid >= maxfiles)
		Con_Warning("PF_fwrite: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fwrite: file not open\n");
	else
		G_INT(OFS_RETURN) = fwrite(nativeptr, 1, size, qcfiles[fileid].file);
}
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
static void PF_fsize()
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	G_INT(OFS_RETURN) = 0;
	if (fileid >= maxfiles)
		Con_Warning("PF_fread: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fread: file not open\n");
	else if (qcfiles[fileid].mode == 0)
	{
		G_INT(OFS_RETURN) = qcfiles[fileid].filesize;
		//can't truncate if we're reading.
	}
	else
	{
		long curpos = ftell(qcfiles[fileid].file);
		fseek(qcfiles[fileid].file, 0, SEEK_END);
		G_INT(OFS_RETURN) = ftell(qcfiles[fileid].file);
		fseek(qcfiles[fileid].file, curpos, SEEK_SET);

		if (pr_argc>1)
		{
			//specifically resize. or maybe extend.
#ifdef _WIN32
			_chsize(fileno(qcfiles[fileid].file), G_INT(OFS_PARM1));
#else
			ftruncate(fileno(qcfiles[fileid].file), G_INT(OFS_PARM1));
#endif
		}
	}
}
#endif

struct filesearch_s
{
    qcvm_t* owner;
    size_t numfiles;
    size_t maxfiles;
    struct filesearch_file_t
    {
        char name[MAX_QPATH];
        time_t mtime;
        size_t fsize;
    } * file;
} searches[16];
static bool PR_Search_AddFile(
    void* ctx, const char* fname, time_t mtime, size_t fsize)
{
    struct filesearch_s* c = (struct filesearch_s*)ctx;
    if(c->numfiles == c->maxfiles)
    {
        c->maxfiles = c->maxfiles * 2 + 2;
        c->file = (filesearch_s::filesearch_file_t*)realloc(
            c->file, c->maxfiles * sizeof(*c->file));
    }
    q_strlcpy(
        c->file[c->numfiles].name, fname, sizeof(c->file[c->numfiles].name));
    c->file[c->numfiles].mtime = mtime;
    c->file[c->numfiles].fsize = fsize;
    c->numfiles++;
    return true;
}
void COM_ListFiles(void* ctx, const char* gamedir, const char* pattern,
    bool (*cb)(void* ctx, const char* fname, time_t mtime, size_t fsize));
static void PF_search_shutdown()
{
    size_t i;
    for(i = 0; i < countof(searches); i++)
    {
        if(searches[i].owner == qcvm)
        {
            searches[i].owner = nullptr;
            searches[i].numfiles = 0;
            searches[i].maxfiles = 0;
            free(searches[i].file);
            searches[i].file = nullptr;
        }
    }
}

static void PF_search_begin()
{
    size_t i;
    const char* pattern = G_STRING(OFS_PARM0);
    //	bool caseinsensitive = !!G_FLOAT(OFS_PARM0);
    //	qboolaen quiet = !!G_FLOAT(OFS_PARM0);

    for(i = 0; i < countof(searches); i++)
    {
        if(!searches[i].owner)
        {
            COM_ListFiles(
                &searches[i], com_gamedir, pattern, PR_Search_AddFile);
            if(!searches[i].numfiles)
            {
                break;
            }
            searches[i].owner = qcvm;
            G_FLOAT(OFS_RETURN) = i;
            return;
        }
    }
    G_FLOAT(OFS_RETURN) = -1;
}
static void PF_search_end()
{
    int handle = G_FLOAT(OFS_PARM0);
    if(handle < 0 || handle >= countof(searches))
    {
        return; // erk
    }
    searches[handle].owner = nullptr;
    searches[handle].numfiles = 0;
    searches[handle].maxfiles = 0;
    free(searches[handle].file);
    searches[handle].file = nullptr;
}
static void PF_search_getsize()
{
    size_t handle = G_FLOAT(OFS_PARM0);
    if(handle < 0 || handle >= countof(searches))
    {
        G_FLOAT(OFS_RETURN) = 0;
        return; // erk
    }
    G_FLOAT(OFS_RETURN) = searches[handle].numfiles;
}
static void PF_search_getfilename()
{
    size_t handle = G_FLOAT(OFS_PARM0);
    size_t index = G_FLOAT(OFS_PARM1);
    G_INT(OFS_RETURN) = 0;
    if(handle < 0 || handle >= countof(searches))
    {
        return; // erk
    }
    if(index < 0 || index >= searches[handle].numfiles)
    {
        return; // erk
    }
    G_INT(OFS_RETURN) = PR_MakeTempString(searches[handle].file[index].name);
}
static void PF_search_getfilesize()
{
    size_t handle = G_FLOAT(OFS_PARM0);
    size_t index = G_FLOAT(OFS_PARM1);
    G_FLOAT(OFS_RETURN) = 0;
    if(handle < 0 || handle >= countof(searches))
    {
        return; // erk
    }
    if(index < 0 || index >= searches[handle].numfiles)
    {
        return; // erk
    }
    G_FLOAT(OFS_RETURN) = searches[handle].file[index].fsize;
}
static void PF_search_getfilemtime()
{
    char* ret = PR_GetTempString();
    size_t handle = G_FLOAT(OFS_PARM0);
    size_t index = G_FLOAT(OFS_PARM1);
    G_INT(OFS_RETURN) = 0;
    if(handle < 0 || handle >= countof(searches))
    {
        return; // erk
    }
    if(index < 0 || index >= searches[handle].numfiles)
    {
        return; // erk
    }

    strftime(ret, STRINGTEMP_LENGTH, "%Y-%m-%d %H:%M:%S",
        localtime(&searches[handle].file[index].mtime));
    G_INT(OFS_RETURN) = PR_SetEngineString(ret);
}

static void PF_whichpack()
{
    const char* fname = G_STRING(OFS_PARM0); // uses native paths, as this isn't
                                             // actually reading anything.
    unsigned int path_id;
    if(COM_FileExists(fname, &path_id))
    {
        // FIXME: quakespasm reports which gamedir the file is in, but paks are
        // hidden. I'm too lazy to rewrite COM_FindFile, so I'm just going to
        // hack something small to get the gamedir, just not the paks

        searchpath_t* path;
        for(path = com_searchpaths; path; path = path->next)
        {
            if(!path->pack && path->path_id == path_id)
            {
                break; // okay, this one looks like one we can report
            }
        }

        // sandbox it by stripping the basedir
        fname = path->filename;
        if(!strncmp(fname, com_basedir, strlen(com_basedir)))
        {
            fname += strlen(com_basedir);
        }
        else
        {
            fname = "?"; // no idea where this came from. something is screwing
        }
        // with us.
        while(*fname == '/' || *fname == '\\')
        {
            fname++; // small cleanup, just in case
        }
        G_INT(OFS_RETURN) = PR_SetEngineString(fname);
    }
    else
    {
        G_INT(OFS_RETURN) = 0;
    }
}

// string buffers

struct strbuf
{
    qcvm_t* owningvm;
    char** strings;
    unsigned int used;
    unsigned int allocated;
};

#define BUFSTRBASE 1
#define NUMSTRINGBUFS 64u
struct strbuf strbuflist[NUMSTRINGBUFS];

static void PF_buf_shutdown()
{
    unsigned int i, bufno;

    for(bufno = 0; bufno < NUMSTRINGBUFS; bufno++)
    {
        if(strbuflist[bufno].owningvm != qcvm)
        {
            continue;
        }

        for(i = 0; i < strbuflist[bufno].used; i++)
        {
            Z_Free(strbuflist[bufno].strings[i]);
        }
        Z_Free(strbuflist[bufno].strings);

        strbuflist[bufno].owningvm = nullptr;
        strbuflist[bufno].strings = nullptr;
        strbuflist[bufno].used = 0;
        strbuflist[bufno].allocated = 0;
    }
}

// #440 float() buf_create (DP_QC_STRINGBUFFERS)
static void PF_buf_create()
{
    unsigned int i;

    const char* type = ((qcvm->argc > 0) ? G_STRING(OFS_PARM0) : "string");
    //	unsigned int flags = ((pr_argc>1)?G_FLOAT(OFS_PARM1):1);

    if(!q_strcasecmp(type, "string"))
    {
        ;
    }
    else
    {
        G_FLOAT(OFS_RETURN) = -1;
        return;
    }

    // flags&1 == saved. apparently.

    for(i = 0; i < NUMSTRINGBUFS; i++)
    {
        if(!strbuflist[i].owningvm)
        {
            strbuflist[i].owningvm = qcvm;
            strbuflist[i].used = 0;
            strbuflist[i].allocated = 0;
            strbuflist[i].strings = nullptr;
            G_FLOAT(OFS_RETURN) = i + BUFSTRBASE;
            return;
        }
    }
    G_FLOAT(OFS_RETURN) = -1;
}
// #441 void(float bufhandle) buf_del (DP_QC_STRINGBUFFERS)
static void PF_buf_del()
{
    unsigned int i;
    unsigned int bufno = G_FLOAT(OFS_PARM0) - BUFSTRBASE;

    if(bufno >= NUMSTRINGBUFS)
    {
        return;
    }
    if(!strbuflist[bufno].owningvm)
    {
        return;
    }

    for(i = 0; i < strbuflist[bufno].used; i++)
    {
        if(strbuflist[bufno].strings[i])
        {
            Z_Free(strbuflist[bufno].strings[i]);
        }
    }
    Z_Free(strbuflist[bufno].strings);

    strbuflist[bufno].strings = nullptr;
    strbuflist[bufno].used = 0;
    strbuflist[bufno].allocated = 0;

    strbuflist[bufno].owningvm = nullptr;
}
// #442 float(float bufhandle) buf_getsize (DP_QC_STRINGBUFFERS)
static void PF_buf_getsize()
{
    int bufno = G_FLOAT(OFS_PARM0) - BUFSTRBASE;

    if((unsigned int)bufno >= NUMSTRINGBUFS)
    {
        return;
    }
    if(!strbuflist[bufno].owningvm)
    {
        return;
    }

    G_FLOAT(OFS_RETURN) = strbuflist[bufno].used;
}
// #443 void(float bufhandle_from, float bufhandle_to) buf_copy
// (DP_QC_STRINGBUFFERS)
static void PF_buf_copy()
{
    unsigned int buffrom = G_FLOAT(OFS_PARM0) - BUFSTRBASE;
    unsigned int bufto = G_FLOAT(OFS_PARM1) - BUFSTRBASE;
    unsigned int i;

    if(bufto == buffrom)
    { // err...
        return;
    }
    if(buffrom >= NUMSTRINGBUFS)
    {
        return;
    }
    if(!strbuflist[buffrom].owningvm)
    {
        return;
    }
    if(bufto >= NUMSTRINGBUFS)
    {
        return;
    }
    if(!strbuflist[bufto].owningvm)
    {
        return;
    }

    // obliterate any and all existing data.
    for(i = 0; i < strbuflist[bufto].used; i++)
    {
        if(strbuflist[bufto].strings[i])
        {
            Z_Free(strbuflist[bufto].strings[i]);
        }
    }
    Z_Free(strbuflist[bufto].strings);

    // copy new data over.
    strbuflist[bufto].used = strbuflist[bufto].allocated =
        strbuflist[buffrom].used;
    strbuflist[bufto].strings =
        (char**)Z_Malloc(strbuflist[buffrom].used * sizeof(char*));
    for(i = 0; i < strbuflist[buffrom].used; i++)
        strbuflist[bufto].strings[i] =
            strbuflist[buffrom].strings[i]
                ? Z_StrDup(strbuflist[buffrom].strings[i])
                : nullptr;
}
static int PF_buf_sort_sortprefixlen;
static int PF_buf_sort_ascending(const void* a, const void* b)
{
    return strncmp(*(char**)a, *(char**)b, PF_buf_sort_sortprefixlen);
}
static int PF_buf_sort_descending(const void* b, const void* a)
{
    return strncmp(*(char**)a, *(char**)b, PF_buf_sort_sortprefixlen);
}
// #444 void(float bufhandle, float sortprefixlen, float backward) buf_sort
// (DP_QC_STRINGBUFFERS)
static void PF_buf_sort()
{
    int bufno = G_FLOAT(OFS_PARM0) - BUFSTRBASE;
    int sortprefixlen = G_FLOAT(OFS_PARM1);
    int backwards = G_FLOAT(OFS_PARM2);
    unsigned int s, d;
    char** strings;

    if((unsigned int)bufno >= NUMSTRINGBUFS)
    {
        return;
    }
    if(!strbuflist[bufno].owningvm)
    {
        return;
    }

    if(sortprefixlen <= 0)
    {
        sortprefixlen = 0x7fffffff;
    }

    // take out the nulls first, to avoid weird/crashy sorting
    for(s = 0, d = 0, strings = strbuflist[bufno].strings;
        s < strbuflist[bufno].used;)
    {
        if(!strings[s])
        {
            s++;
            continue;
        }
        strings[d++] = strings[s++];
    }
    strbuflist[bufno].used = d;

    // no nulls now, sort it.
    PF_buf_sort_sortprefixlen = sortprefixlen; // eww, a global. burn in hell.
    if(backwards)
    { // z first
        qsort(strings, strbuflist[bufno].used, sizeof(char*),
            PF_buf_sort_descending);
    }
    else
    { // a first
        qsort(strings, strbuflist[bufno].used, sizeof(char*),
            PF_buf_sort_ascending);
    }
}
// #445 string(float bufhandle, string glue) buf_implode (DP_QC_STRINGBUFFERS)
static void PF_buf_implode()
{
    int bufno = G_FLOAT(OFS_PARM0) - BUFSTRBASE;
    const char* glue = G_STRING(OFS_PARM1);
    unsigned int gluelen = strlen(glue);
    unsigned int retlen, l, i;
    char** strings;
    char* ret;

    if((unsigned int)bufno >= NUMSTRINGBUFS)
    {
        return;
    }
    if(!strbuflist[bufno].owningvm)
    {
        return;
    }

    // count neededlength
    strings = strbuflist[bufno].strings;
    /*
    for (i = 0, retlen = 0; i < strbuflist[bufno].used; i++)
    {
        if (strings[i])
        {
            if (retlen)
                retlen += gluelen;
            retlen += strlen(strings[i]);
        }
    }
    ret = malloc(retlen+1);*/

    // generate the output
    ret = PR_GetTempString();
    for(i = 0, retlen = 0; i < strbuflist[bufno].used; i++)
    {
        if(strings[i])
        {
            if(retlen)
            {
                if(retlen + gluelen + 1 > STRINGTEMP_LENGTH)
                {
                    Con_Printf("PF_buf_implode: tempstring overflow\n");
                    break;
                }
                memcpy(ret + retlen, glue, gluelen);
                retlen += gluelen;
            }
            l = strlen(strings[i]);
            if(retlen + l + 1 > STRINGTEMP_LENGTH)
            {
                Con_Printf("PF_buf_implode: tempstring overflow\n");
                break;
            }
            memcpy(ret + retlen, strings[i], l);
            retlen += l;
        }
    }

    // add the null and return
    ret[retlen] = 0;
    G_INT(OFS_RETURN) = PR_SetEngineString(ret);
}
// #446 string(float bufhandle, float string_index) bufstr_get
// (DP_QC_STRINGBUFFERS)
static void PF_bufstr_get()
{
    unsigned int bufno = G_FLOAT(OFS_PARM0) - BUFSTRBASE;
    unsigned int index = G_FLOAT(OFS_PARM1);
    char* ret;

    if(bufno >= NUMSTRINGBUFS)
    {
        G_INT(OFS_RETURN) = 0;
        return;
    }
    if(!strbuflist[bufno].owningvm)
    {
        G_INT(OFS_RETURN) = 0;
        return;
    }

    if(index >= strbuflist[bufno].used)
    {
        G_INT(OFS_RETURN) = 0;
        return;
    }

    if(strbuflist[bufno].strings[index])
    {
        ret = PR_GetTempString();
        q_strlcpy(ret, strbuflist[bufno].strings[index], STRINGTEMP_LENGTH);
        G_INT(OFS_RETURN) = PR_SetEngineString(ret);
    }
    else
    {
        G_INT(OFS_RETURN) = 0;
    }
}
// #447 void(float bufhandle, float string_index, string str) bufstr_set
// (DP_QC_STRINGBUFFERS)
static void PF_bufstr_set()
{
    unsigned int bufno = G_FLOAT(OFS_PARM0) - BUFSTRBASE;
    unsigned int index = G_FLOAT(OFS_PARM1);
    const char* string = G_STRING(OFS_PARM2);
    unsigned int oldcount;

    if((unsigned int)bufno >= NUMSTRINGBUFS)
    {
        return;
    }
    if(!strbuflist[bufno].owningvm)
    {
        return;
    }

    if(index >= strbuflist[bufno].allocated)
    {
        oldcount = strbuflist[bufno].allocated;
        strbuflist[bufno].allocated = (index + 256);
        strbuflist[bufno].strings = (char**)Z_Realloc(strbuflist[bufno].strings,
            strbuflist[bufno].allocated * sizeof(char*));
        memset(strbuflist[bufno].strings + oldcount, 0,
            (strbuflist[bufno].allocated - oldcount) * sizeof(char*));
    }
    if(strbuflist[bufno].strings[index])
    {
        Z_Free(strbuflist[bufno].strings[index]);
    }
    strbuflist[bufno].strings[index] = (char*)Z_Malloc(strlen(string) + 1);
    strcpy(strbuflist[bufno].strings[index], string);

    if(index >= strbuflist[bufno].used)
    {
        strbuflist[bufno].used = index + 1;
    }
}

static int PF_bufstr_add_internal(
    unsigned int bufno, const char* string, int appendonend)
{
    unsigned int index;
    if(appendonend)
    {
        // add on end
        index = strbuflist[bufno].used;
    }
    else
    {
        // find a hole
        for(index = 0; index < strbuflist[bufno].used; index++)
        {
            if(!strbuflist[bufno].strings[index])
            {
                break;
            }
        }
    }

    // expand it if needed
    if(index >= strbuflist[bufno].allocated)
    {
        unsigned int oldcount;
        oldcount = strbuflist[bufno].allocated;
        strbuflist[bufno].allocated = (index + 256);
        strbuflist[bufno].strings = (char**)Z_Realloc(strbuflist[bufno].strings,
            strbuflist[bufno].allocated * sizeof(char*));
        memset(strbuflist[bufno].strings + oldcount, 0,
            (strbuflist[bufno].allocated - oldcount) * sizeof(char*));
    }

    // add in the new string.
    if(strbuflist[bufno].strings[index])
    {
        Z_Free(strbuflist[bufno].strings[index]);
    }
    strbuflist[bufno].strings[index] = (char*)Z_Malloc(strlen(string) + 1);
    strcpy(strbuflist[bufno].strings[index], string);

    if(index >= strbuflist[bufno].used)
    {
        strbuflist[bufno].used = index + 1;
    }

    return index;
}

// #448 float(float bufhandle, string str, float order) bufstr_add
// (DP_QC_STRINGBUFFERS)
static void PF_bufstr_add()
{
    size_t bufno = G_FLOAT(OFS_PARM0) - BUFSTRBASE;
    const char* string = G_STRING(OFS_PARM1);
    bool ordered = G_FLOAT(OFS_PARM2);

    if((unsigned int)bufno >= NUMSTRINGBUFS)
    {
        return;
    }
    if(!strbuflist[bufno].owningvm)
    {
        return;
    }

    G_FLOAT(OFS_RETURN) = PF_bufstr_add_internal(bufno, string, ordered);
}
// #449 void(float bufhandle, float string_index) bufstr_free
// (DP_QC_STRINGBUFFERS)
static void PF_bufstr_free()
{
    size_t bufno = G_FLOAT(OFS_PARM0) - BUFSTRBASE;
    size_t index = G_FLOAT(OFS_PARM1);

    if((unsigned int)bufno >= NUMSTRINGBUFS)
    {
        return;
    }
    if(!strbuflist[bufno].owningvm)
    {
        return;
    }

    if(index >= strbuflist[bufno].used)
    {
        return; // not valid anyway.
    }

    if(strbuflist[bufno].strings[index])
    {
        Z_Free(strbuflist[bufno].strings[index]);
    }
    strbuflist[bufno].strings[index] = nullptr;
}

/*static void PF_buf_cvarlist()
{
    size_t bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
    const char *pattern = G_STRING(OFS_PARM1);
    const char *antipattern = G_STRING(OFS_PARM2);
    int i;
    cvar_t	*var;

    if ((unsigned int)bufno >= NUMSTRINGBUFS)
        return;
    if (!strbuflist[bufno].isactive)
        return;

    //obliterate any and all existing data.
    for (i = 0; i < strbuflist[bufno].used; i++)
        Z_Free(strbuflist[bufno].strings[i]);
    Z_Free(strbuflist[bufno].strings);
    strbuflist[bufno].used = strbuflist[bufno].allocated = 0;

    //ignore name2, no point listing it twice.
    for (var=Cvar_FindVarAfter ("", CVAR_NONE) ; var ; var=var->next)
    {
        if (pattern && wildcmp(pattern, var->name))
            continue;
        if (antipattern && !wildcmp(antipattern, var->name))
            continue;

        PF_bufstr_add_internal(bufno, var->name, true);
    }
}*/

// directly reads a file into a stringbuffer
static void PF_buf_loadfile()
{
    const char* fname = G_STRING(OFS_PARM0);
    size_t bufno = G_FLOAT(OFS_PARM1) - BUFSTRBASE;
    char *line, *nl;
    const char* fallback;

    G_FLOAT(OFS_RETURN) = 0;

    if((unsigned int)bufno >= NUMSTRINGBUFS)
    {
        return;
    }
    if(!strbuflist[bufno].owningvm)
    {
        return;
    }

    if(!QC_FixFileName(fname, &fname, &fallback))
    {
        Con_Printf("qcfopen: Access denied: %s\n", fname);
        return;
    }
    line = (char*)COM_LoadTempFile(fname, nullptr);
    if(!line && fallback)
    {
        line = (char*)COM_LoadTempFile(fallback, nullptr);
    }
    if(!line)
    {
        return;
    }

    while(line)
    {
        nl = strchr(line, '\n');
        if(nl)
        {
            *nl++ = 0;
        }
        PF_bufstr_add_internal(bufno, line, true);
        line = nl;
    }

    G_FLOAT(OFS_RETURN) = true;
}

static void PF_buf_writefile()
{
    size_t fnum = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
    size_t bufno = G_FLOAT(OFS_PARM1) - BUFSTRBASE;
    char** strings;
    unsigned int idx, midx;

    G_FLOAT(OFS_RETURN) = 0;

    if((unsigned int)bufno >= NUMSTRINGBUFS)
    {
        return;
    }
    if(!strbuflist[bufno].owningvm)
    {
        return;
    }

    if(fnum >= qcfiles_max)
    {
        return;
    }
    if(!qcfiles[fnum].file)
    {
        return;
    }

    if(qcvm->argc >= 3)
    {
        if(G_FLOAT(OFS_PARM2) <= 0)
        {
            idx = 0;
        }
        else
        {
            idx = G_FLOAT(OFS_PARM2);
        }
    }
    else
    {
        idx = 0;
    }
    if(qcvm->argc >= 4)
    {
        midx = idx + G_FLOAT(OFS_PARM3);
    }
    else
    {
        midx = strbuflist[bufno].used - idx;
    }
    if(idx > strbuflist[bufno].used)
    {
        idx = strbuflist[bufno].used;
    }
    if(midx > strbuflist[bufno].used)
    {
        midx = strbuflist[bufno].used;
    }
    for(strings = strbuflist[bufno].strings; idx < midx; idx++)
    {
        if(strings[idx])
        {
            fprintf(qcfiles[fnum].file, "%s\n", strings[idx]);
        }
    }
    G_FLOAT(OFS_RETURN) = 1;
}

// entity stuff
static void PF_WasFreed()
{
    edict_t* ed = G_EDICT(OFS_PARM0);
    G_FLOAT(OFS_RETURN) = ed->free;
}
static void PF_copyentity()
{
    edict_t* src = G_EDICT(OFS_PARM0);
    edict_t* dst = G_EDICT(OFS_PARM1);
    if(src->free || dst->free)
    {
        Con_Printf("PF_copyentity: entity is free\n");
    }
    memcpy(&dst->v, &src->v, qcvm->edict_size - sizeof(edict_t));
    dst->alpha = src->alpha;
    dst->sendinterval = src->sendinterval;
    SV_LinkEdict(dst, false);
}
static void PF_edict_for_num()
{
    G_INT(OFS_RETURN) = EDICT_TO_PROG(EDICT_NUM(G_FLOAT(OFS_PARM0)));
}
static void PF_num_for_edict()
{
    G_FLOAT(OFS_RETURN) = G_EDICTNUM(OFS_PARM0);
}
static void PF_findchain()
{
    edict_t *ent, *chain;
    int i, f;
    const char *s, *t;

    chain = (edict_t*)qcvm->edicts;

    f = G_INT(OFS_PARM0);
    s = G_STRING(OFS_PARM1);
    // FIXME: cfld = G_INT(OFS_PARM2);

    ent = NEXT_EDICT(qcvm->edicts);
    for(i = 1; i < qcvm->num_edicts; i++, ent = NEXT_EDICT(ent))
    {
        if(ent->free)
        {
            continue;
        }
        t = E_STRING(ent, f);
        if(strcmp(s, t))
        {
            continue;
        }
        ent->v.chain = EDICT_TO_PROG(chain);
        chain = ent;
    }

    RETURN_EDICT(chain);
}
static void PF_findfloat()
{
    int e;
    int f;
    float s, t;
    edict_t* ed;

    e = G_EDICTNUM(OFS_PARM0);
    f = G_INT(OFS_PARM1);
    s = G_FLOAT(OFS_PARM2);

    for(e++; e < qcvm->num_edicts; e++)
    {
        ed = EDICT_NUM(e);
        if(ed->free)
        {
            continue;
        }
        t = E_FLOAT(ed, f);
        if(t == s)
        {
            RETURN_EDICT(ed);
            return;
        }
    }

    RETURN_EDICT(qcvm->edicts);
}
static void PF_findchainfloat()
{
    edict_t *ent, *chain;
    int i, f;
    float s, t;

    chain = (edict_t*)qcvm->edicts;

    f = G_INT(OFS_PARM0);
    s = G_FLOAT(OFS_PARM1);
    // FIXME: cfld = G_INT(OFS_PARM2);

    ent = NEXT_EDICT(qcvm->edicts);
    for(i = 1; i < qcvm->num_edicts; i++, ent = NEXT_EDICT(ent))
    {
        if(ent->free)
        {
            continue;
        }
        t = E_FLOAT(ent, f);
        if(s != t)
        {
            continue;
        }
        ent->v.chain = EDICT_TO_PROG(chain);
        chain = ent;
    }

    RETURN_EDICT(chain);
}
static void PF_findflags()
{
    int e;
    int f;
    int s, t;
    edict_t* ed;

    e = G_EDICTNUM(OFS_PARM0);
    f = G_INT(OFS_PARM1);
    s = G_FLOAT(OFS_PARM2);

    for(e++; e < qcvm->num_edicts; e++)
    {
        ed = EDICT_NUM(e);
        if(ed->free)
        {
            continue;
        }
        t = E_FLOAT(ed, f);
        if(t & s)
        {
            RETURN_EDICT(ed);
            return;
        }
    }

    RETURN_EDICT(qcvm->edicts);
}
static void PF_findchainflags()
{
    edict_t *ent, *chain;
    int i, f;
    int s, t;

    chain = (edict_t*)qcvm->edicts;

    f = G_INT(OFS_PARM0);
    s = G_FLOAT(OFS_PARM1);
    // FIXME: cfld = G_INT(OFS_PARM2);

    ent = NEXT_EDICT(qcvm->edicts);
    for(i = 1; i < qcvm->num_edicts; i++, ent = NEXT_EDICT(ent))
    {
        if(ent->free)
        {
            continue;
        }
        t = E_FLOAT(ent, f);
        if(!(s & t))
        {
            continue;
        }
        ent->v.chain = EDICT_TO_PROG(chain);
        chain = ent;
    }

    RETURN_EDICT(chain);
}
static void PF_numentityfields()
{
    G_FLOAT(OFS_RETURN) = qcvm->progs->numfielddefs;
}
static void PF_findentityfield()
{
    ddef_t* fld = ED_FindField(G_STRING(OFS_PARM0));
    if(fld)
    {
        G_FLOAT(OFS_RETURN) = fld - qcvm->fielddefs;
    }
    else
    {
        G_FLOAT(OFS_RETURN) = 0; // the first field is meant to be some dummy
    }
    // placeholder. or it could be modelindex...
}
static void PF_entityfieldref()
{
    unsigned int fldidx = G_FLOAT(OFS_PARM0);
    if(fldidx >= (unsigned int)qcvm->progs->numfielddefs)
    {
        G_INT(OFS_RETURN) = 0;
    }
    else
    {
        G_INT(OFS_RETURN) = qcvm->fielddefs[fldidx].ofs;
    }
}
static void PF_entityfieldname()
{
    unsigned int fldidx = G_FLOAT(OFS_PARM0);
    if(fldidx < (unsigned int)qcvm->progs->numfielddefs)
    {
        G_INT(OFS_RETURN) = qcvm->fielddefs[fldidx].s_name;
    }
    else
    {
        G_INT(OFS_RETURN) = 0;
    }
}
static void PF_entityfieldtype()
{
    unsigned int fldidx = G_FLOAT(OFS_PARM0);
    if(fldidx >= (unsigned int)qcvm->progs->numfielddefs)
    {
        G_FLOAT(OFS_RETURN) = ev_void;
    }
    else
    {
        G_FLOAT(OFS_RETURN) = qcvm->fielddefs[fldidx].type;
    }
}
static void PF_getentfldstr()
{
    unsigned int fldidx = G_FLOAT(OFS_PARM0);
    edict_t* ent = G_EDICT(OFS_PARM1);
    if(fldidx < (unsigned int)qcvm->progs->numfielddefs)
    {
        char* ret = PR_GetTempString();
        const char* val = PR_UglyValueString(qcvm->fielddefs[fldidx].type,
            (eval_t*)((float*)&ent->v + qcvm->fielddefs[fldidx].ofs));
        q_strlcpy(ret, val, STRINGTEMP_LENGTH);
        G_INT(OFS_RETURN) = PR_SetEngineString(ret);
    }
    else
    {
        G_INT(OFS_RETURN) = 0;
    }
}
static void PF_putentfldstr()
{
    unsigned int fldidx = G_FLOAT(OFS_PARM0);
    edict_t* ent = G_EDICT(OFS_PARM1);
    const char* value = G_STRING(OFS_PARM2);
    if(fldidx < (unsigned int)qcvm->progs->numfielddefs)
    {
        G_FLOAT(OFS_RETURN) =
            ED_ParseEpair((void*)&ent->v, qcvm->fielddefs + fldidx, value);
    }
    else
    {
        G_FLOAT(OFS_RETURN) = false;
    }
}
// static void PF_loadfromdata()
//{
// fixme;
//}
// static void PF_loadfromfile()
//{
// fixme;
//}

static void PF_parseentitydata()
{
    edict_t* ed = G_EDICT(OFS_PARM0);
    const char *data = G_STRING(OFS_PARM1), *end;
    unsigned int offset = (qcvm->argc > 2) ? G_FLOAT(OFS_PARM2) : 0;
    if(offset)
    {
        unsigned int len = strlen(data);
        if(offset > len)
        {
            offset = len;
        }
    }
    if(!data[offset])
    {
        G_FLOAT(OFS_RETURN) = 0;
    }
    else
    {
        end = ED_ParseEdict(data + offset, ed);
        G_FLOAT(OFS_RETURN) = end - data;
    }
}
static void PF_callfunction()
{
    dfunction_t* fnc;
    const char* fname;
    if(!qcvm->argc)
    {
        return;
    }
    qcvm->argc--;
    fname = G_STRING(OFS_PARM0 + qcvm->argc * 3);
    fnc = ED_FindFunction(fname);
    if(fnc && fnc->first_statement > 0)
    {
        PR_ExecuteProgram(fnc - qcvm->functions);
    }
}
static void PF_isfunction()
{
    const char* fname = G_STRING(OFS_PARM0);
    G_FLOAT(OFS_RETURN) = ED_FindFunction(fname) ? true : false;
}

// other stuff
static void PF_gettime()
{
    int timer = (qcvm->argc > 0) ? G_FLOAT(OFS_PARM0) : 0;
    switch(timer)
    {
        default: Con_DPrintf("PF_gettime: unsupported timer %i\n", timer);
        case 0: // cached time at start of frame
            G_FLOAT(OFS_RETURN) = realtime;
            break;
        case 1: // actual time
            G_FLOAT(OFS_RETURN) = Sys_DoubleTime();
            break;
            // case 2:	//highres.. looks like time into the frame. no idea
            // case 3:	//uptime
            // case 4:	//cd track
            // case 5:	//client simtime
    }
}
#define STRINGIFY2(x) #x
#define STRINGIFY(x) STRINGIFY2(x)
static void PF_infokey_internal(bool returnfloat)
{
    unsigned int ent = G_EDICTNUM(OFS_PARM0);
    const char* key = G_STRING(OFS_PARM1);
    const char* r;
    char buf[64];
    if(!ent)
    { // nq doesn't really do serverinfo. it just has some cvars.
        if(!strcmp(key, "*version"))
        {
            q_snprintf(buf, sizeof(buf), ENGINE_NAME_AND_VER);
            r = buf;
        }
        else
        {
            cvar_t* var = Cvar_FindVar(key);
            if(var && (var->flags & CVAR_SERVERINFO))
            {
                r = var->string;
            }
            else
            {
                r = nullptr;
            }
        }
    }
    else if(ent <= (unsigned int)svs.maxclients && svs.clients[ent - 1].active)
    {
        ent--;
        r = buf;
        if(!strcmp(key, "ip"))
        {
            r = NET_QSocketGetTrueAddressString(svs.clients[ent].netconnection);
        }
        else if(!strcmp(key, "ping"))
        {
            float total = 0;
            unsigned int j;
            for(j = 0; j < NUM_PING_TIMES; j++)
            {
                total += svs.clients[ent].ping_times[j];
            }
            total /= NUM_PING_TIMES;
            q_snprintf(buf, sizeof(buf), "%f", total);
        }
        else if(!strcmp(key, "protocol"))
        {
            switch(sv.protocol)
            {
                case PROTOCOL_QUAKEVR: r = "quakevr"; break;
                case PROTOCOL_NETQUAKE: r = "quake"; break;
                case PROTOCOL_FITZQUAKE: r = "fitz666"; break;
                case PROTOCOL_RMQ: r = "rmq999"; break;
                default: r = ""; break;
            }
        }
        else if(!strcmp(key, "name"))
        {
            r = svs.clients[ent].name;
        }
        else if(!strcmp(key, "topcolor"))
        {
            q_snprintf(buf, sizeof(buf), "%u", svs.clients[ent].colors >> 4);
        }
        else if(!strcmp(key, "bottomcolor"))
        {
            q_snprintf(buf, sizeof(buf), "%u", svs.clients[ent].colors & 15);
        }
        else if(!strcmp(key, "team"))
        { // nq doesn't really do teams. qw does
            // though. yay compat?
            q_snprintf(
                buf, sizeof(buf), "t%u", (svs.clients[ent].colors & 15) + 1);
        }
        else if(!strcmp(key, "*VIP"))
        {
            r = "";
        }
        else if(!strcmp(key, "*spectator"))
        {
            r = "";
        }
        else if(!strcmp(key, "skin"))
        {
            r = "";
        }
        else if(!strcmp(key, "csqcactive"))
        {
            r = "";
        }
        else if(!strcmp(key, "rate"))
        {
            r = "0";
        }
        else
        {
            r = nullptr;
        }
    }
    else
    {
        r = nullptr;
    }

    if(returnfloat)
    {
        if(r)
        {
            G_FLOAT(OFS_RETURN) = atof(r);
        }
        else
        {
            G_FLOAT(OFS_RETURN) = 0;
        }
    }
    else
    {
        if(r)
        {
            char* temp = PR_GetTempString();
            q_strlcpy(temp, r, STRINGTEMP_LENGTH);
            G_INT(OFS_RETURN) = PR_SetEngineString(temp);
        }
        else
        {
            G_INT(OFS_RETURN) = 0;
        }
    }
}
static void PF_infokey_s()
{
    PF_infokey_internal(false);
}
static void PF_infokey_f()
{
    PF_infokey_internal(true);
}

static void PF_multicast_internal(
    bool reliable, byte* pvs, unsigned int requireext2)
{
    unsigned int i;
    int cluster;
    mleaf_t* playerleaf;
    if(!pvs)
    {
        if(!requireext2)
        {
            SZ_Write((reliable ? &sv.reliable_datagram : &sv.datagram),
                sv.multicast.data, sv.multicast.cursize);
        }
        else
        {
            for(i = 0; i < (unsigned int)svs.maxclients; i++)
            {
                if(!svs.clients[i].active)
                {
                    continue;
                }

                if(!(svs.clients[i].protocol_pext2 & requireext2))
                {
                    continue;
                }

                SZ_Write((reliable ? &svs.clients[i].message
                                   : &svs.clients[i].datagram),
                    sv.multicast.data, sv.multicast.cursize);
            }
        }
    }
    else
    {
        for(i = 0; i < (unsigned int)svs.maxclients; i++)
        {
            if(!svs.clients[i].active)
            {
                continue;
            }

            if(requireext2 && !(svs.clients[i].protocol_pext2 & requireext2))
            {
                continue;
            }

            // figure out which cluster (read: pvs index) to use.
            playerleaf = Mod_PointInLeaf(
                svs.clients[i].edict->v.origin, qcvm->worldmodel);
            cluster = playerleaf - qcvm->worldmodel->leafs;
            cluster--; // pvs is 1-based, leaf 0 is discarded.
            if(cluster < 0 || (pvs[cluster >> 3] & (1 << (cluster & 7))))
            {
                // they can see it. add it in to whichever buffer is
                // appropriate.
                if(reliable)
                    SZ_Write(&svs.clients[i].message, sv.multicast.data,
                        sv.multicast.cursize);
                else
                    SZ_Write(&svs.clients[i].datagram, sv.multicast.data,
                        sv.multicast.cursize);
            }
        }
    }
}
// FIXME: shouldn't really be using pext2, but we don't track the earlier
// extensions, and it should be safe enough.
static void SV_Multicast(
    multicast_t to, const qvec3& org, int msg_entity, unsigned int requireext2)
{
    unsigned int i;

    if(to == MULTICAST_INIT && sv.state != ss_loading)
    {
        SZ_Write(&sv.signon, sv.multicast.data, sv.multicast.cursize);
        to = MULTICAST_ALL_R; // and send to players that are already on
    }

    switch(to)
    {
        case MULTICAST_INIT:
            SZ_Write(&sv.signon, sv.multicast.data, sv.multicast.cursize);
            break;
        case MULTICAST_ALL_R:
        case MULTICAST_ALL_U:
            PF_multicast_internal(to == MULTICAST_PHS_R, nullptr, requireext2);
            break;
        case MULTICAST_PHS_R:
        case MULTICAST_PHS_U:
            PF_multicast_internal(to == MULTICAST_PHS_R,
                nullptr /*Mod_LeafPHS(Mod_PointInLeaf(org, qcvm->worldmodel),
                        qcvm->worldmodel)*/
                ,
                requireext2); // we don't support phs, that would require lots
                              // of pvs decompression+merging stuff, and many
                              // q1bsps have a LOT of leafs.
            break;
        case MULTICAST_PVS_R:
        case MULTICAST_PVS_U:
            PF_multicast_internal(to == MULTICAST_PVS_R,
                Mod_LeafPVS(
                    Mod_PointInLeaf(org, qcvm->worldmodel), qcvm->worldmodel),
                requireext2);
            break;
        case MULTICAST_ONE_R:
        case MULTICAST_ONE_U:
            i = msg_entity - 1;
            if(i >= (unsigned int)svs.maxclients)
            {
                break;
            }
            // a unicast, which ignores pvs.
            //(unlike vanilla this allows unicast unreliables, so woo)
            if(svs.clients[i].active)
            {
                SZ_Write(((to == MULTICAST_ONE_R) ? &svs.clients[i].message
                                                  : &svs.clients[i].datagram),
                    sv.multicast.data, sv.multicast.cursize);
            }
            break;
        default: break;
    }
    SZ_Clear(&sv.multicast);
}
static void PF_multicast()
{
    const qvec3 org = extractVector(OFS_PARM0);
    multicast_t to = (multicast_t)G_FLOAT(OFS_PARM1);
    SV_Multicast(
        to, org, NUM_FOR_EDICT(PROG_TO_EDICT(pr_global_struct->msg_entity)), 0);
}
static void PF_randomvector()
{
    qvec3 temp;

    do
    {
        temp[0] = (rand() & 32767) * (2.0 / 32767.0) - 1.0;
        temp[1] = (rand() & 32767) * (2.0 / 32767.0) - 1.0;
        temp[2] = (rand() & 32767) * (2.0 / 32767.0) - 1.0;
    } while(DotProduct(temp, temp) >= 1);

    returnVector(temp);
}
static void PF_checkextension();
static void PF_checkbuiltin();
static void PF_builtinsupported();

static void PF_uri_escape()
{
    static const char* hex = "0123456789ABCDEF";

    char* result = PR_GetTempString();
    char* o = result;
    const unsigned char* s = (const unsigned char*)G_STRING(OFS_PARM0);
    *result = 0;
    while(*s && o < result + STRINGTEMP_LENGTH - 4)
    {
        if((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
            (*s >= '0' && *s <= '9') || *s == '.' || *s == '-' || *s == '_')
        {
            *o++ = *s++;
        }
        else
        {
            *o++ = '%';
            *o++ = hex[*s >> 4];
            *o++ = hex[*s & 0xf];
            s++;
        }
    }
    *o = 0;
    G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_uri_unescape()
{
    const char *s = G_STRING(OFS_PARM0), *i;
    char *resultbuf = PR_GetTempString(), *o;
    unsigned char hex;
    i = s;
    o = resultbuf;
    while(*i && o < resultbuf + STRINGTEMP_LENGTH - 2)
    {
        if(*i == '%')
        {
            hex = 0;
            if(i[1] >= 'A' && i[1] <= 'F')
            {
                hex += i[1] - 'A' + 10;
            }
            else if(i[1] >= 'a' && i[1] <= 'f')
            {
                hex += i[1] - 'a' + 10;
            }
            else if(i[1] >= '0' && i[1] <= '9')
            {
                hex += i[1] - '0';
            }
            else
            {
                *o++ = *i++;
                continue;
            }
            hex <<= 4;
            if(i[2] >= 'A' && i[2] <= 'F')
            {
                hex += i[2] - 'A' + 10;
            }
            else if(i[2] >= 'a' && i[2] <= 'f')
            {
                hex += i[2] - 'a' + 10;
            }
            else if(i[2] >= '0' && i[2] <= '9')
            {
                hex += i[2] - '0';
            }
            else
            {
                *o++ = *i++;
                continue;
            }
            *o++ = hex;
            i += 3;
        }
        else
        {
            *o++ = *i++;
        }
    }
    *o = 0;
    G_INT(OFS_RETURN) = PR_SetEngineString(resultbuf);
}
static void PF_crc16()
{
    bool insens = G_FLOAT(OFS_PARM0);
    const char* str = PF_VarString(1);
    size_t len = strlen(str);

    if(insens)
    {
        unsigned short crc;

        CRC_Init(&crc);
        while(len--)
        {
            CRC_ProcessByte(&crc, q_tolower(*str++));
        }
        G_FLOAT(OFS_RETURN) = crc;
    }
    else
    {
        G_FLOAT(OFS_RETURN) = CRC_Block((byte*)str, len);
    }
}

static void PF_strlennocol()
{
    int r = 0;
    struct markup_s mu;

    PR_Markup_Begin(&mu, G_STRING(OFS_PARM0), vec3_zero, 1);
    while(PR_Markup_Parse(&mu))
    {
        r++;
    }
    G_FLOAT(OFS_RETURN) = r;
}
static void PF_strdecolorize()
{
    int l, c;
    char* r = PR_GetTempString();
    struct markup_s mu;

    PR_Markup_Begin(&mu, G_STRING(OFS_PARM0), vec3_zero, 1);
    for(l = 0; l < STRINGTEMP_LENGTH - 1; l++)
    {
        c = PR_Markup_Parse(&mu);
        if(!c)
        {
            break;
        }
        r[l] = c;
    }
    r[l] = 0;

    G_INT(OFS_RETURN) = PR_SetEngineString(r);
}
static void PF_setattachment()
{
    edict_t* ent = G_EDICT(OFS_PARM0);
    edict_t* tagent = G_EDICT(OFS_PARM1);
    const char* tagname = G_STRING(OFS_PARM2);
    eval_t* val;

    if(*tagname)
    {
        // we don't support md3s, or any skeletal formats, so all tag names are
        // logically invalid for us.
        Con_DWarning("PF_setattachment: tag %s not found\n", tagname);
    }

    if((val = GetEdictFieldValue(ent, qcvm->extfields.tag_entity)))
    {
        val->edict = EDICT_TO_PROG(tagent);
    }
    if((val = GetEdictFieldValue(ent, qcvm->extfields.tag_index)))
    {
        val->_float = 0;
    }
}
static void PF_void_stub()
{
    G_FLOAT(OFS_RETURN) = 0;
}

static server_t::svcustomstat_s* PR_CustomStat(int idx, int type)
{
    size_t i;
    if(idx < 0 || idx >= MAX_CL_STATS)
    {
        return nullptr;
    }
    switch(type)
    {
        case ev_ext_integer:
        case ev_float:
        case ev_vector:
        case ev_entity: break;
        default: return nullptr;
    }

    for(i = 0; i < sv.numcustomstats; i++)
    {
        if(sv.customstats[i].idx == idx &&
            (sv.customstats[i].type == ev_string) == (type == ev_string))
        {
            break;
        }
    }
    if(i == sv.numcustomstats)
    {
        sv.numcustomstats++;
    }
    sv.customstats[i].idx = idx;
    sv.customstats[i].type = type;
    sv.customstats[i].fld = 0;
    sv.customstats[i].ptr = nullptr;
    return &sv.customstats[i];
}
static void PF_clientstat()
{
    int idx = G_FLOAT(OFS_PARM0);
    int type = G_FLOAT(OFS_PARM1);
    int fldofs = G_INT(OFS_PARM2);
    server_t::svcustomstat_s* stat = PR_CustomStat(idx, type);
    if(!stat)
    {
        return;
    }
    stat->fld = fldofs;
}
static void PF_globalstat()
{
    int idx = G_FLOAT(OFS_PARM0);
    int type = G_FLOAT(OFS_PARM1);
    const char* globname = G_STRING(OFS_PARM2);
    eval_t* ptr = (eval_t*)PR_FindExtGlobal(type, globname);
    server_t::svcustomstat_s* stat;
    if(ptr)
    {
        stat = PR_CustomStat(idx, type);
        if(!stat)
        {
            return;
        }
        stat->ptr = ptr;
    }
}
static void PF_pointerstat()
{
    int idx = G_FLOAT(OFS_PARM0);
    int type = G_FLOAT(OFS_PARM1);
    int qcptr = G_INT(OFS_PARM2);
    server_t::svcustomstat_s* stat;
    if(qcptr < 0 || qcptr >= qcvm->max_edicts * qcvm->edict_size ||
        (qcptr % qcvm->edict_size) < sizeof(edict_t) - sizeof(entvars_t))
    {
        return; // invalid pointer. this is a more strict check than the qcvm...
    }
    stat = PR_CustomStat(idx, type);
    if(!stat)
    {
        return;
    }
    stat->ptr = (eval_t*)((byte*)qcvm->edicts + qcptr);
}

static void PF_isbackbuffered()
{
    unsigned int plnum = G_EDICTNUM(OFS_PARM0) - 1;
    G_FLOAT(OFS_RETURN) = true; // assume the connection is clogged.
    if(plnum > (unsigned int)svs.maxclients)
    {
        return; // make error?
    }
    if(!svs.clients[plnum].active)
    {
        return; // empty slot
    }
    if(svs.clients[plnum].message.cursize > DATAGRAM_MTU)
    {
        return;
    }
    G_FLOAT(OFS_RETURN) = false; // okay to spam with more reliables.
}

#ifdef PSET_SCRIPT
int PF_SV_ForceParticlePrecache(const char* s)
{
    unsigned int i;
    for(i = 1; i < MAX_PARTICLETYPES; i++)
    {
        if(!sv.particle_precache[i])
        {
            if(sv.state != ss_loading)
            {
                MSG_WriteByte(&sv.multicast, svcdp_precache);
                MSG_WriteShort(&sv.multicast, i | 0x4000);
                MSG_WriteString(&sv.multicast, s);
                SV_Multicast(MULTICAST_ALL_R, nullptr, 0,
                    PEXT2_REPLACEMENTDELTAS); // FIXME
            }

            sv.particle_precache[i] = strcpy(Hunk_Alloc(strlen(s) + 1),
                s); // weirdness to avoid issues with tempstrings
            return i;
        }
        if(!strcmp(sv.particle_precache[i], s))
        {
            return i;
        }
    }
    return 0;
}
static void PF_sv_particleeffectnum()
{
    const char* s;
    unsigned int i;
    extern cvar_t r_particledesc;

    s = G_STRING(OFS_PARM0);
    G_FLOAT(OFS_RETURN) = 0;
    //	PR_CheckEmptyString (s);

    if(!*s)
    {
        return;
    }

    if(!sv.particle_precache[1] &&
        (!strncmp(s, "effectinfo.", 11) ||
            strstr(r_particledesc.string, "effectinfo")))
    {
        COM_Effectinfo_Enumerate(PF_SV_ForceParticlePrecache);
    }

    for(i = 1; i < MAX_PARTICLETYPES; i++)
    {
        if(!sv.particle_precache[i])
        {
            if(sv.state != ss_loading)
            {
                if(pr_ext_warned_particleeffectnum++ < 3)
                {
                    Con_Warning(
                        "PF_sv_particleeffectnum(%s): Precache should only be "
                        "done in spawn functions\n",
                        s);
                }

                MSG_WriteByte(&sv.multicast, svcdp_precache);
                MSG_WriteShort(&sv.multicast, i | 0x4000);
                MSG_WriteString(&sv.multicast, s);
                SV_Multicast(
                    MULTICAST_ALL_R, nullptr, 0, PEXT2_REPLACEMENTDELTAS);
            }

            sv.particle_precache[i] = strcpy(Hunk_Alloc(strlen(s) + 1),
                s); // weirdness to avoid issues with tempstrings
            G_FLOAT(OFS_RETURN) = i;
            return;
        }
        if(!strcmp(sv.particle_precache[i], s))
        {
            if(sv.state != ss_loading && !pr_checkextension.value)
            {
                if(pr_ext_warned_particleeffectnum++ < 3)
                {
                    Con_Warning(
                        "PF_sv_particleeffectnum(%s): Precache should only be "
                        "done in spawn functions\n",
                        s);
                }
            }
            G_FLOAT(OFS_RETURN) = i;
            return;
        }
    }
    PR_RunError("PF_sv_particleeffectnum: overflow");
}
static void PF_sv_trailparticles()
{
    int efnum;
    int ednum;
    float* start = G_VECTOR(OFS_PARM2);
    float* end = G_VECTOR(OFS_PARM3);

    /*DP gets this wrong, lets try to be compatible*/
    if((unsigned int)G_INT(OFS_PARM1) >=
        MAX_EDICTS * (unsigned int)qcvm->edict_size)
    {
        ednum = G_EDICTNUM(OFS_PARM0);
        efnum = G_FLOAT(OFS_PARM1);
    }
    else
    {
        efnum = G_FLOAT(OFS_PARM0);
        ednum = G_EDICTNUM(OFS_PARM1);
    }

    if(efnum <= 0)
    {
        return;
    }

    MSG_WriteByte(&sv.multicast, svcdp_trailparticles);
    MSG_WriteShort(&sv.multicast, ednum);
    MSG_WriteShort(&sv.multicast, efnum);
    MSG_WriteCoord(&sv.multicast, start[0], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, start[1], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, start[2], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, end[0], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, end[1], sv.protocolflags);
    MSG_WriteCoord(&sv.multicast, end[2], sv.protocolflags);

    SV_Multicast(MULTICAST_PHS_U, start, 0, PEXT2_REPLACEMENTDELTAS);
}
static void PF_sv_pointparticles()
{
    int efnum = G_FLOAT(OFS_PARM0);
    float* org = G_VECTOR(OFS_PARM1);
    float* vel = (qcvm->argc < 3) ? vec3_zero : G_VECTOR(OFS_PARM2);
    int count = (qcvm->argc < 4) ? 1 : G_FLOAT(OFS_PARM3);

    if(efnum <= 0)
    {
        return;
    }
    if(count > 65535)
    {
        count = 65535;
    }
    if(count < 1)
    {
        return;
    }

    if(count == 1 && !vel[0] && !vel[1] && !vel[2])
    {
        MSG_WriteByte(&sv.multicast, svcdp_pointparticles1);
        MSG_WriteShort(&sv.multicast, efnum);
        MSG_WriteCoord(&sv.multicast, org[0], sv.protocolflags);
        MSG_WriteCoord(&sv.multicast, org[1], sv.protocolflags);
        MSG_WriteCoord(&sv.multicast, org[2], sv.protocolflags);
    }
    else
    {
        MSG_WriteByte(&sv.multicast, svcdp_pointparticles);
        MSG_WriteShort(&sv.multicast, efnum);
        MSG_WriteCoord(&sv.multicast, org[0], sv.protocolflags);
        MSG_WriteCoord(&sv.multicast, org[1], sv.protocolflags);
        MSG_WriteCoord(&sv.multicast, org[2], sv.protocolflags);
        MSG_WriteCoord(&sv.multicast, vel[0], sv.protocolflags);
        MSG_WriteCoord(&sv.multicast, vel[1], sv.protocolflags);
        MSG_WriteCoord(&sv.multicast, vel[2], sv.protocolflags);
        MSG_WriteShort(&sv.multicast, count);
    }

    SV_Multicast(MULTICAST_PVS_U, org, 0, PEXT2_REPLACEMENTDELTAS);
}

int PF_CL_ForceParticlePrecache(const char* s)
{
    int i;

    // check if an ssqc one already exists with that name
    for(i = 1; i < MAX_PARTICLETYPES; i++)
    {
        if(!cl.particle_precache[i].name)
        {
            break; // nope, no more known
        }
        if(!strcmp(cl.particle_precache[i].name, s))
        {
            return i;
        }
    }

    // nope, check for a csqc one, and allocate if needed
    for(i = 1; i < MAX_PARTICLETYPES; i++)
    {
        if(!cl.local_particle_precache[i].name)
        {
            cl.local_particle_precache[i].name =
                strcpy(Hunk_Alloc(strlen(s) + 1),
                    s); // weirdness to avoid issues with tempstrings
            cl.local_particle_precache[i].index =
                PScript_FindParticleType(cl.local_particle_precache[i].name);
            return -i;
        }
        if(!strcmp(cl.local_particle_precache[i].name, s))
        {
            return -i;
        }
    }

    // err... too many. bum.
    return 0;
}
int PF_CL_GetParticle(int idx)
{ // negatives are csqc-originated particles, positives are ssqc-originated, for
  // consistency allowing networking of particles as identifiers
    if(!idx)
    {
        return P_INVALID;
    }
    if(idx < 0)
    {
        idx = -idx;
        if(idx >= MAX_PARTICLETYPES)
        {
            return P_INVALID;
        }
        return cl.local_particle_precache[idx].index;
    }
    else
    {
        if(idx >= MAX_PARTICLETYPES)
        {
            return P_INVALID;
        }
        return cl.particle_precache[idx].index;
    }
}

static void PF_cl_particleeffectnum()
{
    const char* s;

    s = G_STRING(OFS_PARM0);
    G_FLOAT(OFS_RETURN) = 0;
    //	PR_CheckEmptyString (s);

    if(!*s)
    {
        return;
    }

    G_FLOAT(OFS_RETURN) = PF_CL_ForceParticlePrecache(s);
    if(!G_FLOAT(OFS_RETURN))
    {
        PR_RunError("PF_cl_particleeffectnum: overflow");
    }
}
static void PF_cl_trailparticles()
{
    int efnum;
    edict_t* ent;
    float* start = G_VECTOR(OFS_PARM2);
    float* end = G_VECTOR(OFS_PARM3);

    if((unsigned int)G_INT(OFS_PARM1) >=
        MAX_EDICTS * (unsigned int)qcvm->edict_size)
    { /*DP gets this wrong, lets try to be compatible*/
        ent = G_EDICT(OFS_PARM0);
        efnum = G_FLOAT(OFS_PARM1);
    }
    else
    {
        efnum = G_FLOAT(OFS_PARM0);
        ent = G_EDICT(OFS_PARM1);
    }

    if(efnum <= 0)
    {
        return;
    }
    efnum = PF_CL_GetParticle(efnum);
    PScript_ParticleTrail(start, end, efnum, host_frametime,
        -NUM_FOR_EDICT(ent), nullptr, nullptr /*&ent->trailstate*/);
}
static void PF_cl_pointparticles()
{
    int efnum = G_FLOAT(OFS_PARM0);
    float* org = G_VECTOR(OFS_PARM1);
    float* vel = (qcvm->argc < 3) ? vec3_zero : G_VECTOR(OFS_PARM2);
    int count = (qcvm->argc < 4) ? 1 : G_FLOAT(OFS_PARM3);

    if(efnum <= 0)
    {
        return;
    }
    if(count < 1)
    {
        return;
    }
    efnum = PF_CL_GetParticle(efnum);
    PScript_RunParticleEffectState(org, vel, count, efnum, nullptr);
}
#else
#define PF_sv_particleeffectnum PF_void_stub
#define PF_sv_trailparticles PF_void_stub
#define PF_sv_pointparticles PF_void_stub
#define PF_cl_particleeffectnum PF_void_stub
#define PF_cl_trailparticles PF_void_stub
#define PF_cl_pointparticles PF_void_stub
#endif


static void PF_cl_getstat_int()
{
    int stnum = G_FLOAT(OFS_PARM0);
    if(stnum < 0 || stnum > countof(cl.stats))
    {
        G_INT(OFS_RETURN) = 0;
    }
    else
    {
        G_INT(OFS_RETURN) = cl.stats[stnum];
    }
}
static void PF_cl_getstat_float()
{
    int stnum = G_FLOAT(OFS_PARM0);
    if(stnum < 0 || stnum > countof(cl.stats))
    {
        G_FLOAT(OFS_RETURN) = 0;
    }
    else if(qcvm->argc > 1)
    {
        int firstbit = G_FLOAT(OFS_PARM1);
        int bitcount = G_FLOAT(OFS_PARM2);
        G_FLOAT(OFS_RETURN) =
            (cl.stats[stnum] >> firstbit) & ((1 << bitcount) - 1);
    }
    else
    {
        G_FLOAT(OFS_RETURN) = cl.statsf[stnum];
    }
}

struct qcpics_t
{
    char name[MAX_QPATH];
    int type;
    qpic_t* pic;
} * qcpics;

size_t numqcpics;
size_t maxqcpics;


void PR_ReloadPics(bool purge)
{
    numqcpics = 0;

    free(qcpics);
    qcpics = nullptr;
    maxqcpics = 0;
}
static qpic_t* DrawQC_CachePic(const char* picname, int cachetype)
{ // okay, so this is silly. we've ended up with 3 different cache levels.
  // qcpics, pics, and images.
    size_t i;
    for(i = 0; i < numqcpics; i++)
    { // binary search? something more sane?
        if(!strcmp(picname, qcpics[i].name))
        {
            if(qcpics[i].pic)
            {
                return qcpics[i].pic;
            }
            break;
        }
    }

    if(strlen(picname) >= MAX_QPATH)
    {
        return nullptr; // too long. get lost.
    }

    if(cachetype < 0)
    {
        return nullptr; // its a query, not actually needed.
    }

    if(i + 1 > maxqcpics)
    {
        maxqcpics = i + 32;
        qcpics = (qcpics_t*)realloc(qcpics, maxqcpics * sizeof(*qcpics));
    }

    strcpy(qcpics[i].name, picname);
    qcpics[i].type = cachetype;
    qcpics[i].pic = nullptr;

    // try to load it from a wad if applicable.
    // the extra gfx/ crap is because DP insists on it for wad images. and its a
    // nightmare to get things working in all engines if we don't accept that
    // quirk too.
    if(cachetype == 1)
    {
        qcpics[i].pic =
            Draw_PicFromWad(picname + (strncmp(picname, "gfx/", 4) ? 0 : 4));
    }
    else if(!strncmp(picname, "gfx/", 4) && !strchr(picname + 4, '.'))
    {
        qcpics[i].pic = Draw_PicFromWad(picname + 4);
    }

    // okay, not a wad pic, try and load a lmp/tga/etc
    if(!qcpics[i].pic)
    {
        qcpics[i].pic = Draw_TryCachePic(picname);
    }

    if(i == numqcpics)
    {
        numqcpics++;
    }

    return qcpics[i].pic;
}
static void DrawQC_CharacterQuad(float x, float y, int num, float w, float h)
{
    float size = 0.0625;
    float frow = (num >> 4) * size;
    float fcol = (num & 15) * size;
    size = 0.0624; // avoid rounding errors...

    glTexCoord2f(fcol, frow);
    glVertex2f(x, y);
    glTexCoord2f(fcol + size, frow);
    glVertex2f(x + w, y);
    glTexCoord2f(fcol + size, frow + size);
    glVertex2f(x + w, y + h);
    glTexCoord2f(fcol, frow + size);
    glVertex2f(x, y + h);
}
static void PF_cl_drawcharacter()
{
    extern gltexture_t* char_texture;

    float* pos = G_VECTOR(OFS_PARM0);
    int charcode = (int)G_FLOAT(OFS_PARM1) & 0xff;
    float* size = G_VECTOR(OFS_PARM2);
    float* rgb = G_VECTOR(OFS_PARM3);
    float alpha = G_FLOAT(OFS_PARM4);
    //	int flags	= G_FLOAT (OFS_PARM5);

    if(charcode == 32)
    {
        return; // don't waste time on spaces
    }

    GL_Bind(char_texture);
    glColor4f(rgb[0], rgb[1], rgb[2], alpha);
    glBegin(GL_QUADS);
    DrawQC_CharacterQuad(pos[0], pos[1], charcode, size[0], size[1]);
    glEnd();
}

static void PF_cl_drawrawstring()
{
    extern gltexture_t* char_texture;

    float* pos = G_VECTOR(OFS_PARM0);
    const char* text = G_STRING(OFS_PARM1);
    float* size = G_VECTOR(OFS_PARM2);
    float* rgb = G_VECTOR(OFS_PARM3);
    float alpha = G_FLOAT(OFS_PARM4);
    //	int flags	= G_FLOAT (OFS_PARM5);

    float x = pos[0];
    int c;

    if(!*text)
    {
        return; // don't waste time on spaces
    }

    GL_Bind(char_texture);
    glColor4f(rgb[0], rgb[1], rgb[2], alpha);
    glBegin(GL_QUADS);
    while((c = *text++))
    {
        DrawQC_CharacterQuad(x, pos[1], c, size[0], size[1]);
        x += size[0];
    }
    glEnd();
}
static void PF_cl_drawstring()
{
    extern gltexture_t* char_texture;

    float* pos = G_VECTOR(OFS_PARM0);
    const char* text = G_STRING(OFS_PARM1);
    float* size = G_VECTOR(OFS_PARM2);
    qvec3 rgb = extractVector(OFS_PARM3);
    float alpha = G_FLOAT(OFS_PARM4);
    //	int flags	= G_FLOAT (OFS_PARM5);

    float x = pos[0];
    struct markup_s mu;
    int c;

    if(!*text)
    {
        return; // don't waste time on spaces
    }

    PR_Markup_Begin(&mu, text, rgb, alpha);

    GL_Bind(char_texture);
    glBegin(GL_QUADS);
    while((c = PR_Markup_Parse(&mu)))
    {
        glColor4fv(toGlVec(mu.colour));
        DrawQC_CharacterQuad(x, pos[1], c, size[0], size[1]);
        x += size[0];
    }
    glEnd();
}
static void PF_cl_stringwidth()
{
    static const float defaultfontsize[] = {8, 8};
    const char* text = G_STRING(OFS_PARM0);
    bool usecolours = G_FLOAT(OFS_PARM1);
    const float* fontsize =
        (qcvm->argc > 2) ? G_VECTOR(OFS_PARM2) : defaultfontsize;
    struct markup_s mu;
    int r = 0;

    if(!usecolours)
    {
        r = strlen(text);
    }
    else
    {
        PR_Markup_Begin(&mu, text, vec3_zero, 1);
        while(PR_Markup_Parse(&mu))
        {
            r += 1;
        }
    }

    // primitive and lame, but hey.
    G_FLOAT(OFS_RETURN) = fontsize[0] * r;
}


static void PF_cl_drawsetclip()
{
    float s = CLAMP(1.0, scr_sbarscale.value, (float)glwidth / 320.0);

    float x = G_FLOAT(OFS_PARM0) * s;
    float y = G_FLOAT(OFS_PARM1) * s;
    float w = G_FLOAT(OFS_PARM2) * s;
    float h = G_FLOAT(OFS_PARM3) * s;

    glScissor(x, glheight - (y + h), w, h);
    glEnable(GL_SCISSOR_TEST);
}
static void PF_cl_drawresetclip()
{
    glDisable(GL_SCISSOR_TEST);
}

static void PF_cl_precachepic()
{
    const char* name = G_STRING(OFS_PARM0);
    int trywad = (qcvm->argc > 1) ? !!G_FLOAT(OFS_PARM1) : false;

    G_INT(OFS_RETURN) =
        G_INT(OFS_PARM0); // return input string, for convienience

    DrawQC_CachePic(name, trywad);
}
static void PF_cl_iscachedpic()
{
    const char* name = G_STRING(OFS_PARM0);
    if(DrawQC_CachePic(name, -1))
    {
        G_FLOAT(OFS_RETURN) = true;
    }
    else
    {
        G_FLOAT(OFS_RETURN) = false;
    }
}

static void PF_cl_drawpic()
{
    float* pos = G_VECTOR(OFS_PARM0);
    qpic_t* pic = DrawQC_CachePic(G_STRING(OFS_PARM1), false);
    float* size = G_VECTOR(OFS_PARM2);
    float* rgb = G_VECTOR(OFS_PARM3);
    float alpha = G_FLOAT(OFS_PARM4);
    //	int flags	= G_FLOAT (OFS_PARM5);

    if(pic)
    {
        glColor4f(rgb[0], rgb[1], rgb[2], alpha);
        Draw_SubPic(pos[0], pos[1], size[0], size[1], pic, 0, 0, 1, 1);
    }
}

static void PF_cl_getimagesize()
{
    qpic_t* pic = DrawQC_CachePic(G_STRING(OFS_PARM0), false);
    if(pic)
    {
        G_VECTORSET(OFS_RETURN, pic->width, pic->height, 0);
    }
    else
    {
        G_VECTORSET(OFS_RETURN, 0, 0, 0);
    }
}

static void PF_cl_drawsubpic()
{
    float* pos = G_VECTOR(OFS_PARM0);
    float* size = G_VECTOR(OFS_PARM1);
    qpic_t* pic = DrawQC_CachePic(G_STRING(OFS_PARM2), false);
    float* srcpos = G_VECTOR(OFS_PARM3);
    float* srcsize = G_VECTOR(OFS_PARM4);
    float* rgb = G_VECTOR(OFS_PARM5);
    float alpha = G_FLOAT(OFS_PARM6);
    //	int flags	= G_FLOAT (OFS_PARM7);

    if(pic)
    {
        glColor4f(rgb[0], rgb[1], rgb[2], alpha);
        Draw_SubPic(pos[0], pos[1], size[0], size[1], pic, srcpos[0], srcpos[1],
            srcsize[0], srcsize[1]);
    }
}

static void PF_cl_drawfill()
{
    float* pos = G_VECTOR(OFS_PARM0);
    float* size = G_VECTOR(OFS_PARM1);
    float* rgb = G_VECTOR(OFS_PARM2);
    float alpha = G_FLOAT(OFS_PARM3);
    //	int flags	= G_FLOAT (OFS_PARM4);

    glDisable(GL_TEXTURE_2D);

    glColor4f(rgb[0], rgb[1], rgb[2], alpha);

    glBegin(GL_QUADS);
    glVertex2f(pos[0], pos[1]);
    glVertex2f(pos[0] + size[0], pos[1]);
    glVertex2f(pos[0] + size[0], pos[1] + size[1]);
    glVertex2f(pos[0], pos[1] + size[1]);
    glEnd();

    glEnable(GL_TEXTURE_2D);
}


static qpic_t* polygon_pic;
#define MAX_POLYVERTS
polygonvert_t polygon_verts[256];
unsigned int polygon_numverts;
static void PF_R_PolygonBegin()
{
    qpic_t* pic = DrawQC_CachePic(G_STRING(OFS_PARM0), false);
    int flags = (qcvm->argc > 1) ? G_FLOAT(OFS_PARM1) : 0;
    int is2d = (qcvm->argc > 2) ? G_FLOAT(OFS_PARM2) : 0;

    if(!is2d)
    {
        PR_RunError("PF_R_PolygonBegin: scene polygons are not supported");
    }
    if(flags)
    {
        PR_RunError("PF_R_PolygonBegin: modifier flags are not supported");
    }

    polygon_pic = pic;
    polygon_numverts = 0;
}
static void PF_R_PolygonVertex()
{
    polygonvert_t* v = &polygon_verts[polygon_numverts];
    if(polygon_numverts == countof(polygon_verts))
    {
        return; // panic!
    }
    polygon_numverts++;

    v->xy[0] = G_FLOAT(OFS_PARM0 + 0);
    v->xy[1] = G_FLOAT(OFS_PARM0 + 1);
    v->st[0] = G_FLOAT(OFS_PARM1 + 0);
    v->st[1] = G_FLOAT(OFS_PARM1 + 1);
    v->rgba[0] = G_FLOAT(OFS_PARM2 + 0);
    v->rgba[1] = G_FLOAT(OFS_PARM2 + 1);
    v->rgba[2] = G_FLOAT(OFS_PARM2 + 2);
    v->rgba[3] = G_FLOAT(OFS_PARM3);
}
static void PF_R_PolygonEnd()
{
    if(polygon_pic)
    {
        Draw_PicPolygon(polygon_pic, polygon_numverts, polygon_verts);
    }
    polygon_numverts = 0;
}

static void PF_cl_cprint()
{
    const char* str = PF_VarString(0);
    SCR_CenterPrint(str);
}
static void PF_cl_keynumtostring()
{
    int keynum = Key_QCToNative(G_FLOAT(OFS_PARM0));
    char* s = PR_GetTempString();
    if(keynum < 0)
    {
        keynum = -1;
    }
    Q_strncpy(s, Key_KeynumToString(keynum), STRINGTEMP_LENGTH);
    G_INT(OFS_RETURN) = PR_SetEngineString(s);
}
static void PF_cl_stringtokeynum()
{
    const char* keyname = G_STRING(OFS_PARM0);
    G_FLOAT(OFS_RETURN) = Key_NativeToQC(Key_StringToKeynum(keyname));
}
static void PF_cl_getkeybind()
{
    int keynum = Key_QCToNative(G_FLOAT(OFS_PARM0));
    int bindmap = (qcvm->argc <= 1) ? 0 : G_FLOAT(OFS_PARM1);
    char* s = PR_GetTempString();
    if(bindmap < 0 || bindmap >= MAX_BINDMAPS)
    {
        bindmap = 0;
    }
    if(keynum >= 0 && keynum < MAX_KEYS && keybindings[bindmap][keynum])
    {
        Q_strncpy(s, keybindings[bindmap][keynum], STRINGTEMP_LENGTH);
    }
    else
    {
        Q_strncpy(s, "", STRINGTEMP_LENGTH);
    }
    G_INT(OFS_RETURN) = PR_SetEngineString(s);
}
static void PF_cl_setkeybind()
{
    int keynum = Key_QCToNative(G_FLOAT(OFS_PARM0));
    const char* binding = G_STRING(OFS_PARM1);
    int bindmap = (qcvm->argc <= 1) ? 0 : G_FLOAT(OFS_PARM2);
    if(bindmap < 0 || bindmap >= MAX_BINDMAPS)
    {
        bindmap = 0;
    }
    if(keynum >= 0 && keynum < MAX_KEYS)
    {
        Key_SetBinding(keynum, binding, bindmap);
    }
}
static void PF_cl_getbindmaps()
{
    G_FLOAT(OFS_RETURN + 0) = key_bindmap[0];
    G_FLOAT(OFS_RETURN + 1) = key_bindmap[1];
    G_FLOAT(OFS_RETURN + 2) = 0;
}
static void PF_cl_setbindmaps()
{
    float* bm = G_VECTOR(OFS_PARM0);
    key_bindmap[0] = bm[0];
    key_bindmap[1] = bm[1];

    if(key_bindmap[0] < 0 || key_bindmap[0] >= MAX_BINDMAPS)
    {
        key_bindmap[0] = 0;
    }
    if(key_bindmap[1] < 0 || key_bindmap[1] >= MAX_BINDMAPS)
    {
        key_bindmap[1] = 0;
    }

    G_FLOAT(OFS_RETURN) = false;
}
static void PF_cl_findkeysforcommand()
{
    const char* command = G_STRING(OFS_PARM0);
    int bindmap = G_FLOAT(OFS_PARM1);
    int keys[5];
    char gah[64];
    char* s = PR_GetTempString();
    int count = 0;
    int j;
    int l = strlen(command);
    const char* b;
    if(bindmap < 0 || bindmap >= MAX_BINDMAPS)
    {
        bindmap = 0;
    }

    for(j = 0; j < MAX_KEYS; j++)
    {
        b = keybindings[bindmap][j];
        if(!b)
        {
            continue;
        }
        if(!strncmp(b, command, l) &&
            (!b[l] || (!strchr(command, ' ') && (b[l] == ' ' || b[l] == '\t'))))
        {
            keys[count++] = Key_NativeToQC(j);
            if(count == countof(keys))
            {
                break;
            }
        }
    }

    while(count < 2)
    {
        keys[count++] = -1; // always return at least two keys. DP behaviour.
    }

    *s = 0;
    for(j = 0; j < count; j++)
    {
        if(*s)
        {
            q_strlcat(s, " ", STRINGTEMP_LENGTH);
        }

        // FIXME: This should be "'%i'", but our tokenize builtin doesn't
        // support DP's fuckage, so this tends to break everything. So I'm just
        // going to keep things sane by being less compatible-but-cleaner here.
        q_snprintf(gah, sizeof(gah), "%i", keys[j]);
        q_strlcat(s, gah, STRINGTEMP_LENGTH);
    }

    G_INT(OFS_RETURN) = PR_SetEngineString(s);
}
// this extended version returns actual key names. which modifiers can be
// returned.
static void PF_cl_findkeysforcommandex()
{
    const char* command = G_STRING(OFS_PARM0);
    int bindmap = G_FLOAT(OFS_PARM1);
    int keys[16];
    char* s = PR_GetTempString();
    int count = 0;
    int j;
    int l = strlen(command);
    const char* b;
    if(bindmap < 0 || bindmap >= MAX_BINDMAPS)
    {
        bindmap = 0;
    }

    for(j = 0; j < MAX_KEYS; j++)
    {
        b = keybindings[bindmap][j];
        if(!b)
        {
            continue;
        }
        if(!strncmp(b, command, l) &&
            (!b[l] || (!strchr(command, ' ') && (b[l] == ' ' || b[l] == '\t'))))
        {
            keys[count++] = j;
            if(count == countof(keys))
            {
                break;
            }
        }
    }

    *s = 0;
    for(j = 0; j < count; j++)
    {
        if(*s)
        {
            q_strlcat(s, " ", STRINGTEMP_LENGTH);
        }

        q_strlcat(s, Key_KeynumToString(keys[j]), STRINGTEMP_LENGTH);
    }

    G_INT(OFS_RETURN) = PR_SetEngineString(s);
}

static void PF_cl_setcursormode()
{
    bool absmode = G_FLOAT(OFS_PARM0);
    //	const char *cursorname = (qcvm->argc<=1)?"":G_STRING(OFS_PARM1);
    //	float *hotspot = (qcvm->argc<=2)?NULL:G_VECTOR(OFS_PARM2);
    //	float cursorscale = (qcvm->argc<=3)?1:G_FLOAT(OFS_PARM3);

    /*	if (absmode)
        {
            int mark = Hunk_LowMark();
            int width, height;
            bool malloced;
            byte *imagedata = Image_LoadImage(cursorname, &width, &height,
       &malloced);
            //TODO: rescale image by cursorscale
            SDL_Surface *surf =
       !imagedata?NULL:SDL_CreateRGBSurfaceFrom(imagedata, width, height, 32,
       width*4, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
            Hunk_FreeToLowMark(mark);
            if (malloced)
                free(imagedata);
            if (surf)
            {
                cursor = SDL_CreateColorCursor(surf, hotspot[0], hotspot[1]);
                SDL_FreeSurface(surf);
                SDL_SetCursor(cursor);
            }
            else
            {
                SDL_SetCursor(SDL_GetDefaultCursor());
                cursor = nullptr;
            }
            if (oldcursor)
                SDL_FreeCursor(oldcursor);
            oldcursor = cursor;
        }*/

    cl.csqc_cursorforced = absmode;
}
static void PF_cl_getcursormode()
{
    //	bool effectivemode = (qcvm->argc==0)?false:G_FLOAT(OFS_PARM0);

    //	if (effectivemode)
    //		G_FLOAT(OFS_RETURN) = cl.csqc_cursorforced;
    //	else
    G_FLOAT(OFS_RETURN) = cl.csqc_cursorforced;
}
static void PF_cl_setsensitivity()
{
    float sens = G_FLOAT(OFS_PARM0);

    cl.csqc_sensitivity = sens;
}
void PF_cl_playerkey_internal(int player, const char* key, bool retfloat)
{
    char buf[64];
    char* ret = buf;
    extern int fragsort[MAX_SCOREBOARD];
    extern int scoreboardlines;
    extern int Sbar_ColorForMap(int m);
    if(player < 0 && player >= -scoreboardlines)
    {
        player = fragsort[-1 - player];
    }
    if(player < 0 || player > MAX_SCOREBOARD)
    {
        ret = nullptr;
    }
    else if(!strcmp(key, "viewentity"))
    {
        q_snprintf(buf, sizeof(buf), "%i",
            player + 1); // hack for DP compat. always returned even when the
                         // slot is empty (so long as its valid).
    }
    else if(!*cl.scores[player].name)
    {
        ret = nullptr;
    }
    else if(!strcmp(key, "name"))
    {
        ret = cl.scores[player].name;
    }
    else if(!strcmp(key, "frags"))
    {
        q_snprintf(buf, sizeof(buf), "%i", cl.scores[player].frags);
    }
    else if(!strcmp(key, "ping"))
    {
        q_snprintf(buf, sizeof(buf), "%i", cl.scores[player].ping);
    }
    else if(!strcmp(key, "pl"))
    {
        ret = nullptr; // unknown
    }
    else if(!strcmp(key, "entertime"))
    {
        q_snprintf(buf, sizeof(buf), "%g", cl.scores[player].entertime);
    }
    else if(!strcmp(key, "topcolor_rgb"))
    {
        byte* pal = (byte*)d_8to24table +
                    4 * Sbar_ColorForMap((cl.scores[player].colors) &
                                         0xf0); // johnfitz -- use d_8to24table
                                                // instead of host_basepal
        q_snprintf(buf, sizeof(buf), "%g %g %g", pal[0] / 255.0, pal[1] / 255.0,
            pal[2] / 255.0);
    }
    else if(!strcmp(key, "bottomcolor_rgb"))
    {
        byte* pal = (byte*)d_8to24table +
                    4 * Sbar_ColorForMap((cl.scores[player].colors << 4) &
                                         0xf0); // johnfitz -- use d_8to24table
                                                // instead of host_basepal
        q_snprintf(buf, sizeof(buf), "%g %g %g", pal[0] / 255.0, pal[1] / 255.0,
            pal[2] / 255.0);
    }
    else if(!strcmp(key, "topcolor"))
    {
        q_snprintf(
            buf, sizeof(buf), "%i", (cl.scores[player].colors >> 4) & 0xf);
    }
    else if(!strcmp(key, "bottomcolor"))
    {
        q_snprintf(buf, sizeof(buf), "%i", cl.scores[player].colors & 0xf);
    }
    else if(!strcmp(key, "team"))
    { // quakeworld uses team infokeys to decide teams
        // (instead of colours). but NQ never did, so that's
        // fun. Lets allow mods to use either so that they can
        // favour QW and let the engine hide differences .
        q_snprintf(buf, sizeof(buf), "%i", cl.scores[player].colors & 0xf);
    }
    else if(!strcmp(key, "userid"))
    {
        ret = nullptr; // unknown
        //	else if (!strcmp(key, "vignored"))	//checks to see this player's
        // voicechat is ignored. 		q_snprintf(buf, sizeof(buf), "%i",
        //(int)cl.scores[player].vignored);
    }
    else if(!strcmp(key, "voipspeaking"))
    {
        q_snprintf(buf, sizeof(buf), "%i", S_Voip_Speaking(player));
    }
    else if(!strcmp(key, "voiploudness"))
    {
        if(player == cl.viewentity - 1)
        {
            q_snprintf(buf, sizeof(buf), "%i", S_Voip_Loudness(false));
        }
        else
        {
            ret = nullptr;
        }
    }

    else
    {
        ret = nullptr; // no idea.
    }

    if(retfloat)
    {
        G_FLOAT(OFS_RETURN) = ret ? atof(ret) : 0;
    }
    else
    {
        G_INT(OFS_RETURN) = ret ? PR_MakeTempString(ret) : 0;
    }
}
static void PF_cl_playerkey_s()
{
    int playernum = G_FLOAT(OFS_PARM0);
    const char* keyname = G_STRING(OFS_PARM1);
    PF_cl_playerkey_internal(playernum, keyname, false);
}
static void PF_cl_playerkey_f()
{
    int playernum = G_FLOAT(OFS_PARM0);
    const char* keyname = G_STRING(OFS_PARM1);
    PF_cl_playerkey_internal(playernum, keyname, true);
}
static void PF_cl_isdemo()
{
    G_FLOAT(OFS_RETURN) = !!cls.demoplayback;
}
static void PF_cl_isserver()
{
    G_FLOAT(OFS_RETURN) = !!sv.active;
}
static void PF_cl_registercommand()
{
    const char* cmdname = G_STRING(OFS_PARM0);
    Cmd_AddCommand(cmdname, nullptr);
}
void PF_cl_serverkey_internal(const char* key, bool retfloat)
{
    const char* ret;
    if(!strcmp(key, "constate"))
    {
        if(cls.state != ca_connected)
        {
            ret = "disconnected";
        }
        else if(cls.signon == SIGNONS)
        {
            ret = "active";
        }
        else
        {
            ret = "connecting";
        }
    }
    else
    {
        // FIXME
        ret = "";
    }

    if(retfloat)
    {
        G_FLOAT(OFS_RETURN) = atof(ret);
    }
    else
    {
        G_INT(OFS_RETURN) = PR_SetEngineString(ret);
    }
}
static void PF_cl_serverkey_s()
{
    const char* keyname = G_STRING(OFS_PARM0);
    PF_cl_serverkey_internal(keyname, false);
}
static void PF_cl_serverkey_f()
{
    const char* keyname = G_STRING(OFS_PARM0);
    PF_cl_serverkey_internal(keyname, true);
}

static void PF_cl_readbyte()
{
    G_FLOAT(OFS_RETURN) = MSG_ReadByte();
}
static void PF_cl_readchar()
{
    G_FLOAT(OFS_RETURN) = MSG_ReadChar();
}
static void PF_cl_readshort()
{
    G_FLOAT(OFS_RETURN) = MSG_ReadShort();
}
static void PF_cl_readlong()
{
    G_FLOAT(OFS_RETURN) = MSG_ReadLong();
}
static void PF_cl_readcoord()
{
    G_FLOAT(OFS_RETURN) = MSG_ReadCoord(cl.protocolflags);
}
static void PF_cl_readangle()
{
    G_FLOAT(OFS_RETURN) = MSG_ReadAngle(cl.protocolflags);
}
static void PF_cl_readstring()
{
    G_INT(OFS_RETURN) = PR_MakeTempString(MSG_ReadString());
}
static void PF_cl_readfloat()
{
    G_FLOAT(OFS_RETURN) = MSG_ReadFloat();
}
static void PF_cl_readentitynum()
{
    G_FLOAT(OFS_RETURN) = MSG_ReadEntity(cl.protocol_pext2);
}
static void PF_cl_sendevent()
{
    const char* eventname = G_STRING(OFS_PARM0);
    const char* eventargs = G_STRING(OFS_PARM1);
    int a;

    MSG_WriteByte(&cls.message, clcfte_qcrequest);
    for(a = 2; a < 8 && *eventargs; a++, eventargs++)
    {
        switch(*eventargs)
        {
            case 's':
                MSG_WriteByte(&cls.message, ev_string);
                MSG_WriteString(&cls.message, G_STRING(OFS_PARM0 + a * 3));
                break;
            case 'f':
                MSG_WriteByte(&cls.message, ev_float);
                MSG_WriteFloat(&cls.message, G_FLOAT(OFS_PARM0 + a * 3));
                break;
            case 'i':
                MSG_WriteByte(&cls.message, ev_ext_integer);
                MSG_WriteLong(&cls.message, G_INT(OFS_PARM0 + a * 3));
                break;
            case 'v':
                MSG_WriteByte(&cls.message, ev_vector);
                MSG_WriteFloat(&cls.message, G_FLOAT(OFS_PARM0 + a * 3 + 0));
                MSG_WriteFloat(&cls.message, G_FLOAT(OFS_PARM0 + a * 3 + 1));
                MSG_WriteFloat(&cls.message, G_FLOAT(OFS_PARM0 + a * 3 + 2));
                break;
                //		case 'e':
                //			MSG_WriteByte(&cls.message, ev_entity);
                //			MSG_WriteEntity(&cls.message, ent->v.entnum);
                //			break;
        }
    }
    MSG_WriteByte(&cls.message, 0);
    MSG_WriteString(&cls.message, eventname);
}

static void PF_cl_setwindowcaption()
{
    VID_SetWindowCaption(G_STRING(OFS_PARM0));
}

// A quick note on number ranges.
// 0: automatically assigned. more complicated, but no conflicts over numbers,
// just names...
//   NOTE: #0 is potentially ambiguous - vanilla will interpret it as
//   instruction 0 (which is normally reserved) rather than a builtin.
//         if such functions were actually used, this would cause any 64bit
//         engines that switched to unsigned types to crash due to an underflow.
//         we do some sneaky hacks to avoid changes to the vm... because we're
//         evil.
// 0-199: free for all.
// 200-299: fte's random crap
// 300-399: csqc's random crap
// 400+: dp's random crap
static struct
{
    const char* name;
    builtin_t ssqcfunc;
    builtin_t csqcfunc;
    int documentednumber;
    const char* typestr;
    const char* desc;
    int number;
} extensionbuiltins[] =
#define PF_NoSSQC NULL
#define PF_NoCSQC NULL
#define PF_FullCSQCOnly NULL
    {
        {"vectoangles2", PF_ext_vectoangles, PF_ext_vectoangles, 51,
            D("vector(vector fwd, optional vector up)",
                "Returns the angles (+x=UP) required to orient an entity "
                "to look in the given direction. The 'up' argument is "
                "required if you wish to set a roll angle, otherwise it "
                "will be limited to just monster-style turning.")},

        {"sin", PF_Sin, PF_Sin, 60, "float(float angle)"},    // 60
        {"cos", PF_Cos, PF_Cos, 61, "float(float angle)"},    // 61
        {"sqrt", PF_Sqrt, PF_Sqrt, 62, "float(float value)"}, // 62
        {"tracetoss", PF_TraceToss, PF_TraceToss, 64,
            "void(entity ent, entity ignore)"},
        {"etos", PF_etos, PF_etos, 65, "string(entity ent)"},



        {"infokey", PF_infokey_s, PF_NoCSQC, 80,
            D("string(entity e, string key)",
                "If e is world, returns the field 'key' from either the "
                "serverinfo or the localinfo. If e is a player, returns "
                "the value of 'key' from the player's userinfo string. "
                "There are a few special exceptions, like 'ip' which is "
                "not technically part of the userinfo.")}, // 80
        {"infokeyf", PF_infokey_f, PF_NoCSQC, 0,
            D("float(entity e, string key)",
                "Identical to regular infokey, but returns it as a float "
                "instead of creating new tempstrings.")}, // 80
        {"stof", PF_stof, PF_stof, 81, "float(string)"},  // 81
        {"multicast", PF_multicast, PF_NoCSQC, 82,
            D("#define unicast(pl,reli) do{msg_entity = pl; multicast('0 0 "
              "0', reli?MULITCAST_ONE_R:MULTICAST_ONE);}while(0)\n"
              "void(vector where, float set)",
                "Once the MSG_MULTICAST network message buffer has been "
                "filled with data, this builtin is used to dispatch it to "
                "the given target, filtering by pvs for reduced network "
                "bandwidth.")}, // 82
        {"tracebox", PF_tracebox, PF_tracebox, 90,
            D("void(vector start, vector mins, vector maxs, vector end, "
              "float nomonsters, entity ent)",
                "Exactly like traceline, but a box instead of a uselessly "
                "thin point. Acceptable sizes are limited by bsp format, "
                "q1bsp has strict acceptable size values.")},
        {"randomvec", PF_randomvector, PF_randomvector, 91,
            D("vector()",
                "Returns a vector with random values. Each axis is "
                "independantly a value between -1 and 1 inclusive.")},
        {"getlight", PF_sv_getlight, PF_cl_getlight, 92,
            "vector(vector org)"}, // (DP_QC_GETLIGHT),
        {"registercvar", PF_registercvar, PF_registercvar, 93,
            D("float(string cvarname, string defaultvalue)",
                "Creates a new cvar on the fly. If it does not already "
                "exist, it will be given the specified value. If it does "
                "exist, this is a no-op.\nThis builtin has the limitation "
                "that it does not apply to configs or commandlines. Such "
                "configs will need to use the set or seta command causing "
                "this builtin to be a noop.\nIn engines that support it, "
                "you will generally find the autocvar feature easier and "
                "more efficient to use.")},
        {"min", PF_min, PF_min, 94,
            D("float(float a, float b, ...)",
                "Returns the lowest value of its arguments.")}, // (DP_QC_MINMAXBOUND)
        {"max", PF_max, PF_max, 95,
            D("float(float a, float b, ...)",
                "Returns the highest value of its arguments.")}, // (DP_QC_MINMAXBOUND)
        {"bound", PF_bound, PF_bound, 96,
            D("float(float minimum, float val, float maximum)",
                "Returns val, unless minimum is higher, or maximum is "
                "less.")}, // (DP_QC_MINMAXBOUND)
        {"pow", PF_pow, PF_pow, 97, "float(float value, float exp)"},
        {"findfloat", PF_findfloat, PF_findfloat, 98,
            D("#define findentity findfloat\nentity(entity start, "
              ".__variant fld, __variant match)",
                "Equivelent to the find builtin, but instead of comparing "
                "strings contents, this builtin compares the raw values. "
                "This builtin requires multiple calls in order to scan all "
                "entities - set start to the previous call's return "
                "value.\nworld is returned when there are no more "
                "entities.")}, // #98 (DP_QC_FINDFLOAT)
        {"checkextension", PF_checkextension, PF_checkextension, 99,
            D("float(string extname)",
                "Checks for an extension by its name (eg: "
                "checkextension(\"FRIK_FILE\") says that its okay to go "
                "ahead and use strcat).\nUse cvar(\"pr_checkextension\") "
                "to see if this builtin exists.")}, // #99	//darkplaces
                                                    // system - query a
                                                    // string to see if the
                                                    // mod supports X Y and
                                                    // Z.
        {"checkbuiltin", PF_checkbuiltin, PF_checkbuiltin, 0,
            D("float(__variant funcref)",
                "Checks to see if the specified builtin is "
                "supported/mapped. This is intended as a way to check for "
                "#0 functions, allowing for simple single-builtin "
                "functions.")},
        {"builtin_find", PF_builtinsupported, PF_builtinsupported, 100,
            D("float(string builtinname)",
                "Looks to see if the named builtin is valid, and returns "
                "the builtin number it exists at.")}, // #100	//per
                                                      // builtin system.
        {"anglemod", PF_anglemod, PF_anglemod, 102, "float(float value)"},

        {"fopen", PF_fopen, PF_fopen, 110,
            D("filestream(string filename, float mode, optional float "
              "mmapminsize)",
                "Opens a file, typically prefixed with \"data/\", for "
                "either read or write access.")}, // (FRIK_FILE)
        {"fclose", PF_fclose, PF_fclose, 111,
            "void(filestream fhandle)"}, // (FRIK_FILE)
        {"fgets", PF_fgets, PF_fgets, 112,
            D("string(filestream fhandle)",
                "Reads a single line out of the file. The new line "
                "character is not returned as part of the string. Returns "
                "the null string on EOF (use if not(string) to easily test "
                "for this, which distinguishes it from the empty string "
                "which is returned if the line being read is blank")}, // (FRIK_FILE)
        {"fputs", PF_fputs, PF_fputs, 113,
            D("void(filestream fhandle, string s, optional string s2, "
              "optional string s3, optional string s4, optional string s5, "
              "optional string s6, optional string s7)",
                "Writes the given string(s) into the file. For "
                "compatibility with fgets, you should ensure that the "
                "string is terminated with a \\n - this will not otherwise "
                "be done for you. It is up to the engine whether dos or "
                "unix line endings are actually written.")}, // (FRIK_FILE)
        //		{"fread",			PF_fread,			PF_fread, 0,
        // D("int(filestream fhandle, void *ptr, int size)", "Reads binary
        // data out of the file. Returns truncated lengths if the read
        // exceeds the length of the file.")},
        //		{"fwrite",			PF_fwrite,			PF_fwrite, 0,
        // D("int(filestream fhandle, void *ptr, int size)", "Writes binary
        // data out of the file.")},
        {"fseek", PF_fseek, PF_fseek, 0,
            D("#define ftell fseek //c-compat\nint(filestream fhandle, "
              "optional int newoffset)",
                "Changes the current position of the file, if specified. "
                "Returns prior position, in bytes.")},
        //		{"fsize",			PF_fsize,			PF_fsize, 0,
        // D("int(filestream fhandle, optional int newsize)", "Reports the
        // total size of the file, in bytes. Can also be used to
        // truncate/extend the file")},
        {"strlen", PF_strlen, PF_strlen, 114, "float(string s)"}, // (FRIK_FILE)
        {"strcat", PF_strcat, PF_strcat, 115,
            "string(string s1, optional string s2, optional string s3, "
            "optional string s4, optional string s5, optional string s6, "
            "optional string s7, optional string s8)"}, // (FRIK_FILE)
        {"substring", PF_substring, PF_substring, 116,
            "string(string s, float start, float length)"},  // (FRIK_FILE)
        {"stov", PF_stov, PF_stov, 117, "vector(string s)"}, // (FRIK_FILE)
        {"strzone", PF_strzone, PF_strzone, 118,
            D("string(string s, ...)",
                "Create a semi-permanent copy of a string that only "
                "becomes invalid once strunzone is called on the string "
                "(instead of when the engine assumes your string has left "
                "scope).")}, // (FRIK_FILE)
        {"strunzone", PF_strunzone, PF_strunzone, 119,
            D("void(string s)",
                "Destroys a string that was allocated by strunzone. "
                "Further references to the string MAY crash the game.")}, // (FRIK_FILE)

        //	{"getmodelindex",	PF_getmodelindex,	PF_getmodelindex,	200,
        // D("float(string modelname, optional float queryonly)", "Acts as an
        // alternative to precache_model(foo);setmodel(bar, foo); return
        // bar.modelindex;\nIf queryonly is set and the model was not
        // previously precached, the builtin will return 0 without needlessly
        // precaching the model.")},
        //	{"externcall",		PF_externcall,		PF_externcall,		201,
        // D("__variant(float prnum, string funcname, ...)", "Directly call a
        // function in a different/same progs by its name.\nprnum=0 is the
        //'default' or 'main' progs.\nprnum=-1 means current
        // progs.\nprnum=-2 will scan through the active progs and will use
        // the first it finds.")},
        //	{"addprogs",		PF_addprogs,		PF_addprogs,		202,
        // D("float(string progsname)", "Loads an additional .dat file into
        // the current qcvm. The returned handle can be used with any of the
        // externcall/externset/externvalue builtins.\nThere are cvars that
        // allow progs to be loaded automatically.")},
        //	{"externvalue",		PF_externvalue,		PF_externvalue,		203,
        // D("__variant(float prnum, string varname)", "Reads a global in the
        // named progs by the name of that global.\nprnum=0 is the 'default'
        // or 'main' progs.\nprnum=-1 means current progs.\nprnum=-2 will
        // scan through the active progs and will use the first it finds.")},
        //	{"externset",		PF_externset,		PF_externset,		204,
        // D("void(float prnum, __variant newval, string varname)", "Sets a
        // global in the named progs by name.\nprnum=0 is the 'default' or
        //'main' progs.\nprnum=-1 means current progs.\nprnum=-2 will scan
        // through the active progs and will use the first it finds.")},
        //	{"externrefcall",	PF_externrefcall,	PF_externrefcall,	205,
        // D("__variant(float prnum, void() func, ...)","Calls a function
        // between progs by its reference. No longer needed as direct
        // function calls now switch progs context automatically, and have
        // done for a long time. There is no remaining merit for this
        // function."), true},
        //	{"instr",			PF_instr,			PF_instr,			206,
        // D("float(string input, string token)", "Returns substring(input,
        // strstrpos(input, token), -1), or the null string if token was not
        // found in input. You're probably better off using strstrpos."),
        // true},
        //	{"openportal",		PF_OpenPortal,		PF_OpenPortal,		207,
        // D("void(entity portal, float state)", "Opens or closes the portals
        // associated with a door or some such on q2 or q3 maps. On Q2BSPs,
        // the entity should be the 'func_areaportal' entity - its style
        // field will say which portal to open. On Q3BSPs, the entity is the
        // door itself, the portal will be determined by the two areas found
        // from a preceding setorigin call.")},
        //	{"RegisterTempEnt", PF_RegisterTEnt,	PF_NoCSQC,			208,
        //"float(float attributes, string effectname, ...)"},
        //	{"CustomTempEnt",	PF_CustomTEnt,		PF_NoCSQC,			209,
        //"void(float type, vector pos, ...)"},
        //	{"fork",			PF_Fork,			PF_Fork,			210,
        // D("float(optional float sleeptime)", "When called, this builtin
        // simply returns. Twice.\nThe current 'thread' will return instantly
        // with a return value of 0. The new 'thread' will return after
        // sleeptime seconds with a return value of 1. See documentation for
        // the 'sleep' builtin for limitations/requirements concerning the
        // new thread. Note that QC should probably call abort in the new
        // thread, as otherwise the function will return to the calling qc
        // function twice also.")},
        //	{"abort",			PF_Abort,			PF_Abort,			211,
        // D("void(optional __variant ret)", "QC execution is aborted. Parent
        // QC functions on the stack will be skipped, effectively this forces
        // all QC functions to 'return ret' until execution returns to the
        // engine. If ret is ommited, it is assumed to be 0.")},
        //	{"sleep",			PF_Sleep,			PF_Sleep,			212,
        // D("void(float sleeptime)", "Suspends the current QC execution thread
        // for 'sleeptime' seconds.\nOther QC functions can and will be executed
        // in the interim, including changing globals and field state (but
        // not simultaneously).\nThe self and other globals will be restored
        // when the thread wakes up (or set to world if they were removed
        // since the thread started sleeping). Locals will be preserved, but
        // will not be protected from remove calls.\nIf the engine is
        // expecting the QC to return a value (even in the parent/root
        // function), the value 0 shall be used instead of waiting for the qc
        // to resume.")},
        //	{"forceinfokey",	PF_ForceInfoKey,	PF_NoCSQC,			213,
        // D("void(entity player, string key, string value)", "Directly
        // changes a user's info without pinging off the client. Also allows
        // explicitly setting * keys, including *spectator. Does not affect
        // the user's config or other servers.")},
        //	{"chat",			PF_chat,			PF_NoCSQC,			214,
        //"void(string filename, float starttag, entity edict)"},
        // //(FTE_NPCCHAT)
        //	{"particle2",		PF_sv_particle2,	PF_cl_particle2,	215,
        //"void(vector org, vector dmin, vector dmax, float colour, float
        // effect, float count)"},
        //	{"particle3",		PF_sv_particle3,	PF_cl_particle3,	216,
        //"void(vector org, vector box, float colour, float effect, float
        // count)"},
        //	{"particle4",		PF_sv_particle4,	PF_cl_particle4,	217,
        //"void(vector org, float radius, float colour, float effect, float
        // count)"},
        {"bitshift", PF_bitshift, PF_bitshift, 218,
            "float(float number, float quantity)"},
        {"te_lightningblood", PF_sv_te_lightningblood, nullptr, 219,
            "void(vector org)"},
        //	{"map_builtin",		PF_builtinsupported,PF_builtinsupported,220,
        // D("float(string builtinname, float builtinnum)","Attempts to map
        // the named builtin at a non-standard builtin number. Returns 0 on
        // failure."), true},	//like #100 - takes 2 args. arg0 is
        // builtinname, 1 is number to map to.
        {"strstrofs", PF_strstrofs, PF_strstrofs, 221,
            D("float(string s1, string sub, optional float startidx)",
                "Returns the 0-based offset of sub within the s1 string, "
                "or -1 if sub is not in s1.\nIf startidx is set, this "
                "builtin will ignore matches before that 0-based offset.")},
        {"str2chr", PF_str2chr, PF_str2chr, 222,
            D("float(string str, float index)",
                "Retrieves the character value at offset 'index'.")},
        {"chr2str", PF_chr2str, PF_chr2str, 223,
            D("string(float chr, ...)",
                "The input floats are considered character values, and are "
                "concatenated.")},
        {"strconv", PF_strconv, PF_strconv, 224,
            D("string(float ccase, float redalpha, float redchars, string "
              "str, ...)",
                "Converts quake chars in the input string amongst "
                "different representations.\nccase specifies the new case "
                "for letters.\n 0: not changed.\n 1: forced to lower "
                "case.\n 2: forced to upper case.\nredalpha and redchars "
                "switch between colour ranges.\n 0: no change.\n 1: Forced "
                "white.\n 2: Forced red.\n 3: Forced gold(low) (numbers "
                "only).\n 4: Forced gold (high) (numbers only).\n 5+6: "
                "Forced to white and red alternately.\nYou should not use "
                "this builtin in combination with UTF-8.")},
        {"strpad", PF_strpad, PF_strpad, 225,
            D("string(float pad, string str1, ...)",
                "Pads the string with spaces, to ensure its a specific "
                "length (so long as a fixed-width font is used, anyway). "
                "If pad is negative, the spaces are added on the left. If "
                "positive the padding is on the right.")}, // will be moved
        {"infoadd", PF_infoadd, PF_infoadd, 226,
            D("string(infostring old, string key, string value)",
                "Returns a new tempstring infostring with the named value "
                "changed (or added if it was previously unspecified). Key "
                "and value may not contain the \\ character.")},
        {"infoget", PF_infoget, PF_infoget, 227,
            D("string(infostring info, string key)",
                "Reads a named value from an infostring. The returned "
                "value is a tempstring")},
        //	{"strcmp",			PF_strncmp,			PF_strncmp,			228,
        // D("float(string s1, string s2)", "Compares the two strings
        // exactly. s1ofs allows you to treat s2 as a substring to compare
        // against, or should be 0.\nReturns 0 if the two strings are equal,
        // a negative value if s1 appears numerically lower, and positive if
        // s1 appears numerically higher.")},
        {"strncmp", PF_strncmp, PF_strncmp, 228,
            D("#define strcmp strncmp\nfloat(string s1, string s2, "
              "optional float len, optional float s1ofs, optional float "
              "s2ofs)",
                "Compares up to 'len' chars in the two strings. s1ofs "
                "allows you to treat s2 as a substring to compare against, "
                "or should be 0.\nReturns 0 if the two strings are equal, "
                "a negative value if s1 appears numerically lower, and "
                "positive if s1 appears numerically higher.")},
        {"strcasecmp", PF_strncasecmp, PF_strncasecmp, 229,
            D("float(string s1, string s2)",
                "Compares the two strings without case "
                "sensitivity.\nReturns 0 if they are equal. The sign of "
                "the return value may be significant, but should not be "
                "depended upon.")},
        {"strncasecmp", PF_strncasecmp, PF_strncasecmp, 230,
            D("float(string s1, string s2, float len, optional float "
              "s1ofs, optional float s2ofs)",
                "Compares up to 'len' chars in the two strings without "
                "case sensitivity. s1ofs allows you to treat s2 as a "
                "substring to compare against, or should be 0.\nReturns 0 "
                "if they are equal. The sign of the return value may be "
                "significant, but should not be depended upon.")},
        {"strtrim", PF_strtrim, PF_strtrim, 0,
            D("string(string s)",
                "Trims the whitespace from the start+end of the string.")},
        //	{"calltimeofday",	PF_calltimeofday,	PF_calltimeofday,	231,
        // D("void()", "Asks the engine to instantly call the qc's
        //'timeofday' function, before returning. For compatibility with
        // mvdsv.\ntimeofday should have the prototype: void(float secs,
        // float mins, float hour, float day, float mon, float year, string
        // strvalue)\nThe strftime builtin is more versatile and less
        // weird.")},
        {"clientstat", PF_clientstat, PF_NoCSQC, 232,
            D("void(float num, float type, .__variant fld)",
                "Specifies what data to use in order to send various "
                "stats, in a client-specific way.\n'num' should be a value "
                "between 32 and 127, other values are reserved.\n'type' "
                "must be set to one of the EV_* constants, one of "
                "EV_FLOAT, EV_STRING, EV_INTEGER, EV_ENTITY.\nfld must be "
                "a reference to the field used, each player will be sent "
                "only their own copy of these fields.")}, // EXT_CSQC
        {"globalstat", PF_globalstat, PF_NoCSQC, 233,
            D("void(float num, float type, string name)",
                "Specifies what data to use in order to send various "
                "stats, in a non-client-specific way. num and type are as "
                "in clientstat, name however, is the name of the global to "
                "read in the form of a string (pass \"foo\").")}, // EXT_CSQC_1
                                                                  // actually
        {"pointerstat", PF_pointerstat, PF_NoCSQC, 0,
            D("void(float num, float type, __variant *address)",
                "Specifies what data to use in order to send various "
                "stats, in a non-client-specific way. num and type are as "
                "in clientstat, address however, is the address of the "
                "variable you would like to use (pass &foo).")},
        {"isbackbuffered", PF_isbackbuffered, PF_NoCSQC, 234,
            D("float(entity player)",
                "Returns if the given player's network buffer will take "
                "multiple network frames in order to clear. If this "
                "builtin returns non-zero, you should delay or reduce the "
                "amount of reliable (and also unreliable) data that you "
                "are sending to that client.")},
        //	{"rotatevectorsbyangle",PF_rotatevectorsbyangles,PF_rotatevectorsbyangles,235,D("void(vector
        // angle)", "rotates the v_forward,v_right,v_up matrix by the
        // specified angles.")}, // #235
        //	{"rotatevectorsbyvectors",PF_rotatevectorsbymatrix,PF_rotatevectorsbymatrix,236,"void(vector
        // fwd, vector right, vector up)"}, // #236
        //	{"skinforname",		PF_skinforname,		PF_skinforname,		237,
        //"float(float mdlindex, string skinname)"},		// #237
        //	{"shaderforname",	PF_Fixme,			PF_Fixme,			238,
        // D("float(string shadername, optional string defaultshader, ...)",
        //"Caches the named shader and returns a handle to it.\nIf the
        // shader could not be loaded from disk (missing file or
        // ruleset_allow_shaders 0), it will be created from the
        //'defaultshader' string if specified, or a 'skin shader' default
        // will be used.\ndefaultshader if not empty should include the outer
        //{} that you would ordinarily find in a shader.")},
        {"te_bloodqw", PF_sv_te_bloodqw, nullptr, 239,
            "void(vector org, float count)"},
        //	{"te_muzzleflash",	PF_te_muzzleflash,	PF_clte_muzzleflash,0,
        //"void(entity ent)"},
        //	{"checkpvs",		PF_checkpvs,		PF_checkpvs,		240,
        //"float(vector viewpos, entity entity)"},
        //	{"matchclientname",	PF_matchclient,		PF_NoCSQC,			241,
        //"entity(string match, optional float matchnum)"},
        //	{"sendpacket",		PF_SendPacket,		PF_SendPacket,		242,
        //"void(string destaddress, string content)"},// (FTE_QC_SENDPACKET)
        //	{"rotatevectorsbytag",PF_Fixme,			PF_Fixme,			244,
        //"vector(entity ent, float tagnum)"},
        {"mod", PF_mod, PF_mod, 245, "float(float a, float n)"},
        {"stoi", PF_stoi, PF_stoi, 259,
            D("int(string)",
                "Converts the given string into a true integer. Base 8, "
                "10, or 16 is determined based upon the format of the "
                "string.")},
        {"itos", PF_itos, PF_itos, 260,
            D("string(int)",
                "Converts the passed true integer into a base10 string.")},
        {"stoh", PF_stoh, PF_stoh, 261,
            D("int(string)",
                "Reads a base-16 string (with or without 0x prefix) as an "
                "integer. Bugs out if given a base 8 or base 10 string. "
                ":P")},
        {"htos", PF_htos, PF_htos, 262,
            D("string(int)",
                "Formats an integer as a base16 string, with leading 0s "
                "and no prefix. Always returns 8 characters.")},
        {"ftoi", PF_ftoi, PF_ftoi, 0,
            D("int(float)",
                "Converts the given float into a true integer without "
                "depending on extended qcvm instructions.")},
        {"itof", PF_itof, PF_itof, 0,
            D("float(int)",
                "Converts the given true integer into a float without "
                "depending on extended qcvm instructions.")},
        //	{"skel_create",		PF_skel_create,		PF_skel_create,		263,
        // D("float(float modlindex, optional float useabstransforms)",
        //"Allocates a new uninitiaised skeletal object, with enough bone
        // info to animate the given model.\neg: self.skeletonobject =
        // skel_create(self.modelindex);")}, // (FTE_CSQC_SKELETONOBJECTS)
        //	{"skel_build",		PF_skel_build,		PF_skel_build,		264,
        // D("float(float skel, entity ent, float modelindex, float
        // retainfrac, float firstbone, float lastbone, optional float
        // addfrac)", "Animation data (according to the entity's frame info)
        // is pulled from the specified model and blended into the specified
        // skeletal object.\nIf retainfrac is set to 0 on the first call and
        // 1 on the others, you can blend multiple animations together
        // according to the addfrac value. The final weight should be 1.
        // Other values will result in scaling and/or other weirdness. You
        // can use firstbone and lastbone to update only part of the skeletal
        // object, to allow legs to animate separately from torso, use 0 for
        // both arguments to specify all, as bones are 1-based.")}, //
        //(FTE_CSQC_SKELETONOBJECTS)
        //	{"skel_get_numbones",PF_skel_get_numbones,PF_skel_get_numbones,265,
        // D("float(float skel)", "Retrives the number of bones in the model.
        // The valid range is 1<=bone<=numbones.")}, //
        //(FTE_CSQC_SKELETONOBJECTS)
        //	{"skel_get_bonename",PF_skel_get_bonename,PF_skel_get_bonename,266,
        // D("string(float skel, float bonenum)", "Retrieves the name of the
        // specified bone. Mostly only for debugging.")}, //
        //(FTE_CSQC_SKELETONOBJECTS) (returns tempstring)
        //	{"skel_get_boneparent",PF_skel_get_boneparent,PF_skel_get_boneparent,267,D("float(float
        // skel, float bonenum)", "Retrieves which bone this bone's position
        // is relative to. Bone 0 refers to the entity's position rather than
        // an actual bone")}, // (FTE_CSQC_SKELETONOBJECTS)
        //	{"skel_find_bone",	PF_skel_find_bone,	PF_skel_find_bone,	268,
        // D("float(float skel, string tagname)", "Finds a bone by its name,
        // from the model that was used to create the skeletal object.")}, //
        //(FTE_CSQC_SKELETONOBJECTS)
        //	{"skel_get_bonerel",PF_skel_get_bonerel,PF_skel_get_bonerel,269,
        // D("vector(float skel, float bonenum)", "Gets the bone position and
        // orientation relative to the bone's parent. Return value is the
        // offset, and v_forward, v_right, v_up contain the orientation.")},
        //// (FTE_CSQC_SKELETONOBJECTS) (sets v_forward etc)
        //	{"skel_get_boneabs",PF_skel_get_boneabs,PF_skel_get_boneabs,270,
        // D("vector(float skel, float bonenum)", "Gets the bone position and
        // orientation relative to the entity. Return value is the offset,
        // and v_forward, v_right, v_up contain the orientation.\nUse
        // gettaginfo for world coord+orientation.")}, //
        //(FTE_CSQC_SKELETONOBJECTS) (sets v_forward etc)
        //	{"skel_set_bone",	PF_skel_set_bone,	PF_skel_set_bone,	271,
        // D("void(float skel, float bonenum, vector org, optional vector
        // fwd, optional vector right, optional vector up)", "Sets a bone
        // position relative to its parent. If the orientation arguments are
        // not specified, v_forward+v_right+v_up are used instead.")}, //
        //(FTE_CSQC_SKELETONOBJECTS) (reads v_forward etc)
        //	{"skel_premul_bone",PF_skel_premul_bone,PF_skel_premul_bone,272,
        // D("void(float skel, float bonenum, vector org, optional vector
        // fwd, optional vector right, optional vector up)", "Transforms a
        // single bone by a matrix. You can use makevectors to generate a
        // rotation matrix from an angle.")}, // (FTE_CSQC_SKELETONOBJECTS)
        //(reads v_forward etc)
        //	{"skel_premul_bones",PF_skel_premul_bones,PF_skel_premul_bones,273,
        // D("void(float skel, float startbone, float endbone, vector org,
        // optional vector fwd, optional vector right, optional vector up)",
        //"Transforms an entire consecutive range of bones by a matrix. You
        // can use makevectors to generate a rotation matrix from an angle,
        // but you'll probably want to divide the angle by the number of
        // bones.")}, // (FTE_CSQC_SKELETONOBJECTS) (reads v_forward etc)
        //	{"skel_postmul_bone",PF_skel_postmul_bone,PF_skel_postmul_bone,0,
        // D("void(float skel, float bonenum, vector org, optional vector
        // fwd, optional vector right, optional vector up)", "Transforms a
        // single bone by a matrix. You can use makevectors to generate a
        // rotation matrix from an angle.")}, // (FTE_CSQC_SKELETONOBJECTS)
        //(reads v_forward etc)
        //	{"skel_postmul_bones",PF_skel_postmul_bones,PF_skel_postmul_bones,0,D("void(float
        // skel, float startbone, float endbone, vector org, optional vector
        // fwd, optional vector right, optional vector up)", "Transforms an
        // entire consecutive range of bones by a matrix. You can use
        // makevectors to generate a rotation matrix from an angle, but
        // you'll probably want to divide the angle by the number of
        // bones.")}, // (FTE_CSQC_SKELETONOBJECTS) (reads v_forward etc)
        //	{"skel_copybones",	PF_skel_copybones,	PF_skel_copybones,	274,
        // D("void(float skeldst, float skelsrc, float startbone, float
        // entbone)", "Copy bone data from one skeleton directly into
        // another.")}, // (FTE_CSQC_SKELETONOBJECTS)
        //	{"skel_delete",		PF_skel_delete,		PF_skel_delete,		275,
        // D("void(float skel)", "Deletes a skeletal object. The actual
        // delete is delayed, allowing the skeletal object to be deleted in
        // an entity's predraw function yet still be valid by the time the
        // addentity+renderscene builtins need it. Also uninstanciates any
        // ragdoll currently in effect on the skeletal object.")}, //
        //(FTE_CSQC_SKELETONOBJECTS)
        {"crossproduct", PF_crossproduct, PF_crossproduct, 0,
            D("#ifndef dotproduct\n#define dotproduct(v1,v2) "
              "((vector)(v1)*(vector)(v2))\n#endif\nvector(vector v1, "
              "vector v2)",
                "Small helper function to calculate the crossproduct of "
                "two vectors.")},
        //	{"pushmove", 		PF_pushmove, 		PF_pushmove,	 	0,
        //"float(entity pusher, vector move, vector amove)"},
        {"frameforname", PF_frameforname, PF_frameforname, 276,
            D("float(float modidx, string framename)",
                "Looks up a framegroup from a model by name, avoiding the "
                "need for hardcoding. Returns -1 on error.")}, // (FTE_CSQC_SKELETONOBJECTS)
        {"frameduration", PF_frameduration, PF_frameduration, 277,
            D("float(float modidx, float framenum)",
                "Retrieves the duration (in seconds) of the specified "
                "framegroup.")}, // (FTE_CSQC_SKELETONOBJECTS)
        //	{"processmodelevents",PF_processmodelevents,PF_processmodelevents,0,D("void(float
        // modidx, float framenum, __inout float basetime, float targettime,
        // void(float timestamp, int code, string data) callback)", "Calls a
        // callback for each event that has been reached. Basetime is set to
        // targettime.")},
        //	{"getnextmodelevent",PF_getnextmodelevent,PF_getnextmodelevent,0,
        // D("float(float modidx, float framenum, __inout float basetime,
        // float targettime, __out int code, __out string data)", "Reports
        // the next event within a model's animation. Returns a boolean if an
        // event was found between basetime and targettime. Writes to
        // basetime,code,data arguments (if an event was found, basetime is
        // set to the event's time, otherwise to targettime).\nWARNING: this
        // builtin cannot deal with multiple events with the same timestamp
        //(only the first will be reported).")},
        //	{"getmodeleventidx",PF_getmodeleventidx,PF_getmodeleventidx,0,
        // D("float(float modidx, float framenum, int eventidx, __out float
        // timestamp, __out int code, __out string data)", "Reports an
        // indexed event within a model's animation. Writes to
        // timestamp,code,data arguments on success. Returns false if the
        // animation/event/model was out of range/invalid. Does not consider
        // looping animations (retry from index 0 if it fails and you know
        // that its a looping animation). This builtin is more annoying to
        // use than getnextmodelevent, but can be made to deal with multiple
        // events with the exact same timestamp.")},
        ///	{"touchtriggers",	PF_touchtriggers,	PF_touchtriggers,	279,
        /// D("void(optional entity ent, optional vector neworigin)",
        ///"Triggers a touch events between self and every SOLID_TRIGGER
        /// entity that it is in contact with. This should typically just be
        /// the triggers touch functions. Also optionally updates the origin
        /// of the moved entity.")},//
        {"WriteFloat", PF_WriteFloat, PF_NoCSQC, 280,
            "void(float buf, float fl)"},
        //	{"skel_ragupdate",	PF_skel_ragedit,	PF_skel_ragedit,	281,
        // D("float(entity skelent, string dollcmd, float animskel)",
        //"Updates the skeletal object attached to the entity according to
        // its origin and other properties.\nif animskel is non-zero, the
        // ragdoll will animate towards the bone state in the animskel
        // skeletal object, otherwise they will pick up the model's base pose
        // which may not give nice results.\nIf dollcmd is not set, the
        // ragdoll will update (this should be done each frame).\nIf the doll
        // is updated without having a valid doll, the model's default .doll
        // will be instanciated.\ncommands:\n doll foo.doll : sets up the
        // entity to use the named doll file\n dollstring TEXT : uses the
        // doll file directly embedded within qc, with that extra prefix.\n
        // cleardoll : uninstanciates the doll without destroying the
        // skeletal object.\n animate 0.5 : specifies the strength of the
        // ragdoll as a whole \n animatebody somebody 0.5 : specifies the
        // strength of the ragdoll on a specific body (0 will disable ragdoll
        // animations on that body).\n enablejoint somejoint 1 : enables (or
        // disables) a joint. Disabling joints will allow the doll to
        // shatter.")}, // (FTE_CSQC_RAGDOLL)
        //	{"skel_mmap",		PF_skel_mmap,		PF_skel_mmap,		282,
        // D("float*(float skel)", "Map the bones in VM memory. They can then
        // be accessed via pointers. Each bone is 12 floats, the four vectors
        // interleaved (sadly).")},// (FTE_QC_RAGDOLL)
        //	{"skel_set_bone_world",PF_skel_set_bone_world,PF_skel_set_bone_world,283,D("void(entity
        // ent, float bonenum, vector org, optional vector angorfwd, optional
        // vector right, optional vector up)", "Sets the world position of a
        // bone within the given entity's attached skeletal object. The world
        // position is dependant upon the owning entity's position. If no
        // orientation argument is specified, v_forward+v_right+v_up are used
        // for the orientation instead. If 1 is specified, it is understood
        // as angles. If 3 are specified, they are the forawrd/right/up
        // vectors to use.")},
        {"frametoname", PF_frametoname, PF_frametoname, 284,
            "string(float modidx, float framenum)"},
        //	{"skintoname",		PF_skintoname,		PF_skintoname,		285,
        //"string(float modidx, float skin)"},
        //	{"resourcestatus",	PF_NoSSQC,			PF_resourcestatus,	286,
        // D("float(float resourcetype, float tryload, string resourcename)",
        //"resourcetype must be one of the RESTYPE_ constants. Returns one
        // of the RESSTATE_ constants. Tryload 0 is a query only. Tryload 1
        // will attempt to reload the content if it was flushed.")},
        //	{"hash_createtab",	PF_hash_createtab,	PF_hash_createtab,	287,
        // D("hashtable(float tabsize, optional float defaulttype)", "Creates
        // a hash table object with at least 'tabsize' slots. hash table with
        // index 0 is a game-persistant table and will NEVER be returned by
        // this builtin (except as an error return).")},
        //	{"hash_destroytab",	PF_hash_destroytab,	PF_hash_destroytab, 288,
        // D("void(hashtable table)", "Destroys a hash table object.")},
        //	{"hash_add",		PF_hash_add,		PF_hash_add,		289,
        // D("void(hashtable table, string name, __variant value, optional
        // float typeandflags)", "Adds the given key with the given value to
        // the table.\nIf flags&HASH_REPLACE, the old value will be removed,
        // if not set then multiple values may be added for a single key,
        // they won't overwrite.\nThe type argument describes how the value
        // should be stored and saved to files. While you can claim that all
        // variables are just vectors, being more precise can result in less
        // issues with tempstrings or saved games.")},
        //	{"hash_get",		PF_hash_get,		PF_hash_get,		290,
        // D("__variant(hashtable table, string name, optional __variant
        // deflt, optional float requiretype, optional float index)", "looks
        // up the specified key name in the hash table. returns deflt if key
        // was not found. If stringsonly=1, the return value will be in the
        // form of a tempstring, otherwise it'll be the original value
        // argument exactly as it was. If requiretype is specified, then
        // values not of the specified type will be ignored. Hurrah for
        // multiple types with the same name.")},
        //	{"hash_delete",		PF_hash_delete,		PF_hash_delete,		291,
        // D("__variant(hashtable table, string name)", "removes the named
        // key. returns the value of the object that was destroyed, or 0 on
        // error.")},
        //	{"hash_getkey",		PF_hash_getkey,		PF_hash_getkey,		292,
        // D("string(hashtable table, float idx)", "gets some random key
        // name. add+delete can change return values of this, so don't
        // blindly increment the key index if you're removing all.")},
        //	{"hash_getcb",		PF_hash_getcb,		PF_hash_getcb,		293,
        // D("void(hashtable table, void(string keyname, __variant val)
        // callback, optional string name)", "For each item in the table that
        // matches the name, call the callback. if name is omitted, will
        // enumerate ALL keys."), true},
        {"checkcommand", PF_checkcommand, PF_checkcommand, 294,
            D("float(string name)",
                "Checks to see if the supplied name is a valid command, "
                "cvar, or alias. Returns 0 if it does not exist.")},
        //	{"argescape",		PF_argescape,		PF_argescape,		295,
        // D("string(string s)", "Marks up a string so that it can be
        // reliably tokenized as a single argument later.")},
        //	{"clusterevent",	PF_clusterevent,	PF_NoCSQC,			0,
        // D("void(string dest, string from, string cmd, string info)", "Only
        // functions in mapcluster mode. Sends an event to whichever server
        // the named player is on. The destination server can then dispatch
        // the event to the client or handle it itself via the
        // SV_ParseClusterEvent entrypoint. If dest is empty, the event is
        // broadcast to ALL servers. If the named player can't be found, the
        // event will be returned to this server with the cmd prefixed with
        //'error:'.")},
        //	{"clustertransfer",	PF_clustertransfer,	PF_NoCSQC,			0,
        // D("string(entity player, optional string newnode)", "Only
        // functions in mapcluster mode. Initiate transfer of the player to a
        // different node. Can take some time. If dest is specified, returns
        // null on error. Otherwise returns the current/new target node (or
        // null if not transferring).")},
        //	{"modelframecount", PF_modelframecount, PF_modelframecount,	0,
        // D("float(float mdlidx)", "Retrieves the number of frames in the
        // specified model.")},

        //	{"clearscene",		PF_NoSSQC,			PF_FullCSQCOnly,	300,
        // D("void()", "Forgets all rentities, polygons, and temporary
        // dlights. Resets all view properties to their default values.")},//
        //(EXT_CSQC)
        //	{"addentities",		PF_NoSSQC,			PF_FullCSQCOnly,	301,
        // D("void(float mask)", "Walks through all entities effectively
        // doing this:\n if (ent.drawmask&mask){ if (!ent.predaw())
        // addentity(ent); }\nIf mask&MASK_DELTA, non-csqc entities,
        // particles, and related effects will also be added to the rentity
        // list.\n If mask&MASK_STDVIEWMODEL then the default view model will
        // also be added.")},// (EXT_CSQC)
        //	{"addentity",		PF_NoSSQC,			PF_FullCSQCOnly,	302,
        // D("void(entity ent)", "Copies the entity fields into a new rentity
        // for later rendering via addscene.")},// (EXT_CSQC)
        //	{"removeentity",	PF_NoSSQC,			PF_FullCSQCOnly,	0,
        // D("void(entity ent)", "Undoes all addentities added to the scene
        // from the given entity, without removing ALL entities (useful for
        // splitscreen/etc, readd modified versions as desired).")},//
        //(EXT_CSQC)
        //	{"addtrisoup_simple",PF_NoSSQC,			PF_FullCSQCOnly,	0,
        // D("typedef float vec2[2];\ntypedef float vec3[3];\ntypedef float
        // vec4[4];\ntypedef struct trisoup_simple_vert_s {vec3 xyz;vec2
        // st;vec4 rgba;} trisoup_simple_vert_t;\nvoid(string texturename,
        // int flags, struct trisoup_simple_vert_s *verts, int *indexes, int
        // numindexes)", "Adds the specified trisoup into the scene as
        // additional geometry. This permits caching geometry to reduce
        // builtin spam. Indexes are a triangle list (so eg quads will need 6
        // indicies to form two triangles). NOTE: this is not going to be a
        // speedup over polygons if you're still generating lots of new data
        // every frame.")},
        //	{"setproperty",		PF_NoSSQC,			PF_FullCSQCOnly,	303,
        // D("#define setviewprop setproperty\nfloat(float property, ...)",
        //"Allows you to override default view properties like viewport,
        // fov, and whether the engine hud will be drawn. Different VF_
        // values have slightly different arguments, some are vectors, some
        // floats.")},// (EXT_CSQC)
        //	{"renderscene",		PF_NoSSQC,			PF_FullCSQCOnly,	304,
        // D("void()", "Draws all entities, polygons, and particles on the
        // rentity list (which were added via addentities or addentity),
        // using the various view properties set via setproperty. There is no
        // ordering dependancy.\nThe scene must generally be cleared again
        // before more entities are added, as entities will persist even over
        // to the next frame.\nYou may call this builtin multiple times per
        // frame, but should only be called from CSQC_UpdateView.")},//
        //(EXT_CSQC)
        //	{"dynamiclight_add",PF_NoSSQC,			PF_FullCSQCOnly,	305,
        // D("float(vector org, float radius, vector lightcolours, optional
        // float style, optional string cubemapname, optional float pflags)",
        //"Adds a temporary dlight, ready to be drawn via addscene. Cubemap
        // orientation will be read from v_forward/v_right/v_up.")},//
        //(EXT_CSQC)
        {"R_BeginPolygon", PF_NoSSQC, PF_R_PolygonBegin, 306,
            D("void(string texturename, optional float flags, optional "
              "float is2d)",
                "Specifies the shader to use for the following polygons, "
                "along with optional flags.\nIf is2d, the polygon will be "
                "drawn as soon as the EndPolygon call is made, rather than "
                "waiting for renderscene. This allows complex 2d "
                "effects.")}, // (EXT_CSQC_???)
        {"R_PolygonVertex", PF_NoSSQC, PF_R_PolygonVertex, 307,
            D("void(vector org, vector texcoords, vector rgb, float alpha)",
                "Specifies a polygon vertex with its various properties.")}, // (EXT_CSQC_???)
        {"R_EndPolygon", PF_NoSSQC, PF_R_PolygonEnd, 308,
            D("void()",
                "Ends the current polygon. At least 3 verticies must have "
                "been specified. You do not need to call beginpolygon if "
                "you wish to draw another polygon with the same shader.")},
        //	{"getproperty",		PF_NoSSQC,			PF_FullCSQCOnly,	309,
        // D("#define getviewprop getproperty\n__variant(float property)",
        //"Retrieve a currently-set (typically view) property, allowing you
        // to read the current viewport or other things. Due to cheat
        // protection, certain values may be unretrievable.")},//
        //(EXT_CSQC_1)
        //	{"unproject",		PF_NoSSQC,			PF_FullCSQCOnly,	310,
        // D("vector (vector v)", "Transform a 2d screen-space point (with
        // depth) into a 3d world-space point, according the various
        // origin+angle+fov etc settings set via setproperty.")},//
        //(EXT_CSQC)
        //	{"project",			PF_NoSSQC,			PF_FullCSQCOnly,	311,
        // D("vector (vector v)", "Transform a 3d world-space point into a 2d
        // screen-space point, according the various origin+angle+fov etc
        // settings set via setproperty.")},// (EXT_CSQC)
        //	{"drawtextfield",	PF_NoSSQC,			PF_FullCSQCOnly,	0,
        // D("void(vector pos, vector size, float alignflags, string text)",
        //"Draws a multi-line block of text, including word wrapping and
        // alignment. alignflags bits are RTLB, typically 3.")},// (EXT_CSQC)
        //	{"drawline",		PF_NoSSQC,			PF_FullCSQCOnly,	315,
        // D("void(float width, vector pos1, vector pos2, vector rgb, float
        // alpha, optional float drawflag)", "Draws a 2d line between the two
        // 2d points.")},// (EXT_CSQC)
        {"iscachedpic", PF_NoSSQC, PF_cl_iscachedpic, 316,
            D("float(string name)",
                "Checks to see if the image is currently loaded. Engines "
                "might lie, or cache between maps.")}, // (EXT_CSQC)
        {"precache_pic", PF_NoSSQC, PF_cl_precachepic, 317,
            D("string(string name, optional float trywad)",
                "Forces the engine to load the named image. If trywad is "
                "specified, the specified name must any lack path and "
                "extension.")}, // (EXT_CSQC)
        //	{"r_uploadimage",	PF_NoSSQC,			PF_FullCSQCOnly,	0,
        // D("void(string imagename, int width, int height, int *pixeldata)",
        //"Updates a texture with the specified rgba data. Will be created
        // if needed.")},
        //	{"r_readimage",		PF_NoSSQC,			PF_FullCSQCOnly,	0,
        // D("int*(string filename, __out int width, __out int height)",
        //"Reads and decodes an image from disk, providing raw pixel data.
        // Returns __NULL__ if the image could not be read for any reason.
        // Use memfree to free the data once you're done with it.")},
        {"drawgetimagesize", PF_NoSSQC, PF_cl_getimagesize, 318,
            D("#define draw_getimagesize drawgetimagesize\nvector(string "
              "picname)",
                "Returns the dimensions of the named image. Images "
                "specified with .lmp should give the original .lmp's "
                "dimensions even if texture replacements use a different "
                "resolution.")}, // (EXT_CSQC)
        //	{"freepic",			PF_NoSSQC,			PF_FullCSQCOnly,	319,
        // D("void(string name)", "Tells the engine that the image is no
        // longer needed. The image will appear to be new the next time its
        // needed.")},// (EXT_CSQC)
        {"drawcharacter", PF_NoSSQC, PF_cl_drawcharacter, 320,
            D("float(vector position, float character, vector size, vector "
              "rgb, float alpha, optional float drawflag)",
                "Draw the given quake character at the given position.\nIf "
                "flag&4, the function will consider the char to be a "
                "unicode char instead (or display as a ? if outside the "
                "32-127 range).\nsize should normally be something like '8 "
                "8 0'.\nrgb should normally be '1 1 1'\nalpha normally "
                "1.\nSoftware engines may assume the named defaults.\nNote "
                "that ALL text may be rescaled on the X axis due to "
                "variable width fonts. The X axis may even be ignored "
                "completely.")}, // (EXT_CSQC, [EXT_CSQC_???])
        {"drawrawstring", PF_NoSSQC, PF_cl_drawrawstring, 321,
            D("float(vector position, string text, vector size, vector "
              "rgb, float alpha, optional float drawflag)",
                "Draws the specified string without using any markup at "
                "all, even in engines that support it.\nIf UTF-8 is "
                "globally enabled in the engine, then that encoding is "
                "used (without additional markup), otherwise it is raw "
                "quake chars.\nSoftware engines may assume a size of '8 8 "
                "0', rgb='1 1 1', alpha=1, flag&3=0, but it is not an "
                "error to draw out of the screen.")}, // (EXT_CSQC,
                                                      // [EXT_CSQC_???])
        {"drawpic", PF_NoSSQC, PF_cl_drawpic, 322,
            D("float(vector position, string pic, vector size, vector rgb, "
              "float alpha, optional float drawflag)",
                "Draws an shader within the given 2d screen box. Software "
                "engines may omit support for rgb+alpha, but must support "
                "rescaling, and must clip to the screen without "
                "crashing.")}, // (EXT_CSQC, [EXT_CSQC_???])
        {"drawfill", PF_NoSSQC, PF_cl_drawfill, 323,
            D("float(vector position, vector size, vector rgb, float "
              "alpha, optional float drawflag)",
                "Draws a solid block over the given 2d box, with given "
                "colour, alpha, and blend mode (specified via "
                "flags).\nflags&3=0 simple blend.\nflags&3=1 additive "
                "blend")}, // (EXT_CSQC, [EXT_CSQC_???])
        {"drawsetcliparea", PF_NoSSQC, PF_cl_drawsetclip, 324,
            D("void(float x, float y, float width, float height)",
                "Specifies a 2d clipping region (aka: scissor test). 2d "
                "draw calls will all be clipped to this 2d box, the area "
                "outside will not be modified by any 2d draw call (even 2d "
                "polygons).")}, // (EXT_CSQC_???)
        {"drawresetcliparea", PF_NoSSQC, PF_cl_drawresetclip, 325,
            D("void()",
                "Reverts the scissor/clip area to the whole "
                "screen.")}, // (EXT_CSQC_???)
        {"drawstring", PF_NoSSQC, PF_cl_drawstring, 326,
            D("float(vector position, string text, vector size, vector "
              "rgb, float alpha, float drawflag)",
                "Draws a string, interpreting markup and recolouring as "
                "appropriate.")}, // #326
        {"stringwidth", PF_NoSSQC, PF_cl_stringwidth, 327,
            D("float(string text, float usecolours, optional vector "
              "fontsize)",
                "Calculates the width of the screen in virtual pixels. If "
                "usecolours is 1, markup that does not affect the string "
                "width will be ignored. Will always be decoded as UTF-8 if "
                "UTF-8 is globally enabled.\nIf the char size is not "
                "specified, '8 8 0' will be assumed.")}, // EXT_CSQC_'DARKPLACES'
        {"drawsubpic", PF_NoSSQC, PF_cl_drawsubpic, 328,
            D("void(vector pos, vector sz, string pic, vector srcpos, "
              "vector srcsz, vector rgb, float alpha, optional float "
              "drawflag)",
                "Draws a rescaled subsection of an image to the screen.")}, // #328 EXT_CSQC_'DARKPLACES'
        //	{"drawrotpic",		PF_NoSSQC,			PF_FullCSQCOnly,	0,
        // D("void(vector pivot, vector mins, vector maxs, string pic, vector
        // rgb, float alpha, float angle)", "Draws an image rotating at the
        // pivot. To rotate in the center, use mins+maxs of half the size
        // with mins negated. Angle is in degrees.")},
        //	{"drawrotsubpic",	PF_NoSSQC,			PF_FullCSQCOnly,	0,
        // D("void(vector pivot, vector mins, vector maxs, string pic, vector
        // txmin, vector txsize, vector rgb, vector alphaandangles)",
        //"Overcomplicated draw function for over complicated people.
        // Positions follow drawrotpic, while texture coords follow
        // drawsubpic. Due to argument count limitations in builtins, the
        // alpha value and angles are combined into separate fields of a
        // vector (tip: use fteqcc's [alpha, angle] feature.")},
        {"getstati", PF_NoSSQC, PF_cl_getstat_int, 330,
            D("#define getstati_punf(stnum) "
              "(float)(__variant)getstati(stnum)\nint(float stnum)",
                "Retrieves the numerical value of the given EV_INTEGER or "
                "EV_ENTITY stat. Use getstati_punf if you wish to type-pun "
                "a float stat as an int to avoid truncation issues in "
                "DP.")}, // (EXT_CSQC)
        {"getstatf", PF_NoSSQC, PF_cl_getstat_float, 331,
            D("#define getstatbits getstatf\nfloat(float stnum, optional "
              "float firstbit, optional float bitcount)",
                "Retrieves the numerical value of the given EV_FLOAT stat. "
                "If firstbit and bitcount are specified, retrieves the "
                "upper bits of the STAT_ITEMS stat (converted into a "
                "float, so there are no VM dependancies).")}, // (EXT_CSQC)
        //	{"getstats",		PF_NoSSQC,			PF_cl_getstat_str,	332,
        // D("string(float stnum)", "Retrieves the value of the given
        // EV_STRING stat, as a tempstring.\nOlder engines may use 4
        // consecutive integer stats, with a limit of 15 chars (yes,
        // really. 15.), but "FULLENGINENAME" uses a separate namespace for
        // string stats and has a much higher length limit.")},
        //	{"getplayerstat",	PF_NoSSQC,			PF_FullCSQCOnly,	0,
        // D("__variant(float playernum, float statnum, float stattype)",
        //"Retrieves a specific player's stat, matching the type specified
        // on the server. This builtin is primarily intended for mvd playback
        // where ALL players are known. For EV_ENTITY, world will be returned
        // if the entity is not in the pvs, use type-punning with EV_INTEGER
        // to get the entity number if you just want to see if its set.
        // STAT_ITEMS should be queried as an EV_INTEGER on account of runes
        // and items2 being packed into the upper bits.")},
        {"setmodelindex", PF_sv_setmodelindex, PF_cl_setmodelindex, 333,
            D("void(entity e, float mdlindex)",
                "Sets a model by precache index instead of by name. "
                "Otherwise identical to setmodel.")}, //
        //	{"modelnameforindex",PF_modelnameforidx,PF_modelnameforidx, 334,
        // D("string(float mdlindex)", "Retrieves the name of the model based
        // upon a precache index. This can be used to reduce csqc network
        // traffic by enabling model matching.")},//
        {"particleeffectnum", PF_sv_particleeffectnum, PF_cl_particleeffectnum,
            335,
            D("float(string effectname)",
                "Precaches the named particle effect. If your effect name "
                "is of the form 'foo.bar' then particles/foo.cfg will be "
                "loaded by the client if foo.bar was not already "
                "defined.\nDifferent engines will have different particle "
                "systems, this specifies the QC API only.")}, // (EXT_CSQC)
        {"trailparticles", PF_sv_trailparticles, PF_cl_trailparticles, 336,
            D("void(float effectnum, entity ent, vector start, vector end)",
                "Draws the given effect between the two named points. If "
                "ent is not world, distances will be cached in the entity "
                "in order to avoid framerate dependancies. The entity is "
                "not otherwise used.")}, // (EXT_CSQC),
        {"pointparticles", PF_sv_pointparticles, PF_cl_pointparticles, 337,
            D("void(float effectnum, vector origin, optional vector dir, "
              "optional float count)",
                "Spawn a load of particles from the given effect at the "
                "given point traveling or aiming along the direction "
                "specified. The number of particles are scaled by the "
                "count argument.")}, // (EXT_CSQC)
        {"cprint", PF_NoSSQC, PF_cl_cprint, 338,
            D("void(string s, ...)",
                "Print into the center of the screen just as ssqc's "
                "centerprint would appear.")}, //(EXT_CSQC)
        {"print", PF_print, PF_print, 339,
            D("void(string s, ...)",
                "Unconditionally print on the local system's console, even "
                "in ssqc (doesn't care about the value of the developer "
                "cvar).")}, //(EXT_CSQC)
        {"keynumtostring", nullptr, PF_cl_keynumtostring, 340,
            D("string(float keynum)",
                "Returns a hunam-readable name for the given keycode, as a "
                "tempstring.")}, // (EXT_CSQC)
        {"stringtokeynum", nullptr, PF_cl_stringtokeynum, 341,
            D("float(string keyname)",
                "Looks up the key name in the same way that the bind "
                "command would, returning the keycode for that key.")}, // (EXT_CSQC)
        {"getkeybind", nullptr, PF_cl_getkeybind, 342,
            D("string(float keynum)",
                "Returns the current binding for the given key (returning "
                "only the command executed when no modifiers are "
                "pressed).")}, // (EXT_CSQC)
        {"setcursormode", PF_NoSSQC, PF_cl_setcursormode, 343,
            D("void(float usecursor, optional string cursorimage, optional "
              "vector hotspot, optional float scale)",
                "Pass TRUE if you want the engine to release the mouse "
                "cursor (absolute input events + touchscreen mode). Pass "
                "FALSE if you want the engine to grab the cursor (relative "
                "input events + standard looking). If the image name is "
                "specified, the engine will use that image for a cursor "
                "(use an empty string to clear it again), in a way that "
                "will not conflict with the console. Images specified this "
                "way will be hardware accelerated, if supported by the "
                "platform/port.")},
        {"getcursormode", PF_NoSSQC, PF_cl_getcursormode, 0,
            D("float(float effective)",
                "Reports the cursor mode this module previously attempted "
                "to use. If 'effective' is true, reports the cursor mode "
                "currently active (if was overriden by a different module "
                "which has precidence, for instance, or if there is only a "
                "touchscreen and no mouse).")},
        //	{"getmousepos",		PF_NoSSQC,			PF_FullCSQCOnly,	344,
        // D("vector()", "Nasty convoluted DP extension. Typically returns
        // deltas instead of positions. Use CSQC_InputEvent for such things
        // in csqc mods.")},	// #344 This is a DP extension
        //	{"getinputstate",	PF_NoSSQC,			PF_FullCSQCOnly,	345,
        // D("float(float inputsequencenum)", "Looks up an input frame from
        // the log, setting the input_* globals accordingly.\nThe sequence
        // number range used for prediction should normally be
        // servercommandframe < sequence <= clientcommandframe.\nThe sequence
        // equal to clientcommandframe will change between input
        // frames.")},// (EXT_CSQC)
        {"setsensitivityscaler", PF_NoSSQC, PF_cl_setsensitivity, 346,
            D("void(float sens)",
                "Temporarily scales the player's mouse sensitivity based "
                "upon something like zoom, avoiding potential cvar saving "
                "and thus corruption.")}, // (EXT_CSQC)
        //	{"runstandardplayerphysics",nullptr,		PF_FullCSQCOnly,	347,
        // D("void(entity ent)", "Perform the engine's standard player
        // movement prediction upon the given entity using the input_*
        // globals to describe movement.")},
        {"getplayerkeyvalue", nullptr, PF_cl_playerkey_s, 348,
            D("string(float playernum, string keyname)",
                "Look up a player's userinfo, to discover things like "
                "their name, topcolor, bottomcolor, skin, team, "
                "*ver.\nAlso includes scoreboard info like frags, ping, "
                "pl, userid, entertime, as well as voipspeaking and "
                "voiploudness.")}, // (EXT_CSQC)
        {"getplayerkeyfloat", nullptr, PF_cl_playerkey_f, 0,
            D("float(float playernum, string keyname, optional float "
              "assumevalue)",
                "Cheaper version of getplayerkeyvalue that avoids the need "
                "for so many tempstrings.")},
        {"isdemo", PF_NoSSQC, PF_cl_isdemo, 349,
            D("float()",
                "Returns if the client is currently playing a demo or "
                "not")}, // (EXT_CSQC)
        {"isserver", PF_NoSSQC, PF_cl_isserver, 350,
            D("float()",
                "Returns if the client is acting as the server (aka: "
                "listen server)")}, //(EXT_CSQC)
        //	{"SetListener",		nullptr,				PF_FullCSQCOnly,	351,
        // D("void(vector origin, vector forward, vector right, vector up,
        // optional float reverbtype)", "Sets the position of the view, as
        // far as the audio subsystem is concerned. This should be called
        // once per CSQC_UpdateView as it will otherwise revert to default.
        // For reverbtype, see setup_reverb or treat as 'underwater'.")},//
        //(EXT_CSQC)
        //	{"setup_reverb",	PF_NoSSQC,			PF_FullCSQCOnly,	0,
        // D("typedef struct {\n\tfloat flDensity;\n\tfloat
        // flDiffusion;\n\tfloat flGain;\n\tfloat flGainHF;\n\tfloat
        // flGainLF;\n\tfloat flDecayTime;\n\tfloat flDecayHFRatio;\n\tfloat
        // flDecayLFRatio;\n\tfloat flReflectionsGain;\n\tfloat
        // flReflectionsDelay;\n\tvector flReflectionsPan;\n\tfloat
        // flLateReverbGain;\n\tfloat flLateReverbDelay;\n\tvector
        // flLateReverbPan;\n\tfloat flEchoTime;\n\tfloat
        // flEchoDepth;\n\tfloat flModulationTime;\n\tfloat
        // flModulationDepth;\n\tfloat flAirAbsorptionGainHF;\n\tfloat
        // flHFReference;\n\tfloat flLFReference;\n\tfloat
        // flRoomRolloffFactor;\n\tint   iDecayHFLimit;\n}
        // reverbinfo_t;\nvoid(float reverbslot, reverbinfo_t *reverbinfo,
        // int sizeofreverinfo_t)", "Reconfigures a reverb slot for weird
        // effects. Slot 0 is reserved for no effects. Slot 1 is reserved for
        // underwater effects. Reserved slots will be reinitialised on
        // snd_restart, but can otherwise be changed. These reverb slots can
        // be activated with SetListener. Note that reverb will currently
        // only work when using OpenAL.")},
        {"registercommand", nullptr, PF_cl_registercommand, 352,
            D("void(string cmdname)",
                "Register the given console command, for easy console "
                "use.\nConsole commands that are later used will invoke "
                "CSQC_ConsoleCommand.")}, //(EXT_CSQC)
        {"wasfreed", PF_WasFreed, PF_WasFreed, 353,
            D("float(entity ent)",
                "Quickly check to see if the entity is currently free. "
                "This function is only valid during the two-second "
                "non-reuse window, after that it may give bad results. Try "
                "one second to make it more robust.")}, //(EXT_CSQC) (should
                                                        // be availabe on
                                                        // server too)
        {"serverkey", nullptr, PF_cl_serverkey_s, 354,
            D("string(string key)",
                "Look up a key in the server's public serverinfo string")}, //
        {"serverkeyfloat", nullptr, PF_cl_serverkey_f, 0,
            D("float(string key, optional float assumevalue)",
                "Version of serverkey that returns the value as a float "
                "(which avoids tempstrings).")}, //
        //	{"getentitytoken",	nullptr,				PF_FullCSQCOnly,	355,
        // D("string(optional string resetstring)", "Grab the next token in
        // the map's entity lump.\nIf resetstring is not specified, the next
        // token will be returned with no other sideeffects.\nIf empty, will
        // reset from the map before returning the first token, probably
        //{.\nIf not empty, will tokenize from that string instead.\nAlways
        // returns tempstrings.")},//;
        //	{"findfont",		PF_NoSSQC,			PF_FullCSQCOnly,	356,
        // D("float(string s)", "Looks up a named font slot. Matches the
        // actual font name as a last resort.")},//;
        //	{"loadfont",		PF_NoSSQC,			PF_FullCSQCOnly,	357,
        // D("float(string fontname, string fontmaps, string sizes, float
        // slot, optional float fix_scale, optional float fix_voffset)", "too
        // convoluted for me to even try to explain correct usage. Try
        // drawfont = loadfont(\"\", \"cour\", \"16\", -1, 0, 0); to switch
        // to the courier font (optimised for 16 virtual pixels high), if you
        // have the freetype2 library in windows..")},
        {"sendevent", PF_NoSSQC, PF_cl_sendevent, 359,
            D("void(string evname, string evargs, ...)",
                "Invoke Cmd_evname_evargs in ssqc. evargs must be a string "
                "of initials refering to the types of the arguments to "
                "pass. v=vector, e=entity(.entnum field is sent), f=float, "
                "i=int. 6 arguments max - you can get more if you pack "
                "your floats into vectors.")},                   // (EXT_CSQC_1)
        {"readbyte", PF_NoSSQC, PF_cl_readbyte, 360, "float()"}, // (EXT_CSQC)
        {"readchar", PF_NoSSQC, PF_cl_readchar, 361, "float()"}, // (EXT_CSQC)
        {"readshort", PF_NoSSQC, PF_cl_readshort, 362, "float()"}, // (EXT_CSQC)
        {"readlong", PF_NoSSQC, PF_cl_readlong, 363, "float()"},   // (EXT_CSQC)
        {"readcoord", PF_NoSSQC, PF_cl_readcoord, 364, "float()"}, // (EXT_CSQC)
        {"readangle", PF_NoSSQC, PF_cl_readangle, 365, "float()"}, // (EXT_CSQC)
        {"readstring", PF_NoSSQC, PF_cl_readstring, 366,
            "string()"},                                           // (EXT_CSQC)
        {"readfloat", PF_NoSSQC, PF_cl_readfloat, 367, "float()"}, // (EXT_CSQC)
        {"readentitynum", PF_NoSSQC, PF_cl_readentitynum, 368,
            "float()"}, // (EXT_CSQC)
                        //	{"deltalisten",		nullptr,
                        // PF_FullCSQCOnly, 371,
        // D("float(string modelname, float(float isnew) updatecallback,
        // float flags)", "Specifies a per-modelindex callback to listen for
        // engine-networking entity updates. Such entities are automatically
        // interpolated by the engine (unless flags specifies not to).\nThe
        // various standard entity fields will be overwritten each frame
        // before the updatecallback function is called.")},//  (EXT_CSQC_1)
        //	{"dynamiclight_get",PF_NoSSQC,			PF_FullCSQCOnly,	372,
        // D("__variant(float lno, float fld)", "Retrieves a property from
        // the given dynamic/rt light. Return type depends upon the light
        // field requested.")},
        //	{"dynamiclight_set",PF_NoSSQC,			PF_FullCSQCOnly,	373,
        // D("void(float lno, float fld, __variant value)", "Changes a
        // property on the given dynamic/rt light. Value type depends upon
        // the light field to be changed.")},
        //	{"particleeffectquery",PF_NoSSQC,		PF_FullCSQCOnly,	374,
        // D("string(float efnum, float body)", "Retrieves either the name or
        // the body of the effect with the given number. The effect body is
        // regenerated from internal state, and can be changed before being
        // reapplied via the localcmd builtin.")},
        //	{"adddecal",		PF_NoSSQC,			PF_FullCSQCOnly,	375,
        // D("void(string shadername, vector origin, vector up, vector side,
        // vector rgb, float alpha)", "Adds a temporary clipped decal shader
        // to the scene, centered at the given point with given orientation.
        // Will be drawn by the next renderscene call, and freed by the next
        // clearscene call.")},
        //	{"setcustomskin",	PF_NoSSQC,			PF_FullCSQCOnly,	376,
        // D("void(entity e, string skinfilename, optional string skindata)",
        //"Sets an entity's skin overrides. These are custom per-entity
        // surface->shader lookups. The skinfilename/data should be in .skin
        // format:\nsurfacename,shadername - makes the named surface use the
        // named shader\nreplace \"surfacename\" \"shadername\" -
        // same.\nqwskin \"foo\" - use an unmodified quakeworld player skin
        //(including crop+repalette rules)\nq1lower 0xff0000 - specify an
        // override for the entity's lower colour, in this case to
        // red\nq1upper 0x0000ff - specify an override for the entity's lower
        // colour, in this case to blue\ncompose \"surfacename\" \"shader\"
        //\"imagename@x,y:w,h$s,t,s2,t2?r,g,b,a\" - compose a skin texture
        // from multiple images.\n  The texture is determined to be
        // sufficient to hold the first named image, additional images can be
        // named as extra tokens on the same line.\n  Use a + at the end of
        // the line to continue reading image tokens from the next line also,
        // the named shader must use 'map $diffuse' to read the composed
        // texture (compatible with the defaultskin shader).")},
        //	{"memalloc",		PF_memalloc,		PF_memalloc,		384,
        // D("__variant*(int size)", "Allocate an arbitary block of
        // memory")},
        //	{"memfree",			PF_memfree,			PF_memfree,			385,
        // D("void(__variant *ptr)", "Frees a block of memory that was
        // allocated with memfree")},
        //	{"memcpy",			PF_memcpy,			PF_memcpy,			386,
        // D("void(__variant *dst, __variant *src, int size)", "Copys memory
        // from one location to another")},
        //	{"memfill8",		PF_memfill8,		PF_memfill8,		387,
        // D("void(__variant *dst, int val, int size)", "Sets an entire block
        // of memory to a specified value. Pretty much always 0.")},
        //	{"memgetval",		PF_memgetval,		PF_memgetval,		388,
        // D("__variant(__variant *dst, float ofs)", "Looks up the 32bit
        // value stored at a pointer-with-offset.")},
        //	{"memsetval",		PF_memsetval,		PF_memsetval,		389,
        // D("void(__variant *dst, float ofs, __variant val)", "Changes the
        // 32bit value stored at the specified pointer-with-offset.")},
        //	{"memptradd",		PF_memptradd,		PF_memptradd,		390,
        // D("__variant*(__variant *base, float ofs)", "Perform some pointer
        // maths. Woo.")},
        //	{"memstrsize",		PF_memstrsize,		PF_memstrsize,		0,
        // D("float(string s)", "strlen, except ignores utf-8")},
        //	{"con_getset",		PF_NoSSQC,			PF_FullCSQCOnly,
        // 391, D("string(string conname, string field, optional string
        // newvalue)", "Reads or sets a property from a console object. The
        // old value is returned. Iterrate through consoles with the 'next'
        // field. Valid properties: 	title, name, next, unseen, markup,
        // forceutf8, close, clear, hidden, linecount")},
        //	{"con_printf",		PF_NoSSQC,			PF_FullCSQCOnly,
        // 392, D("void(string conname, string messagefmt, ...)", "Prints onto a
        // named console.")},
        //	{"con_draw",		PF_NoSSQC,			PF_FullCSQCOnly,
        // 393, D("void(string conname, vector pos, vector size, float
        // fontsize)", "Draws the named console.")},
        //	{"con_input",		PF_NoSSQC,			PF_FullCSQCOnly,
        // 394, D("float(string conname, float inevtype, float parama, float
        // paramb, float paramc)", "Forwards input events to the named
        // console. Mouse updates should be absolute only.")},
        {"setwindowcaption", PF_NoSSQC, PF_cl_setwindowcaption, 0,
            D("void(string newcaption)",
                "Replaces the title of the game window, as seen when task "
                "switching or just running in windowed mode.")},
        //	{"cvars_haveunsaved",PF_NoSSQC,			PF_FullCSQCOnly,
        // 0, D("float()", "Returns true if any archived cvar has an unsaved
        // value.")},
        //	{"entityprotection",nullptr,				nullptr, 0,
        // D("float(entity e, float nowreadonly)", "Changes the protection on
        // the specified entity to protect it from further edits from QC. The
        // return value is the previous setting. Note that this can be used
        // to unprotect the world, but doing so long term is not advised as
        // you will no longer be able to detect invalid entity references.
        // Also, world is not networked, so results might not be seen by
        // clients (or in other words, world.avelocity_y=64 is a bad
        // idea).")},


        {"copyentity", PF_copyentity, PF_copyentity, 400,
            D("entity(entity from, optional entity to)",
                "Copies all fields from one entity to another.")}, // (DP_QC_COPYENTITY)
        {"setcolors", PF_setcolors, PF_NoCSQC, 401,
            D("void(entity ent, float colours)",
                "Changes a player's colours. The bits 0-3 are the "
                "lower/trouser colour, bits 4-7 are the upper/shirt "
                "colours.")}, // DP_SV_SETCOLOR
        {"findchain", PF_findchain, PF_findchain, 402,
            "entity(.string field, string match)"}, // (DP_QC_FINDCHAIN)
        {"findchainfloat", PF_findchainfloat, PF_findchainfloat, 403,
            "entity(.float fld, float match)"}, // (DP_QC_FINDCHAINFLOAT)
        {"effect", PF_sv_effect, nullptr, 404,
            D("void(vector org, string modelname, float startframe, float "
              "endframe, float framerate)",
                "stub. Spawns a self-animating sprite")}, // (DP_SV_EFFECT)
        {"te_blood", PF_sv_te_blooddp, nullptr, 405,
            "void(vector org, vector dir, float count)"}, // #405 te_blood
        {"te_bloodshower", PF_sv_te_bloodshower, nullptr, 406,
            "void(vector mincorner, vector maxcorner, float "
            "explosionspeed, float howmany)",
            "stub."}, // (DP_TE_BLOODSHOWER)
        {"te_explosionrgb", PF_sv_te_explosionrgb, nullptr, 407,
            "void(vector org, vector color)", "stub."}, // (DP_TE_EXPLOSIONRGB)
        {"te_particlecube", PF_sv_te_particlecube, nullptr, 408,
            "void(vector mincorner, vector maxcorner, vector vel, float "
            "howmany, float color, float gravityflag, float "
            "randomveljitter)",
            "stub."}, // (DP_TE_PARTICLECUBE)
        {"te_particlerain", PF_sv_te_particlerain, nullptr, 409,
            "void(vector mincorner, vector maxcorner, vector vel, float "
            "howmany, float color)"}, // (DP_TE_PARTICLERAIN)
        {"te_particlesnow", PF_sv_te_particlesnow, nullptr, 410,
            "void(vector mincorner, vector maxcorner, vector vel, float "
            "howmany, float color)"}, // (DP_TE_PARTICLESNOW)
        {"te_spark", PF_sv_te_spark, nullptr, 411,
            "void(vector org, vector vel, float howmany)",
            "stub."}, // (DP_TE_SPARK)
        {"te_gunshotquad", PF_sv_te_gunshotquad, nullptr, 412,
            "void(vector org)", "stub."}, // (DP_TE_QUADEFFECTS1)
        {"te_spikequad", PF_sv_te_spikequad, nullptr, 413, "void(vector org)",
            "stub."}, // (DP_TE_QUADEFFECTS1)
        {"te_superspikequad", PF_sv_te_superspikequad, nullptr, 414,
            "void(vector org)", "stub."}, // (DP_TE_QUADEFFECTS1)
        {"te_explosionquad", PF_sv_te_explosionquad, nullptr, 415,
            "void(vector org)", "stub."}, // (DP_TE_QUADEFFECTS1)
        {"te_smallflash", PF_sv_te_smallflash, nullptr, 416, "void(vector org)",
            "stub."}, // (DP_TE_SMALLFLASH)
        {"te_customflash", PF_sv_te_customflash, nullptr, 417,
            "void(vector org, float radius, float lifetime, vector color)",
            "stub."}, // (DP_TE_CUSTOMFLASH)
        {"te_gunshot", PF_sv_te_gunshot, nullptr, 418,
            "void(vector org, optional float count)"}, // #418 te_gunshot
        {"te_spike", PF_sv_te_spike, nullptr, 419,
            "void(vector org)"}, // #419 te_spike
        {"te_superspike", PF_sv_te_superspike, nullptr, 420,
            "void(vector org)"}, // #420 te_superspike
        {"te_explosion", PF_sv_te_explosion, nullptr, 421,
            "void(vector org)"}, // #421 te_explosion
        {"te_tarexplosion", PF_sv_te_tarexplosion, nullptr, 422,
            "void(vector org)"}, // #422 te_tarexplosion
        {"te_wizspike", PF_sv_te_wizspike, nullptr, 423,
            "void(vector org)"}, // #423 te_wizspike
        {"te_knightspike", PF_sv_te_knightspike, nullptr, 424,
            "void(vector org)"}, // #424 te_knightspike
        {"te_lavasplash", PF_sv_te_lavasplash, nullptr, 425,
            "void(vector org)"}, // #425 te_lavasplash
        {"te_teleport", PF_sv_te_teleport, nullptr, 426,
            "void(vector org)"}, // #426 te_teleport
        {"te_explosion2", PF_sv_te_explosion2, nullptr, 427,
            "void(vector org, float color, float colorlength)"}, // #427
                                                                 // te_explosion2
        {"te_lightning1", PF_sv_te_lightning1, nullptr, 428,
            "void(entity own, vector start, vector end)"}, // #428
                                                           // te_lightning1
        {"te_lightning2", PF_sv_te_lightning2, nullptr, 429,
            "void(entity own, vector start, vector end)"}, // #429
                                                           // te_lightning2
        {"te_lightning3", PF_sv_te_lightning3, nullptr, 430,
            "void(entity own, vector start, vector end)"}, // #430
                                                           // te_lightning3
        {"te_beam", PF_sv_te_beam, nullptr, 431,
            "void(entity own, vector start, vector end)"}, // #431 te_beam
        {"vectorvectors", PF_vectorvectors, PF_vectorvectors, 432,
            "void(vector dir)"}, // (DP_QC_VECTORVECTORS)
        {"te_plasmaburn", PF_sv_te_plasmaburn, nullptr, 433, "void(vector org)",
            "stub."}, // (DP_TE_PLASMABURN)
        {"getsurfacenumpoints", PF_getsurfacenumpoints, PF_getsurfacenumpoints,
            434, "float(entity e, float s)"}, // (DP_QC_GETSURFACE)
        {"getsurfacepoint", PF_getsurfacepoint, PF_getsurfacepoint, 435,
            "vector(entity e, float s, float n)"}, // (DP_QC_GETSURFACE)
        {"getsurfacenormal", PF_getsurfacenormal, PF_getsurfacenormal, 436,
            "vector(entity e, float s)"}, // (DP_QC_GETSURFACE)
        {"getsurfacetexture", PF_getsurfacetexture, PF_getsurfacetexture, 437,
            "string(entity e, float s)"}, // (DP_QC_GETSURFACE)
        {"getsurfacenearpoint", PF_getsurfacenearpoint, PF_getsurfacenearpoint,
            438, "float(entity e, vector p)"}, // (DP_QC_GETSURFACE)
        {"getsurfaceclippedpoint", PF_getsurfaceclippedpoint,
            PF_getsurfaceclippedpoint, 439,
            "vector(entity e, float s, vector p)"}, // (DP_QC_GETSURFACE)
        {"clientcommand", PF_clientcommand, PF_NoCSQC, 440,
            "void(entity e, string s)"}, // (KRIMZON_SV_PARSECLIENTCOMMAND)
        {"tokenize", PF_Tokenize, PF_Tokenize, 441,
            "float(string s)"}, // (KRIMZON_SV_PARSECLIENTCOMMAND)
        {"argv", PF_ArgV, PF_ArgV, 442,
            "string(float n)"}, // (KRIMZON_SV_PARSECLIENTCOMMAND
        {"argc", PF_ArgC, PF_ArgC, 0, "float()"},
        {"setattachment", PF_setattachment, PF_setattachment, 443,
            "void(entity e, entity tagentity, string tagname)",
            ""}, // (DP_GFX_QUAKE3MODELTAGS)
        {"search_begin", PF_search_begin, PF_search_begin, 444,
            "searchhandle(string pattern, optional float caseinsensitive, "
            "optional float quiet)",
            "initiate a filesystem scan based upon filenames. Be sure to "
            "call search_end on the returned handle."},
        {"search_end", PF_search_end, PF_search_end, 445,
            "void(searchhandle handle)", ""},
        {"search_getsize", PF_search_getsize, PF_search_getsize, 446,
            "float(searchhandle handle)",
            " Retrieves the number of files that were found."},
        {"search_getfilename", PF_search_getfilename, PF_search_getfilename,
            447, "string(searchhandle handle, float num)",
            "Retrieves name of one of the files that was found by the "
            "initial search."},
        {"search_getfilesize", PF_search_getfilesize, PF_search_getfilesize, 0,
            "float(searchhandle handle, float num)",
            "Retrieves the size of one of the files that was found by the "
            "initial search."},
        {"search_getfilemtime", PF_search_getfilemtime, PF_search_getfilemtime,
            0, "string(searchhandle handle, float num)",
            "Retrieves modification time of one of the files in %Y-%m-%d "
            "%H:%M:%S format."},
        {"cvar_string", PF_cvar_string, PF_cvar_string, 448,
            "string(string cvarname)"}, // DP_QC_CVAR_STRING
        {"findflags", PF_findflags, PF_findflags, 449,
            "entity(entity start, .float fld, float match)"}, // DP_QC_FINDFLAGS
        {"findchainflags", PF_findchainflags, PF_findchainflags, 450,
            "entity(.float fld, float match)"}, // DP_QC_FINDCHAINFLAGS
        {"dropclient", PF_dropclient, PF_NoCSQC, 453,
            "void(entity player)"}, // DP_SV_BOTCLIENT
        {"spawnclient", PF_spawnclient, PF_NoCSQC, 454, "entity()",
            "Spawns a dummy player entity.\nNote that such dummy players "
            "will be carried from one map to the next.\nWarning: "
            "DP_SV_CLIENTCOLORS DP_SV_CLIENTNAME are not implemented in "
            "quakespasm, so use KRIMZON_SV_PARSECLIENTCOMMAND's "
            "clientcommand builtin to change the bot's "
            "name/colours/skin/team/etc, in the same way that clients "
            "would ask."}, // DP_SV_BOTCLIENT
        {"clienttype", PF_clienttype, PF_NoCSQC, 455,
            "float(entity client)"}, // botclient
        {"WriteUnterminatedString", PF_WriteString2, PF_NoCSQC, 456,
            "void(float target, string str)"}, // writestring but without
                                               // the null terminator. makes
                                               // things a little nicer.
        {"edict_num", PF_edict_for_num, PF_edict_for_num, 459,
            "entity(float entnum)"}, // DP_QC_EDICT_NUM
        {"buf_create", PF_buf_create, PF_buf_create, 460,
            "strbuf()"}, // DP_QC_STRINGBUFFERS
        {"buf_del", PF_buf_del, PF_buf_del, 461,
            "void(strbuf bufhandle)"}, // DP_QC_STRINGBUFFERS
        {"buf_getsize", PF_buf_getsize, PF_buf_getsize, 462,
            "float(strbuf bufhandle)"}, // DP_QC_STRINGBUFFERS
        {"buf_copy", PF_buf_copy, PF_buf_copy, 463,
            "void(strbuf bufhandle_from, strbuf bufhandle_to)"}, // DP_QC_STRINGBUFFERS
        {"buf_sort", PF_buf_sort, PF_buf_sort, 464,
            "void(strbuf bufhandle, float sortprefixlen, float backward)"}, // DP_QC_STRINGBUFFERS
        {"buf_implode", PF_buf_implode, PF_buf_implode, 465,
            "string(strbuf bufhandle, string glue)"}, // DP_QC_STRINGBUFFERS
        {"bufstr_get", PF_bufstr_get, PF_bufstr_get, 466,
            "string(strbuf bufhandle, float string_index)"}, // DP_QC_STRINGBUFFERS
        {"bufstr_set", PF_bufstr_set, PF_bufstr_set, 467,
            "void(strbuf bufhandle, float string_index, string str)"}, // DP_QC_STRINGBUFFERS
        {"bufstr_add", PF_bufstr_add, PF_bufstr_add, 468,
            "float(strbuf bufhandle, string str, float order)"}, // DP_QC_STRINGBUFFERS
        {"bufstr_free", PF_bufstr_free, PF_bufstr_free, 469,
            "void(strbuf bufhandle, float string_index)"}, // DP_QC_STRINGBUFFERS

        {"asin", PF_asin, PF_asin, 471,
            "float(float s)"}, // DP_QC_ASINACOSATANATAN2TAN
        {"acos", PF_acos, PF_acos, 472,
            "float(float c)"}, // DP_QC_ASINACOSATANATAN2TAN
        {"atan", PF_atan, PF_atan, 473,
            "float(float t)"}, // DP_QC_ASINACOSATANATAN2TAN
        {"atan2", PF_atan2, PF_atan2, 474,
            "float(float c, float s)"}, // DP_QC_ASINACOSATANATAN2TAN
        {"tan", PF_tan, PF_tan, 475,
            "float(float a)"}, // DP_QC_ASINACOSATANATAN2TAN
        {"strlennocol", PF_strlennocol, PF_strlennocol, 476,
            D("float(string s)",
                "Returns the number of characters in the string after any "
                "colour codes or other markup has been parsed.")}, // DP_QC_STRINGCOLORFUNCTIONS
        {"strdecolorize", PF_strdecolorize, PF_strdecolorize, 477,
            D("string(string s)",
                "Flattens any markup/colours, removing them from the "
                "string.")}, // DP_QC_STRINGCOLORFUNCTIONS
        {"strftime", PF_strftime, PF_strftime, 478,
            "string(float uselocaltime, string format, ...)"}, // DP_QC_STRFTIME
        {"tokenizebyseparator", PF_tokenizebyseparator, PF_tokenizebyseparator,
            479,
            "float(string s, string separator1, ...)"}, // DP_QC_TOKENIZEBYSEPARATOR
        {"strtolower", PF_strtolower, PF_strtolower, 480,
            "string(string s)"}, // DP_QC_STRING_CASE_FUNCTIONS
        {"strtoupper", PF_strtoupper, PF_strtoupper, 481,
            "string(string s)"}, // DP_QC_STRING_CASE_FUNCTIONS
        {"cvar_defstring", PF_cvar_defstring, PF_cvar_defstring, 482,
            "string(string s)"}, // DP_QC_CVAR_DEFSTRING
        {"pointsound", PF_sv_pointsound, PF_cl_pointsound, 483,
            "void(vector origin, string sample, float volume, float "
            "attenuation)"}, // DP_SV_POINTSOUND
        {"strreplace", PF_strreplace, PF_strreplace, 484,
            "string(string search, string replace, string subject)"}, // DP_QC_STRREPLACE
        {"strireplace", PF_strireplace, PF_strireplace, 485,
            "string(string search, string replace, string subject)"}, // DP_QC_STRREPLACE
        {"getsurfacepointattribute", PF_getsurfacepointattribute,
            PF_getsurfacepointattribute, 486,
            "vector(entity e, float s, float n, float a)"}, // DP_QC_GETSURFACEPOINTATTRIBUTE

        {"crc16", PF_crc16, PF_crc16, 494,
            "float(float caseinsensitive, string s, ...)"}, // DP_QC_CRC16
        {"cvar_type", PF_cvar_type, PF_cvar_type, 495,
            "float(string name)"}, // DP_QC_CVAR_TYPE
        {"numentityfields", PF_numentityfields, PF_numentityfields, 496,
            D("float()",
                "Gives the number of named entity fields. Note that this "
                "is not the size of an entity, but rather just the number "
                "of unique names (ie: vectors use 4 names rather than "
                "3).")}, // DP_QC_ENTITYDATA
        {"findentityfield", PF_findentityfield, PF_findentityfield, 0,
            D("float(string fieldname)", "Find a field index by name.")},
        {"entityfieldref", PF_entityfieldref, PF_entityfieldref, 0,
            D("typedef .__variant field_t;\nfield_t(float fieldnum)",
                "Returns a field value that can be directly used to read "
                "entity fields. Be sure to validate the type with "
                "entityfieldtype before using.")}, // DP_QC_ENTITYDATA
        {"entityfieldname", PF_entityfieldname, PF_entityfieldname, 497,
            D("string(float fieldnum)",
                "Retrieves the name of the given entity field.")}, // DP_QC_ENTITYDATA
        {"entityfieldtype", PF_entityfieldtype, PF_entityfieldtype, 498,
            D("float(float fieldnum)",
                "Provides information about the type of the field "
                "specified by the field num. Returns one of the EV_ "
                "values.")}, // DP_QC_ENTITYDATA
        {"getentityfieldstring", PF_getentfldstr, PF_getentfldstr, 499,
            "string(float fieldnum, entity ent)"}, // DP_QC_ENTITYDATA
        {"putentityfieldstring", PF_putentfldstr, PF_putentfldstr, 500,
            "float(float fieldnum, entity ent, string s)"}, // DP_QC_ENTITYDATA
        {"whichpack", PF_whichpack, PF_whichpack, 503,
            D("string(string filename, optional float makereferenced)",
                "Returns the pak file name that contains the file "
                "specified. progs/player.mdl will generally return "
                "something like 'pak0.pak'. If makereferenced is true, "
                "clients will automatically be told that the returned "
                "package should be pre-downloaded and used, even if "
                "allow_download_refpackages is not set.")}, // DP_QC_WHICHPACK
        {"uri_escape", PF_uri_escape, PF_uri_escape, 510,
            "string(string in)"}, // DP_QC_URI_ESCAPE
        {"uri_unescape", PF_uri_unescape, PF_uri_unescape, 511,
            "string(string in)"}, // DP_QC_URI_ESCAPE
        {"num_for_edict", PF_num_for_edict, PF_num_for_edict, 512,
            "float(entity ent)"}, // DP_QC_NUM_FOR_EDICT
        {"tokenize_console", PF_tokenize_console, PF_tokenize_console, 514,
            D("float(string str)",
                "Tokenize a string exactly as the console's tokenizer "
                "would do so. The regular tokenize builtin became "
                "bastardized for convienient string parsing, which "
                "resulted in a large disparity that can be exploited to "
                "bypass checks implemented in a naive "
                "SV_ParseClientCommand function, therefore you can use "
                "this builtin to make sure it exactly matches.")},
        {"argv_start_index", PF_argv_start_index, PF_argv_start_index, 515,
            D("float(float idx)",
                "Returns the character index that the tokenized arg "
                "started at.")},
        {"argv_end_index", PF_argv_end_index, PF_argv_end_index, 516,
            D("float(float idx)",
                "Returns the character index that the tokenized arg "
                "stopped at.")},
        //	{"buf_cvarlist",	PF_buf_cvarlist,	PF_buf_cvarlist,	517,
        //"void(strbuf strbuf, string pattern, string antipattern)"},
        {"cvar_description", PF_cvar_description, PF_cvar_description, 518,
            D("string(string cvarname)",
                "Retrieves the description of a cvar, which might be "
                "useful for tooltips or help files. This may still not be "
                "useful.")},
        {"gettime", PF_gettime, PF_gettime, 519,
            "float(optional float timetype)"},
        {"findkeysforcommand", PF_NoSSQC, PF_cl_findkeysforcommand, 521,
            D("string(string command, optional float bindmap)",
                "Returns a list of keycodes that perform the given console "
                "command in a format that can only be parsed via tokenize "
                "(NOT tokenize_console). This always returns at least two "
                "values - if only one key is actually bound, -1 will be "
                "returned. The bindmap argument is listed for "
                "compatibility with dp-specific defs, but is ignored in "
                "FTE.")},
        {"findkeysforcommandex", PF_NoSSQC, PF_cl_findkeysforcommandex, 0,
            D("string(string command, optional float bindmap)",
                "Returns a list of key bindings in keyname format instead "
                "of keynums. Use tokenize to parse. This list may contain "
                "modifiers. May return large numbers of keys.")},
        //	{"loadfromdata",	PF_loadfromdata,	PF_loadfromdata,	529,
        // D("void(string s)", "Reads a set of entities from the given
        // string. This string should have the same format as a .ent file or
        // a saved game. Entities will be spawned as required. If you need to
        // see the entities that were created, you should use parseentitydata
        // instead.")},
        //	{"loadfromfile",	PF_loadfromfile,	PF_loadfromfile,	530,
        // D("void(string s)", "Reads a set of entities from the named file.
        // This file should have the same format as a .ent file or a saved
        // game. Entities will be spawned as required. If you need to see the
        // entities that were created, you should use parseentitydata
        // instead.")},
        {"log", PF_Logarithm, PF_Logarithm, 532,
            D("float(float v, optional float base)",
                "Determines the logarithm of the input value according to "
                "the specified base. This can be used to calculate how "
                "much something was shifted by.")},
        {"soundlength", PF_NoSSQC, PF_cl_soundlength, 534,
            D("float(string sample)",
                "Provides a way to query the duration of a sound sample, "
                "allowing you to set up a timer to chain samples.")},
        {"buf_loadfile", PF_buf_loadfile, PF_buf_loadfile, 535,
            D("float(string filename, strbuf bufhandle)",
                "Appends the named file into a string buffer (which must "
                "have been created in advance). The return value merely "
                "says whether the file was readable.")},
        {"buf_writefile", PF_buf_writefile, PF_buf_writefile, 536,
            D("float(filestream filehandle, strbuf bufhandle, optional "
              "float startpos, optional float numstrings)",
                "Writes the contents of a string buffer onto the end of "
                "the supplied filehandle (you must have already used "
                "fopen). Additional optional arguments permit you to "
                "constrain the writes to a subsection of the "
                "stringbuffer.")},
        {"callfunction", PF_callfunction, PF_callfunction, 605,
            D("void(.../*, string funcname*/)",
                "Invokes the named function. The function name is always "
                "passed as the last parameter and must always be present. "
                "The others are passed to the named function as-is")},
        {"isfunction", PF_isfunction, PF_isfunction, 607,
            D("float(string s)",
                "Returns true if the named function exists and can be "
                "called with the callfunction builtin.")},
        {"parseentitydata", PF_parseentitydata, nullptr, 613,
            D("float(entity e, string s, optional float offset)",
                "Reads a single entity's fields into an already-spawned "
                "entity. s should contain field pairs like in a saved "
                "game: {\"foo1\" \"bar\" \"foo2\" \"5\"}. Returns <=0 on "
                "failure, otherwise returns the offset in the string that "
                "was read to.")},
        // {"generateentitydata",PF_generateentitydata,nullptr, 0,
        // D("string(entity e)", "Dumps the entities fields into a string
        // which can later be parsed with parseentitydata."}),
        {"sprintf", PF_sprintf, PF_sprintf, 627, "string(string fmt, ...)"},
        {"getsurfacenumtriangles", PF_getsurfacenumtriangles,
            PF_getsurfacenumtriangles, 628, "float(entity e, float s)"},
        {"getsurfacetriangle", PF_getsurfacetriangle, PF_getsurfacetriangle,
            629, "vector(entity e, float s, float n)"},
        {"setkeybind", PF_NoSSQC, PF_cl_setkeybind, 631,
            "float(float key, string bind, optional float bindmap)",
            "Changes a key binding."},
        {"getbindmaps", PF_NoSSQC, PF_cl_getbindmaps, 631, "vector()", "stub."},
        {"setbindmaps", PF_NoSSQC, PF_cl_setbindmaps, 632, "float(vector bm)",
            "stub."},
        //	{"digest_hex",		PF_digest_hex,		PF_digest_hex,		639,
        //"string(string digest, string data, ...)"},
};

static const char* extnames[] = {
    "DP_CON_SET",
    "DP_CON_SETA",
    "DP_EF_NOSHADOW",
    "DP_ENT_ALPHA", // already in quakespasm, supposedly.
    "DP_ENT_COLORMOD",
    "DP_ENT_SCALE",
    "DP_ENT_TRAILEFFECTNUM",
    //"DP_GFX_QUAKE3MODELTAGS", //we support attachments but no md3/iqm/tags, so
    // we can't really advertise this (although the builtin is complete if you
    // ignore the lack of md3/iqms/tags)
    "DP_INPUTBUTTONS",
    "DP_QC_AUTOCVARS", // they won't update on changes
    "DP_QC_ASINACOSATANATAN2TAN",
    "DP_QC_COPYENTITY",
    "DP_QC_CRC16",
    //"DP_QC_DIGEST",
    "DP_QC_CVAR_DEFSTRING",
    "DP_QC_CVAR_STRING",
    "DP_QC_CVAR_TYPE",
    "DP_QC_EDICT_NUM",
    "DP_QC_ENTITYDATA",
    "DP_QC_ETOS",
    "DP_QC_FINDCHAIN",
    "DP_QC_FINDCHAINFLAGS",
    "DP_QC_FINDCHAINFLOAT",
    "DP_QC_FINDFLAGS",
    "DP_QC_FINDFLOAT",
    "DP_QC_GETLIGHT",
    "DP_QC_GETSURFACE",
    "DP_QC_GETSURFACETRIANGLE",
    "DP_QC_GETSURFACEPOINTATTRIBUTE",
    "DP_QC_MINMAXBOUND",
    "DP_QC_MULTIPLETEMPSTRINGS",
    "DP_QC_RANDOMVEC",
    "DP_QC_SINCOSSQRTPOW",
    "DP_QC_SPRINTF",
    "DP_QC_STRFTIME",
    "DP_QC_STRING_CASE_FUNCTIONS",
    "DP_QC_STRINGBUFFERS",
    "DP_QC_STRINGCOLORFUNCTIONS",
    "DP_QC_STRREPLACE",
    "DP_QC_TOKENIZEBYSEPARATOR",
    "DP_QC_TRACEBOX",
    "DP_QC_TRACETOSS",
    "DP_QC_TRACE_MOVETYPES",
    "DP_QC_URI_ESCAPE",
    "DP_QC_VECTOANGLES_WITH_ROLL",
    "DP_QC_VECTORVECTORS",
    "DP_QC_WHICHPACK",
    "DP_VIEWZOOM",
    "DP_REGISTERCVAR",
    "DP_SV_BOTCLIENT",
    "DP_SV_DROPCLIENT",
    //	"DP_SV_POINTPARTICLES",	//can't enable this, because certain mods then
    // assume that we're DP and all the particles break.
    "DP_SV_POINTSOUND",
    "DP_SV_PRINT",
    "DP_SV_SETCOLOR",
    "DP_SV_SPAWNFUNC_PREFIX",
    "DP_SV_WRITEUNTERMINATEDSTRING",
//	"DP_TE_BLOOD",
#ifdef PSET_SCRIPT
    "DP_TE_PARTICLERAIN",
    "DP_TE_PARTICLESNOW",
#endif
    "DP_TE_STANDARDEFFECTBUILTINS",
    "EXT_BITSHIFT",
    "FRIK_FILE", // lacks the file part, but does have the strings part.
    "FTE_ENT_SKIN_CONTENTS", // SOLID_BSP&&skin==CONTENTS_FOO changes
                             // CONTENTS_SOLID to CONTENTS_FOO, allowing you to
                             // swim in moving ents without qc hacks, as well as
                             // correcting view cshifts etc.
#ifdef PSET_SCRIPT
    "FTE_PART_SCRIPT",
    "FTE_PART_NAMESPACES",
#ifdef PSET_SCRIPT_EFFECTINFO
    "FTE_PART_NAMESPACE_EFFECTINFO",
#endif
#endif
    "FTE_QC_CHECKCOMMAND",
    "FTE_QC_CROSSPRODUCT",
    "FTE_QC_INFOKEY",
    "FTE_QC_INTCONV",
    "FTE_QC_MULTICAST",
    "FTE_STRINGS",
#ifdef PSET_SCRIPT
    "FTE_SV_POINTPARTICLES",
#endif
    "KRIMZON_SV_PARSECLIENTCOMMAND",
    "ZQ_QC_STRINGS",

};

static void PF_checkextension()
{
    const char* extname = G_STRING(OFS_PARM0);
    unsigned int i;
    for(i = 0; i < sizeof(extnames) / sizeof(extnames[0]); i++)
    {
        if(!strcmp(extname, extnames[i]))
        {
            if(!pr_checkextension.value)
            {
                Con_Printf("Mod found extension %s\n", extname);
            }
            G_FLOAT(OFS_RETURN) = true;
            return;
        }
    }
    if(!pr_checkextension.value)
    {
        Con_DPrintf("Mod tried extension %s\n", extname);
    }
    G_FLOAT(OFS_RETURN) = false;
}

static void PF_builtinsupported()
{
    const char* biname = G_STRING(OFS_PARM0);
    unsigned int i;
    for(i = 0; i < sizeof(extensionbuiltins) / sizeof(extensionbuiltins[0]);
        i++)
    {
        if(!strcmp(extensionbuiltins[i].name, biname))
        {
            G_FLOAT(OFS_RETURN) = extensionbuiltins[i].number;
            return;
        }
    }
    G_FLOAT(OFS_RETURN) = 0;
}


static void PF_checkbuiltin()
{
    func_t funcref = G_INT(OFS_PARM0);
    if((unsigned int)funcref < (unsigned int)qcvm->progs->numfunctions)
    {
        dfunction_t* fnc = &qcvm->functions[(unsigned int)funcref];
        //		const char *funcname = PR_GetString(fnc->s_name);
        int binum = -fnc->first_statement;
        unsigned int i;

        // qc defines the function at least. nothing weird there...
        if(binum > 0 && binum < qcvm->numbuiltins)
        {
            if(qcvm->builtins[binum] == PF_Fixme)
            {
                G_FLOAT(OFS_RETURN) =
                    false; // the builtin with that number isn't defined.
                for(i = 0; i < sizeof(extensionbuiltins) /
                                   sizeof(extensionbuiltins[0]);
                    i++)
                {
                    if(extensionbuiltins[i].number == binum)
                    { // but it will be defined if its actually executed.
                        if(extensionbuiltins[i].desc &&
                            !strncmp(extensionbuiltins[i].desc, "stub.", 5))
                        {
                            G_FLOAT(OFS_RETURN) =
                                false; // pretend it won't work if it probably
                                       // won't be useful
                        }
                        else
                        {
                            G_FLOAT(OFS_RETURN) = true;
                        }
                        break;
                    }
                }
            }
            else
            {
                G_FLOAT(OFS_RETURN) =
                    true; // its defined, within the sane range, mapped,
                          // everything. all looks good.
                // we should probably go through the available builtins and
                // validate that the qc's name matches what would be expected
                // this is really intended more for builtins defined as #0
                // though, in such cases, mismatched assumptions are impossible.
            }
        }
        else
        {
            G_FLOAT(OFS_RETURN) =
                false; // not a valid builtin (#0 builtins get remapped at load,
        }
        // even if the builtin is activated then)
    }
    else
    { // not valid somehow.
        G_FLOAT(OFS_RETURN) = false;
    }
}

void PF_Fixme()
{
    // interrogate the vm to try to figure out exactly which builtin they just
    // tried to execute.
    dstatement_t* st = &qcvm->statements[qcvm->xstatement];
    eval_t* glob = (eval_t*)&qcvm->globals[st->a];
    if((unsigned int)glob->function < (unsigned int)qcvm->progs->numfunctions)
    {
        dfunction_t* fnc = &qcvm->functions[(unsigned int)glob->function];
        const char* funcname = PR_GetString(fnc->s_name);
        int binum = -fnc->first_statement;
        unsigned int i;
        if(binum >= 0)
        {
            // find an extension with the matching number
            for(i = 0;
                i < sizeof(extensionbuiltins) / sizeof(extensionbuiltins[0]);
                i++)
            {
                if(extensionbuiltins[i].number == binum)
                { // set it up so we're faster next time
                    builtin_t bi = nullptr;
                    if(qcvm == &sv.qcvm)
                    {
                        bi = extensionbuiltins[i].ssqcfunc;
                    }
                    else if(qcvm == &cl.qcvm)
                    {
                        bi = extensionbuiltins[i].csqcfunc;
                    }
                    if(!bi)
                    {
                        continue;
                    }

                    if(!pr_checkextension.value ||
                        (extensionbuiltins[i].desc &&
                            !strncmp(extensionbuiltins[i].desc, "stub.", 5)))
                    {
                        Con_Warning("Mod is using builtin #%u - %s\n",
                            extensionbuiltins[i].documentednumber,
                            extensionbuiltins[i].name);
                    }
                    else
                    {
                        Con_DPrintf2("Mod uses builtin #%u - %s\n",
                            extensionbuiltins[i].documentednumber,
                            extensionbuiltins[i].name);
                    }
                    qcvm->builtins[binum] = bi;
                    qcvm->builtins[binum]();
                    return;
                }
            }

            PR_RunError("unimplemented builtin #%i - %s", binum, funcname);
        }
    }
    PR_RunError("PF_Fixme: not a builtin...");
}


// called at map end
void PR_ShutdownExtensions()
{
    PR_UnzoneAll();
    PF_frikfile_shutdown();
    PF_search_shutdown();
    PF_buf_shutdown();
    tokenize_flush();
    if(qcvm == &cl.qcvm)
    {
        PR_ReloadPics(true);
    }

    pr_ext_warned_particleeffectnum = 0;
}

func_t PR_FindExtFunction(const char* entryname)
{ // depends on 0 being an invalid function,
    dfunction_t* func = ED_FindFunction(entryname);
    if(func)
    {
        return func - qcvm->functions;
    }
    return 0;
}
static void* PR_FindExtGlobal(int type, const char* name)
{
    ddef_t* def = ED_FindGlobal(name);
    if(def && (def->type & ~DEF_SAVEGLOBAL) == type &&
        def->ofs < qcvm->progs->numglobals)
    {
        return qcvm->globals + def->ofs;
    }
    return nullptr;
}

void PR_AutoCvarChanged(cvar_t* var)
{
    char* n;
    ddef_t* glob;
    qcvm_t* oldqcvm = qcvm;
    PR_SwitchQCVM(nullptr);

    if(sv.active)
    {
        QCVMGuard qg{&sv.qcvm};

        n = va("autocvar_%s", var->name);
        glob = ED_FindGlobal(n);
        if(glob)
        {
            if(!ED_ParseEpair((void*)qcvm->globals, glob, var->string))
            {
                Con_Warning("EXT: Unable to configure %s\n", n);
            }
        }
    }

    if(cl.qcvm.globals)
    {
        PR_SwitchQCVM(nullptr);

        QCVMGuard qg{&cl.qcvm};

        n = va("autocvar_%s", var->name);
        glob = ED_FindGlobal(n);
        if(glob)
        {
            if(!ED_ParseEpair((void*)qcvm->globals, glob, var->string))
            {
                Con_Warning("EXT: Unable to configure %s\n", n);
            }
        }
    }

    PR_SwitchQCVM(oldqcvm);
}

void PR_InitExtensions()
{
    size_t i, j;
    // this only needs to be done once. because we're evil.
    // it should help slightly with the 'documentation' above at least.
    j = sizeof(qcvm->builtins) / sizeof(qcvm->builtins[0]);
    for(i = 1; i < sizeof(extensionbuiltins) / sizeof(extensionbuiltins[0]);
        i++)
    {
        if(extensionbuiltins[i].documentednumber)
        {
            extensionbuiltins[i].number = extensionbuiltins[i].documentednumber;
        }
        else
        {
            extensionbuiltins[i].number = --j;
        }
    }
}

// called at map start
void PR_EnableExtensions(ddef_t* pr_globaldefs)
{
    unsigned int i, j;
    unsigned int numautocvars = 0;

    for(i = qcvm->numbuiltins; i < countof(qcvm->builtins); i++)
    {
        qcvm->builtins[i] = PF_Fixme;
    }
    qcvm->numbuiltins = i;
    if(!pr_checkextension.value)
    {
        Con_DPrintf("not enabling qc extensions\n");
        return;
    }

    // TODO VR: (P0) crashes on safeAtan2 assertion:
    // qcvm->builtins[51] = PF_ext_vectoangles; // swap it with a two-arg
    // version.

    qcvm->extfuncs.SV_ParseClientCommand =
        PR_FindExtFunction("SV_ParseClientCommand");
    qcvm->extfuncs.EndFrame = PR_FindExtFunction("EndFrame");

    qcvm->extfuncs.CSQC_Init = PR_FindExtFunction("CSQC_Init");
    qcvm->extfuncs.CSQC_DrawHud = PR_FindExtFunction("CSQC_DrawHud");
    qcvm->extfuncs.CSQC_DrawScores = PR_FindExtFunction("CSQC_DrawScores");
    qcvm->extfuncs.CSQC_InputEvent = PR_FindExtFunction("CSQC_InputEvent");
    qcvm->extfuncs.CSQC_ConsoleCommand =
        PR_FindExtFunction("CSQC_ConsoleCommand");
    qcvm->extfuncs.CSQC_Parse_Event = PR_FindExtFunction("CSQC_Parse_Event");
    qcvm->extfuncs.CSQC_Parse_Damage = PR_FindExtFunction("CSQC_Parse_Damage");
    qcvm->extglobals.cltime = (float*)PR_FindExtGlobal(ev_float, "cltime");
    qcvm->extglobals.maxclients =
        (float*)PR_FindExtGlobal(ev_float, "maxclients");
    qcvm->extglobals.intermission =
        (float*)PR_FindExtGlobal(ev_float, "intermission");
    qcvm->extglobals.intermission_time =
        (float*)PR_FindExtGlobal(ev_float, "intermission_time");
    qcvm->extglobals.player_localnum =
        (float*)PR_FindExtGlobal(ev_float, "player_localnum");
    qcvm->extglobals.player_localentnum =
        (float*)PR_FindExtGlobal(ev_float, "player_localentnum");

    // any #0 functions are remapped to their builtins here, so we don't have to
    // tweak the VM in an obscure potentially-breaking way.
    for(i = 0; i < (unsigned int)qcvm->progs->numfunctions; i++)
    {
        if(qcvm->functions[i].first_statement == 0 &&
            qcvm->functions[i].s_name && !qcvm->functions[i].parm_start &&
            !qcvm->functions[i].locals)
        {
            const char* name = PR_GetString(qcvm->functions[i].s_name);
            for(j = 0;
                j < sizeof(extensionbuiltins) / sizeof(extensionbuiltins[0]);
                j++)
            {
                if(!strcmp(extensionbuiltins[j].name, name))
                { // okay, map it
                    qcvm->functions[i].first_statement =
                        -extensionbuiltins[j].number;
                    break;
                }
            }
        }
    }

    // autocvars
    for(i = 0; i < (unsigned int)qcvm->progs->numglobaldefs; i++)
    {
        const char* n = PR_GetString(qcvm->globaldefs[i].s_name);
        if(!strncmp(n, "autocvar_", 9))
        {
            // really crappy approach
            cvar_t* var = Cvar_Create(
                n + 9, PR_UglyValueString(qcvm->globaldefs[i].type,
                           (eval_t*)(qcvm->globals + qcvm->globaldefs[i].ofs)));
            numautocvars++;
            if(!var)
            {
                continue; // name conflicts with a command?
            }

            if(!ED_ParseEpair(
                   (void*)qcvm->globals, &pr_globaldefs[i], var->string))
            {
                Con_Warning("EXT: Unable to configure %s\n", n);
            }
            var->flags |= CVAR_AUTOCVAR;
        }
    }
    if(numautocvars)
    {
        Con_DPrintf2("Found %i autocvars\n", numautocvars);
    }
}

void PR_DumpPlatform_f()
{
    char name[MAX_OSPATH];
    FILE* f;
    const char* outname = nullptr;
    unsigned int i, j;
    int targs = 0;
    for(i = 1; i < (unsigned)Cmd_Argc();)
    {
        const char* arg = Cmd_Argv(i++);
        if(!strcmp(arg, "-O"))
        {
            if(arg[2])
            {
                outname = arg + 2;
            }
            else
            {
                outname = Cmd_Argv(i++);
            }
        }
        else if(!q_strcasecmp(arg, "-Tcs"))
        {
            targs |= 2;
        }
        else if(!q_strcasecmp(arg, "-Tss"))
        {
            targs |= 1;
        }
        else
        {
            Con_Printf("%s: Unknown argument\n", Cmd_Argv(0));
            return;
        }
    }
    if(!outname)
    {
        outname = ((targs == 2) ? "qscsextensions" : "qsextensions");
    }
    if(!targs)
    {
        targs = 3;
    }

    if(strstr(outname, ".."))
    {
        return;
    }
    q_snprintf(name, sizeof(name), "%s/src/%s", com_gamedir, outname);
    COM_AddExtension(name, ".qc", sizeof(name));

    f = fopen(name, "w");
    if(!f)
    {
        Con_Printf("%s: Couldn't write %s\n", Cmd_Argv(0), name);
        return;
    }
    Con_Printf("%s: Writing %s\n", Cmd_Argv(0), name);

    fprintf(f,
        "/*\n"
        "Extensions file for " ENGINE_NAME_AND_VER
        "\n"
        "This file is auto-generated by %s %s.\n"
        "You will probably need to use FTEQCC to compile this.\n"
        "*/\n",
        Cmd_Argv(0), Cmd_Args() ? Cmd_Args() : "with no args");

    fprintf(f,
        "\n\n//This file only supports csqc, so including this file in some "
        "other situation is a user error\n"
        "#if defined(QUAKEWORLD) || defined(MENU)\n"
        "#error Mixed up module defs\n"
        "#endif\n"
        "#ifndef CSQC\n"
        "#define CSQC\n"
        "#endif\n"
        "#ifndef CSQC_SIMPLE\n" // quakespasm's csqc implementation is
                                // simplified, and can do huds+menus, but that's
                                // about it.
        "#define CSQC_SIMPLE\n"
        "#endif\n");

    fprintf(f, "\n\n//List of advertised extensions\n");
    for(i = 0; i < sizeof(extnames) / sizeof(extnames[0]); i++)
    {
        fprintf(f, "//%s\n", extnames[i]);
    }

    fprintf(f,
        "\n\n//Explicitly flag this stuff as probably-not-referenced, meaning "
        "fteqcc will shut up about it and silently strip what it can.\n");
    fprintf(f, "#pragma noref 1\n");

    if(targs & 2)
    { // I really hope that fteqcc's unused variable logic is up to scratch
        fprintf(f, "entity		self,other,world;\n");
        fprintf(f, "float		time,frametime,force_retouch;\n");
        fprintf(f, "string		mapname;\n");
        fprintf(f,
            "float		"
            "deathmatch,coop,teamplay,serverflags,total_secrets,total_monsters,"
            "found_secrets,killed_monsters,parm1, parm2, parm3, parm4, parm5, "
            "parm6, parm7, parm8, parm9, parm10, parm11, parm12, parm13, "
            "parm14, parm15, parm16;\n");
        fprintf(f, "vector		v_forward, v_up, v_right;\n");
        fprintf(
            f, "float		trace_allsolid,trace_startsolid,trace_fraction;\n");
        fprintf(f, "vector		trace_endpos,trace_plane_normal;\n");
        fprintf(f, "float		trace_plane_dist;\n");
        fprintf(f, "entity		trace_ent;\n");
        fprintf(f, "float		trace_inopen,trace_inwater;\n");
        fprintf(f, "entity		msg_entity;\n");
        fprintf(f,
            "void() 		"
            "main,StartFrame,PlayerPreThink,PlayerPostThink,ClientKill,"
            "ClientConnect,PutClientInServer,ClientDisconnect,SetNewParms,"
            "SetChangeParms;\n");
        fprintf(f, "void		end_sys_globals;\n");
        fprintf(f, ".float		modelindex;\n");
        fprintf(f, ".vector		absmin, absmax;\n");
        fprintf(f, ".float		ltime,movetype,solid;\n");
        fprintf(f,
            ".vector		"
            "origin,oldorigin,velocity,angles,avelocity,punchangle;\n");
        fprintf(f, ".string		classname,model;\n");
        fprintf(f, ".float		frame,skin,effects;\n");
        fprintf(f, ".vector		mins, maxs,size;\n");
        fprintf(f, ".void()		touch,use,think,blocked;\n");
        fprintf(f, ".float		nextthink;\n");
        fprintf(f, ".entity		groundentity;\n");
        fprintf(f, ".float		health,frags,weapon;\n");
        fprintf(f, ".string		weaponmodel;\n");
        fprintf(f,
            ".float		"
            "weaponframe,currentammo,ammo_shells,ammo_nails,ammo_rockets,ammo_"
            "cells,items,takedamage;\n");
        fprintf(f, ".entity		chain;\n");
        fprintf(f, ".float		deadflag;\n");
        fprintf(f, ".vector		view_ofs;\n");
        fprintf(f, ".float		button0,button1,button2,impulse,fixangle;\n");
        fprintf(f, ".vector		v_angle;\n");
        fprintf(f, ".float		idealpitch;\n");
        fprintf(f, ".string		netname;\n");
        fprintf(f, ".entity 	enemy;\n");
        fprintf(f,
            ".float		"
            "flags,colormap,team,max_health,teleport_time,armortype,armorvalue,"
            "waterlevel,watertype,ideal_yaw,yaw_speed;\n");
        fprintf(f, ".entity		aiment,goalentity;\n");
        fprintf(f, ".float		spawnflags;\n");
        fprintf(f, ".string		target,targetname;\n");
        fprintf(f, ".float		dmg_take,dmg_save;\n");
        fprintf(f, ".entity		dmg_inflictor,owner;\n");
        fprintf(f, ".vector		movedir;\n");
        fprintf(f, ".string		message;\n");
        fprintf(f, ".float		sounds;\n");
        fprintf(f, ".string		noise, noise1, noise2, noise3;\n");
        fprintf(f, "void		end_sys_fields;\n");
    }

    fprintf(f,
        "\n\n//Some custom types (that might be redefined as accessors by "
        "fteextensions.qc, although we don't define any methods here)\n");
    fprintf(f, "#ifdef _ACCESSORS\n");
    fprintf(f, "accessor strbuf:float;\n");
    fprintf(f, "accessor searchhandle:float;\n");
    fprintf(f, "accessor hashtable:float;\n");
    fprintf(f, "accessor infostring:string;\n");
    fprintf(f, "accessor filestream:float;\n");
    fprintf(f, "#else\n");
    fprintf(f, "#define strbuf float\n");
    fprintf(f, "#define searchhandle float\n");
    fprintf(f, "#define hashtable float\n");
    fprintf(f, "#define infostring string\n");
    fprintf(f, "#define filestream float\n");
    fprintf(f, "#endif\n");


    if(targs & 1)
    {
        fprintf(f, "void(string cmd) SV_ParseClientCommand;\n");
        fprintf(f, "void() EndFrame;\n");
    }

    if(targs & 2)
    {
        fprintf(f,
            "void(float apilevel, string enginename, float engineversion) "
            "CSQC_Init;\n");
        fprintf(f, "float(string cmdstr) CSQC_ConsoleCommand;\n");
        fprintf(f, "void(vector virtsize, float showscores) CSQC_DrawHud;\n");
        fprintf(
            f, "void(vector virtsize, float showscores) CSQC_DrawScores;\n");
        fprintf(f,
            "float(float evtype, float scanx, float chary, float devid) "
            "CSQC_InputEvent;\n");
        fprintf(f, "void() CSQC_Parse_Event;\n");
        fprintf(f,
            "float(float save, float take, vector dir) CSQC_Parse_Damage;\n");
    }

    if(targs & 2)
    {
        fprintf(f,
            "float cltime;				/* increases regardless of pause state "
            "or game speed */\n");
        fprintf(f,
            "float maxclients;			/* maximum number of players possible "
            "on this server */\n");
        fprintf(f, "float intermission;			/* in intermission */\n");
        fprintf(f,
            "float intermission_time;	/* when the intermission started */\n");
        fprintf(f,
            "float player_localnum;		/* the player slot that is believed to "
            "be assigned to us*/\n");
        fprintf(f,
            "float player_localentnum;	/* the entity number that the view is "
            "attached to */\n");
    }

    fprintf(f, "const float FALSE		= 0;\n");
    fprintf(f, "const float TRUE		= 1;\n");

    if(targs & 2)
    {
        fprintf(f, "const float IE_KEYDOWN		= %i;\n", CSIE_KEYDOWN);
        fprintf(f, "const float IE_KEYUP		= %i;\n", CSIE_KEYUP);
        fprintf(f, "const float IE_MOUSEDELTA	= %i;\n", CSIE_MOUSEDELTA);
        fprintf(f, "const float IE_MOUSEABS		= %i;\n", CSIE_MOUSEABS);
        fprintf(f, "const float IE_JOYAXIS		= %i;\n", CSIE_JOYAXIS);

        fprintf(
            f, "const float STAT_HEALTH = 0;		/* Player's health. */\n");
        //		fprintf(f, "const float STAT_FRAGS = 1;			/* unused
        //*/\n");
        fprintf(f,
            "const float STAT_WEAPONMODELI = 2;	/* This is the modelindex of "
            "the current viewmodel (renamed from the original name "
            "'STAT_WEAPON' due to confusions). */\n");
        fprintf(f,
            "const float STAT_AMMO = 3;			/* player.currentammo */\n");
        fprintf(f, "const float STAT_ARMOR = 4;\n");
        fprintf(f, "const float STAT_WEAPONFRAME = 5;\n");
        fprintf(f, "const float STAT_SHELLS = 6;\n");
        fprintf(f, "const float STAT_NAILS = 7;\n");
        fprintf(f, "const float STAT_ROCKETS = 8;\n");
        fprintf(f, "const float STAT_CELLS = 9;\n");
        fprintf(f, "const float STAT_ACTIVEWEAPON = 10;	/* player.weapon */\n");
        fprintf(f, "const float STAT_TOTALSECRETS = 11;\n");
        fprintf(f, "const float STAT_TOTALMONSTERS = 12;\n");
        fprintf(f, "const float STAT_FOUNDSECRETS = 13;\n");
        fprintf(f, "const float STAT_KILLEDMONSTERS = 14;\n");
        fprintf(f,
            "const float STAT_ITEMS = 15;		/* self.items | "
            "(self.items2<<23). In order to decode this stat properly, you "
            "need to use getstatbits(STAT_ITEMS,0,23) to read self.items, and "
            "getstatbits(STAT_ITEMS,23,11) to read self.items2 or "
            "getstatbits(STAT_ITEMS,28,4) to read the visible part of "
            "serverflags, whichever is applicable. */\n");
        fprintf(
            f, "const float STAT_VIEWHEIGHT = 16;	/* player.view_ofs_z */\n");
        //		fprintf(f, "const float STAT_TIME = 17;\n");
        //		fprintf(f, "//const float STAT_MATCHSTARTTIME = 18;\n");
        //		fprintf(f, "const float STAT_UNUSED = 19;\n");
        fprintf(f,
            "const float STAT_VIEW2 = 20;		/* This stat contains the "
            "number of the entity in the server's .view2 field. */\n");
        fprintf(f,
            "const float STAT_VIEWZOOM = 21;		/* Scales fov and "
            "sensitiity. Part of DP_VIEWZOOM. */\n");
        //		fprintf(f, "const float STAT_UNUSED = 22;\n");
        //		fprintf(f, "const float STAT_UNUSED = 23;\n");
        //		fprintf(f, "const float STAT_UNUSED = 24;\n");
        fprintf(f, "const float STAT_IDEALPITCH = 25;\n");
        fprintf(f, "const float STAT_PUNCHANGLE_X = 26;\n");
        fprintf(f, "const float STAT_PUNCHANGLE_Y = 27;\n");
        fprintf(f, "const float STAT_PUNCHANGLE_Y = 28;\n");
        //		fprintf(f, "const float STAT_PUNCHVECTOR_X = 29;\n");
        //		fprintf(f, "const float STAT_PUNCHVECTOR_Y = 30;\n");
        //		fprintf(f, "const float STAT_PUNCHVECTOR_Z = 31;\n");
    }
    fprintf(f,
        "const float STAT_USER = 32;			/* Custom user stats start "
        "here (lower values are reserved for engine use). */\n");
    // these can be used for both custom stats and for reflection
    fprintf(f, "const float EV_VOID = %i;\n", ev_void);
    fprintf(f, "const float EV_STRING = %i;\n", ev_string);
    fprintf(f, "const float EV_FLOAT = %i;\n", ev_float);
    fprintf(f, "const float EV_VECTOR = %i;\n", ev_vector);
    fprintf(f, "const float EV_ENTITY = %i;\n", ev_entity);
    fprintf(f, "const float EV_FIELD = %i;\n", ev_field);
    fprintf(f, "const float EV_FUNCTION = %i;\n", ev_function);
    fprintf(f, "const float EV_POINTER = %i;\n", ev_pointer);
    fprintf(f, "const float EV_INTEGER = %i;\n", ev_ext_integer);

    if(targs & 1)
    {
        // extra fields
        fprintf(f, "\n\n//Supported Extension fields\n");
        fprintf(f, ".float gravity;\n"); // used by hipnotic
        fprintf(f,
            "//.float items2;			/*if defined, overrides serverflags "
            "for displaying runes on the hud*/\n"); // used by both mission
                                                    // packs. *REPLACES*
                                                    // serverflags if defined,
                                                    // so lets try not to define
                                                    // it.
        fprintf(f,
            ".float traileffectnum;		/*can also be set with 'traileffect' "
            "from a map editor*/\n");
        fprintf(f,
            ".float emiteffectnum;		/*can also be set with 'traileffect' "
            "from a map editor*/\n");
        fprintf(f,
            ".vector movement;			/*describes which forward/right/up "
            "keys the player is holidng*/\n");
        fprintf(f,
            ".entity viewmodelforclient;	/*attaches this entity to the "
            "specified player's view. invisible to other players*/\n");
        fprintf(f,
            ".entity exteriormodeltoclient;/*hides the entity in the specified "
            "player's main view. it will remain visible in mirrors etc.*/\n");
        fprintf(f, ".float scale;				/*rescales the etntiy*/\n");
        fprintf(f,
            ".float alpha;				/*entity opacity*/\n"); // entity alpha.
                                                                // woot.
        fprintf(
            f, ".vector colormod;			/*tints the entity's colours*/\n");
        fprintf(f, ".entity tag_entity;\n");
        fprintf(f, ".float tag_index;\n");
        fprintf(f, ".float button3;\n");
        fprintf(f, ".float button4;\n");
        fprintf(f, ".float button5;\n");
        fprintf(f, ".float button6;\n");
        fprintf(f, ".float button7;\n");
        fprintf(f, ".float button8;\n");
        fprintf(f, ".float viewzoom;			/*rescales the user's fov*/\n");
        fprintf(f,
            ".float modelflags;			/*provides additional modelflags to "
            "use (effects&EF_NOMODELFLAGS to replace the original "
            "model's)*/\n");

        // extra constants
        fprintf(f, "\n\n//Supported Extension Constants\n");
        fprintf(f, "const float MOVETYPE_FOLLOW	= " STRINGIFY(
                       MOVETYPE_EXT_FOLLOW) ";\n");
        fprintf(f,
            "const float SOLID_CORPSE	= " STRINGIFY(SOLID_EXT_CORPSE) ";\n");

        fprintf(f, "const float CLIENTTYPE_DISCONNECT	= " STRINGIFY(0) ";\n");
        fprintf(f, "const float CLIENTTYPE_REAL			= " STRINGIFY(1) ";\n");
        fprintf(f, "const float CLIENTTYPE_BOT			= " STRINGIFY(2) ";\n");
        fprintf(f, "const float CLIENTTYPE_NOTCLIENT	= " STRINGIFY(3) ";\n");

        fprintf(f, "const float EF_NOSHADOW			= %#x;\n", EF_NOSHADOW);
        fprintf(f,
            "const float EF_NOMODELFLAGS		= %#x; /*the standard "
            "modelflags from the model are ignored*/\n",
            EF_NOMODELFLAGS);

        fprintf(f, "const float MF_ROCKET			= %#x;\n", EF_ROCKET);
        fprintf(f, "const float MF_GRENADE			= %#x;\n", EF_GRENADE);
        fprintf(f, "const float MF_GIB				= %#x;\n", EF_GIB);
        fprintf(f, "const float MF_ROTATE			= %#x;\n", EF_ROTATE);
        fprintf(f, "const float MF_TRACER			= %#x;\n", EF_TRACER);
        fprintf(f, "const float MF_ZOMGIB			= %#x;\n", EF_ZOMGIB);
        fprintf(f, "const float MF_TRACER2			= %#x;\n", EF_TRACER2);
        fprintf(f, "const float MF_TRACER3			= %#x;\n", EF_TRACER3);

        fprintf(f, "const float MSG_MULTICAST	= %i;\n", 4);
        fprintf(f, "const float MULTICAST_ALL	= %i;\n", MULTICAST_ALL_U);
        //	fprintf(f, "const float MULTICAST_PHS	= %i;\n", MULTICAST_PHS_U);
        fprintf(f, "const float MULTICAST_PVS	= %i;\n", MULTICAST_PVS_U);
        fprintf(f, "const float MULTICAST_ONE	= %i;\n", MULTICAST_ONE_U);
        fprintf(f, "const float MULTICAST_ALL_R	= %i;\n", MULTICAST_ALL_R);
        //	fprintf(f, "const float MULTICAST_PHS_R	= %i;\n", MULTICAST_PHS_R);
        fprintf(f, "const float MULTICAST_PVS_R	= %i;\n", MULTICAST_PVS_R);
        fprintf(f, "const float MULTICAST_ONE_R	= %i;\n", MULTICAST_ONE_R);
        fprintf(f, "const float MULTICAST_INIT	= %i;\n", MULTICAST_INIT);
    }

    fprintf(f, "const float FILE_READ		= " STRINGIFY(0) ";\n");
    fprintf(f, "const float FILE_APPEND		= " STRINGIFY(1) ";\n");
    fprintf(f, "const float FILE_WRITE		= " STRINGIFY(2) ";\n");

    if(targs & 2)
    {
        fprintf(f,
            "\n\n//Vanilla Builtin list (reduced, so as to avoid conflicts)\n");
        fprintf(f, "void(vector) makevectors = #1;\n");
        fprintf(f, "void(entity,vector) setorigin = #2;\n");
        fprintf(f, "void(entity,string) setmodel = #3;\n");
        fprintf(f, "void(entity,vector,vector) setsize = #4;\n");
        fprintf(f, "float() random = #7;\n");
        // sound = #8
        fprintf(f, "vector(vector) normalize = #9;\n");
        fprintf(f, "void(string e) error = #10;\n");
        fprintf(f, "void(string n) objerror = #11;\n");
        fprintf(f, "float(vector) vlen = #12;\n");
        fprintf(f, "entity() spawn = #14;\n");
        fprintf(f, "void(entity e) remove = #15;\n");
        fprintf(f, "void(string,...) dprint = #25;\n");
        fprintf(f, "string(float) ftos = #26;\n");
        fprintf(f, "string(vector) vtos = #27;\n");
        fprintf(f, "float(float n) rint = #36;\n");
        fprintf(f, "float(float n) floor = #37;\n");
        fprintf(f, "float(float n) ceil = #38;\n");
        fprintf(f, "float(float n) fabs = #43;\n");
        fprintf(f, "float(string) cvar = #45;\n");
        fprintf(f, "void(string,...) localcmd = #46;\n");
        fprintf(f, "entity(entity) nextent = #47;\n");
        fprintf(f, "void(string var, string val) cvar_set = #72;\n");
    }

    for(j = 0; j < 2; j++)
    {
        if(j)
        {
            fprintf(f,
                "\n\n//Builtin Stubs List (these are present for simpler "
                "compatibility, but not properly supported in QuakeSpasm at "
                "this time).\n/*\n");
        }
        else
        {
            fprintf(f, "\n\n//Builtin list\n");
        }
        for(i = 0; i < sizeof(extensionbuiltins) / sizeof(extensionbuiltins[0]);
            i++)
        {
            if((targs & 2) && extensionbuiltins[i].csqcfunc)
            {
                ;
            }
            else if((targs & 1) && extensionbuiltins[i].ssqcfunc)
            {
                ;
            }
            else
            {
                continue;
            }

            if(j != (extensionbuiltins[i].desc
                            ? !strncmp(extensionbuiltins[i].desc, "stub.", 5)
                            : 0))
            {
                continue;
            }
            fprintf(f, "%s %s = #%i;", extensionbuiltins[i].typestr,
                extensionbuiltins[i].name,
                extensionbuiltins[i].documentednumber);
            if(extensionbuiltins[i].desc && !j)
            {
                const char* line = extensionbuiltins[i].desc;
                const char* term;
                fprintf(f, " /*");
                while(*line)
                {
                    fprintf(f, "\n\t\t");
                    term = line;
                    while(*term && *term != '\n')
                    {
                        term++;
                    }
                    fwrite(line, 1, term - line, f);
                    if(*term == '\n')
                    {
                        term++;
                    }
                    line = term;
                }
                fprintf(f, " */\n\n");
            }
            else
            {
                fprintf(f, "\n");
            }
        }
        if(j)
        {
            fprintf(f, "*/\n");
        }
    }

    if(targs & 2)
    {
        for(i = 0; i < MAX_KEYS; i++)
        {
            const char* k = Key_KeynumToString(i);
            if(!k[0] || !k[1] || k[0] == '<')
            {
                continue; // skip simple keynames that can be swapped with 'k',
            }
            // and <invalid> keys.
            fprintf(f, "const float K_%s = %i;\n", k, Key_NativeToQC(i));
        }
    }


    fprintf(f, "\n\n//Reset this back to normal.\n");
    fprintf(f, "#pragma noref 0\n");
    fclose(f);
}
