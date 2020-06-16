/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2020-2020 Vittorio Romeo

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

#pragma once

#include "quakeglm_qvec3.hpp"
#include "quakeglm_qvec4.hpp"
#include "bspfile.hpp"
#include "gl_model.hpp"
#include "cvar.hpp"
#include "vid.hpp"
#include "refdef.hpp"
#include "srcformat.hpp"

#include <cstdint>

void GL_BeginRendering(int* x, int* y, int* width, int* height);
void GL_EndRendering();
void GL_Set2D();

extern int glx, gly, glwidth, glheight;

#define GL_UNUSED_TEXTURE (~(GLuint)0)

// r_local.h -- private refresh defs

#define ALIAS_BASE_SIZE_RATIO (1.0 / 11.0)
// normalizing factor so player model works out to about
//  1 pixel per triangle
#define MAX_LBM_HEIGHT 480

#define TILE_SIZE 128 // size of textures generated by R_GenTiledSurf

#define SKYSHIFT 7
#define SKYSIZE (1 << SKYSHIFT)
#define SKYMASK (SKYSIZE - 1)

#define BACKFACE_EPSILON 0.01


void R_TimeRefresh_f();
void R_ReadPointFile_f();
texture_t* R_TextureAnimation(texture_t* base, int frame);

struct texture_t;

typedef struct surfcache_s
{
    struct surfcache_s* next;
    struct surfcache_s** owner; // nullptr is an empty chunk of memory
    int lightadj[MAXLIGHTMAPS]; // checked for strobe flush
    int dlight;
    int size; // including header
    unsigned width;
    unsigned height; // DEBUG only needed for debug
    float mipscale;
    texture_t* texture; // checked for animating textures
    byte data[4];       // width*height elements
} surfcache_t;


typedef struct
{
    pixel_t* surfdat;  // destination for generated surface
    int rowbytes;      // destination logical width in bytes
    surfcache_t* surf; // description for surface to generate
    fixed8_t lightadj[MAXLIGHTMAPS];
    // adjust for lightmap levels for dynamic lighting
    texture_t* texture; // corrected for animating textures
    int surfmip;        // mipmapped ratio of surface texels / world pixels
    int surfwidth;      // in mipmapped texels
    int surfheight;     // in mipmapped texels
} drawsurf_t;



//====================================================

extern bool r_cache_thrash; // compatability
extern qvec3 modelorg, r_entorigin;
extern entity_t* currententity;
extern int r_visframecount; // ??? what difs?
extern int r_framecount;
extern mplane_t frustum[4];

//
// view origin
//
extern qvec3 vup;
extern qvec3 vpn;
extern qvec3 vright;
extern qvec3 r_origin;

//
// screen size info
//
extern refdef_t r_refdef;
extern mleaf_t *r_viewleaf, *r_oldviewleaf;
extern int
    d_lightstylevalue[MAX_LIGHTSTYLES]; // 8.8 fraction of base light value

extern cvar_t r_norefresh;
extern cvar_t r_drawentities;
extern cvar_t r_drawworldtext;
extern cvar_t r_drawworld;
extern cvar_t r_drawviewmodel;
extern cvar_t r_speeds;
extern cvar_t r_pos;
extern cvar_t r_waterwarp;
extern cvar_t r_fullbright;
extern cvar_t r_lightmap;
extern cvar_t r_shadows;
extern cvar_t r_wateralpha;
extern cvar_t r_lavaalpha;
extern cvar_t r_telealpha;
extern cvar_t r_slimealpha;
extern cvar_t r_dynamic;
extern cvar_t r_novis;
extern cvar_t r_scale;

extern cvar_t gl_clear;
extern cvar_t gl_cull;
extern cvar_t gl_smoothmodels;
extern cvar_t gl_affinemodels;
extern cvar_t gl_polyblend;
extern cvar_t gl_flashblend;
extern cvar_t gl_nocolors;

extern cvar_t gl_playermip;

extern cvar_t gl_subdivide_size;
extern float load_subdivide_size; // johnfitz -- remember what subdivide_size
                                  // value was when this map was loaded

extern int gl_stencilbits;

// Multitexture
extern bool mtexenabled;
extern bool gl_mtexable;
// extern PFNGLMULTITEXCOORD2FARBPROC glMultiTexCoord2fARB;
// extern PFNGLACTIVETEXTUREARBPROC glActiveTextureARB;
// extern PFNGLCLIENTACTIVETEXTUREARBPROC glClientActiveTextureARB;
extern GLint gl_max_texture_units; // ericw

// johnfitz -- anisotropic filtering
// #define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
// #define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
extern float gl_max_anisotropy;
extern bool gl_anisotropy_able;

// ericw -- VBO
// extern PFNGLBINDBUFFERARBPROC glBindBufferARB;
// extern PFNGLBUFFERDATAARBPROC glBufferDataARB;
// extern PFNGLBUFFERSUBDATAARBPROC glBufferSubDataARB;
// extern PFNGLDELETEBUFFERSARBPROC glDeleteBuffersARB;
// extern PFNGLGENBUFFERSARBPROC glGenBuffersARB;
extern bool gl_vbo_able;
// ericw

// ericw -- GLSL

// SDL 1.2 has a bug where it doesn't provide these typedefs on OS X!
/*
typedef GLuint(APIENTRYP QS_PFNGLCREATESHADERPROC)(GLenum type);
typedef void(APIENTRYP QS_PFNGLDELETESHADERPROC)(GLuint shader);
typedef void(APIENTRYP QS_PFNGLDELETEPROGRAMPROC)(GLuint program);
typedef void(APIENTRYP QS_PFNGLSHADERSOURCEPROC)(GLuint shader, GLsizei count,
    const GLchar* const* string, const GLint* length);
typedef void(APIENTRYP QS_PFNGLCOMPILESHADERPROC)(GLuint shader);
typedef void(APIENTRYP QS_PFNGLGETSHADERIVPROC)(
    GLuint shader, GLenum pname, GLint* params);
typedef void(APIENTRYP QS_PFNGLGETSHADERINFOLOGPROC)(
    GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
typedef void(APIENTRYP QS_PFNGLGETPROGRAMIVPROC)(
    GLuint program, GLenum pname, GLint* params);
typedef void(APIENTRYP QS_PFNGLGETPROGRAMINFOLOGPROC)(
    GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
typedef GLuint(APIENTRYP QS_PFNGLCREATEPROGRAMPROC)();
typedef void(APIENTRYP QS_PFNGLATTACHSHADERPROC)(GLuint program, GLuint shader);
typedef void(APIENTRYP QS_PFNGLLINKPROGRAMPROC)(GLuint program);
typedef void(APIENTRYP QS_PFNGLBINDATTRIBLOCATIONFUNC)(
    GLuint program, GLuint index, const GLchar* name);
typedef void(APIENTRYP QS_PFNGLUSEPROGRAMPROC)(GLuint program);
typedef GLint(APIENTRYP QS_PFNGLGETATTRIBLOCATIONPROC)(
    GLuint program, const GLchar* name);
typedef void(APIENTRYP QS_PFNGLVERTEXATTRIBPOINTERPROC)(GLuint index,
    GLint size, GLenum type, GLboolean normalized, GLsizei stride,
    const void* pointer);
typedef void(APIENTRYP QS_PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint index);
typedef void(APIENTRYP QS_PFNGLDISABLEVERTEXATTRIBARRAYPROC)(GLuint index);
typedef GLint(APIENTRYP QS_PFNGLGETUNIFORMLOCATIONPROC)(
    GLuint program, const GLchar* name);
typedef void(APIENTRYP QS_PFNGLUNIFORM1IPROC)(GLint location, GLint v0);
typedef void(APIENTRYP QS_PFNGLUNIFORM1FPROC)(GLint location, GLfloat v0);
typedef void(APIENTRYP QS_PFNGLUNIFORM3FPROC)(
    GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
typedef void(APIENTRYP QS_PFNGLUNIFORM4FPROC)(
    GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);

extern QS_PFNGLCREATESHADERPROC glCreateShader;
extern QS_PFNGLDELETESHADERPROC glDeleteShader;
extern QS_PFNGLDELETEPROGRAMPROC glDeleteProgram;
extern QS_PFNGLSHADERSOURCEPROC glShaderSource;
extern QS_PFNGLCOMPILESHADERPROC glCompileShader;
extern QS_PFNGLGETSHADERIVPROC glGetShaderiv;
extern QS_PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog;
extern QS_PFNGLGETPROGRAMIVPROC glGetProgramiv;
extern QS_PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog;
extern QS_PFNGLCREATEPROGRAMPROC glCreateProgram;
extern QS_PFNGLATTACHSHADERPROC glAttachShader;
extern QS_PFNGLLINKPROGRAMPROC glLinkProgram;
extern QS_PFNGLBINDATTRIBLOCATIONFUNC glBindAttribLocation;
extern QS_PFNGLUSEPROGRAMPROC glUseProgram;
extern QS_PFNGLGETATTRIBLOCATIONPROC glGetAttribLocation;
extern QS_PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer;
extern QS_PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray;
extern QS_PFNGLDISABLEVERTEXATTRIBARRAYPROC glDisableVertexAttribArray;
extern QS_PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation;
extern QS_PFNGLUNIFORM1IPROC glUniform1i;
extern QS_PFNGLUNIFORM1FPROC glUniform1f;
extern QS_PFNGLUNIFORM3FPROC glUniform3f;
extern QS_PFNGLUNIFORM4FPROC glUniform4f;
*/

extern bool gl_glsl_able;
extern bool gl_glsl_gamma_able;
extern bool gl_glsl_alias_able;
// ericw --

// ericw -- NPOT texture support
extern bool gl_texture_NPOT;

// johnfitz -- polygon offset
#define OFFSET_BMODEL 1
#define OFFSET_NONE 0
#define OFFSET_DECAL -1
#define OFFSET_FOG -2
#define OFFSET_SHOWTRIS -3
void GL_PolygonOffset(int);

// johnfitz -- GL_EXT_texture_env_combine
// the values for GL_ARB_ are identical
#define GL_COMBINE_EXT 0x8570
#define GL_COMBINE_RGB_EXT 0x8571
#define GL_COMBINE_ALPHA_EXT 0x8572
#define GL_RGB_SCALE_EXT 0x8573
#define GL_CONSTANT_EXT 0x8576
#define GL_PRIMARY_COLOR_EXT 0x8577
#define GL_PREVIOUS_EXT 0x8578
#define GL_SOURCE0_RGB_EXT 0x8580
#define GL_SOURCE1_RGB_EXT 0x8581
#define GL_SOURCE0_ALPHA_EXT 0x8588
#define GL_SOURCE1_ALPHA_EXT 0x8589
extern bool gl_texture_env_combine;
extern bool gl_texture_env_add; // for GL_EXT_texture_env_add

// johnfitz -- rendering statistics
extern int rs_brushpolys, rs_aliaspolys, rs_skypolys, rs_particles, rs_fogpolys;
extern int rs_dynamiclightmaps, rs_brushpasses, rs_aliaspasses, rs_skypasses;
extern float rs_megatexels;

// johnfitz -- track developer statistics that vary every frame
extern cvar_t devstats;
typedef struct
{
    int packetsize;
    int edicts;
    int visedicts;
    int efrags;
    int tempents;
    int beams;
    int dlights;
} devstats_t;
extern devstats_t dev_stats, dev_peakstats;

// ohnfitz -- reduce overflow warning spam
typedef struct
{
    double packetsize;
    double efrags;
    double beams;
    double varstring;
} overflowtimes_t;
extern overflowtimes_t
    dev_overflows; // this stores the last time overflow messages were
                   // displayed, not the last time overflows occured
#define CONSOLE_RESPAM_TIME 3 // seconds between repeated warning messages

// johnfitz -- moved here from r_brush.c
extern int gl_lightmap_format, lightmap_bytes;

#define LMBLOCK_WIDTH \
    256 // FIXME: make dynamic. if we have a decent card there's no real reason
        // not to use 4k or 16k (assuming there's no lightstyles/dynamics that
        // need uploading...)
#define LMBLOCK_HEIGHT \
    256 // Alternatively, use texture arrays, which would avoid the need to
        // switch textures as often.

typedef struct glRect_s
{
    unsigned short l, t, w, h;
} glRect_t;

struct lightmap_s
{
    gltexture_t* texture;
    glpoly_t* polys;
    bool modified;
    glRect_t rectchange;

    // the lightmap texture data needs to be kept in
    // main memory so texsubimage can update properly
    byte* data; //[4*LMBLOCK_WIDTH*LMBLOCK_HEIGHT];
};
extern struct lightmap_s* lightmap;
extern int lightmap_count; // allocated lightmaps

extern int gl_warpimagesize; // johnfitz -- for water warp

extern bool r_drawflat_cheatsafe, r_fullbright_cheatsafe, r_lightmap_cheatsafe,
    r_drawworld_cheatsafe; // johnfitz

typedef struct glsl_attrib_binding_s
{
    const char* name;
    GLuint attrib;
} glsl_attrib_binding_t;

extern float map_wateralpha, map_lavaalpha, map_telealpha,
    map_slimealpha; // ericw
extern float
    map_fallbackalpha; // spike -- because we might want r_wateralpha to apply
                       // to teleporters while water itself wasn't watervised

// johnfitz -- fog functions called from outside gl_fog.c
void Fog_ParseServerMessage();
float* Fog_GetColor();
float Fog_GetDensity();
void Fog_EnableGFog();
void Fog_DisableGFog();
void Fog_StartAdditive();
void Fog_StopAdditive();
void Fog_SetupFrame();
void Fog_NewMap();
void Fog_Init();
void Fog_SetupState();
const char* Fog_GetFogCommand(); // for demo recording

void R_NewGame();

struct dlight_t;
void CL_UpdateLightstyle(unsigned int idx, const char* stylestring);

void R_AnimateLight();
void R_MarkSurfaces();
void R_CullSurfaces();
bool R_CullBox(const qvec3& emins, const qvec3& emaxs);
void R_StoreEfrags(efrag_t** ppefrag);
bool R_CullModelForEntity(entity_t* e);
void R_RotateForEntity(const qvec3& origin, const qvec3& angles);
void R_MarkLights(
    dlight_t* light, const qvec3& lightorg, int num, mnode_t* node);

void R_InitParticles();
void R_DrawParticles();
void CL_RunParticles();
void R_ClearParticles();

void R_TranslatePlayerSkin(int playernum);
void R_TranslateNewPlayerSkin(int playernum); // johnfitz -- this handles cases
                                              // when the actual texture changes
void R_UpdateWarpTextures();

void R_DrawWorld();
void R_DrawAliasModel(entity_t* e);
void R_DrawBrushModel(entity_t* e);
void R_DrawSpriteModel(entity_t* e);

void R_DrawTextureChains_Water(
    qmodel_t* model, entity_t* ent, texchain_t chain);

void R_RenderDlights();
void GL_BuildLightmaps();
void GL_DeleteBModelVertexBuffer();
void GL_BuildBModelVertexBuffer();
void GLMesh_LoadVertexBuffers();
void GLMesh_DeleteVertexBuffers();
void GLMesh_LoadVertexBuffer(qmodel_t* m, const aliashdr_t* hdr);
void R_RebuildAllLightmaps();

int R_LightPoint(const qvec3& p);

void GL_SubdivideSurface(msurface_t* fa);
void R_BuildLightMap(qmodel_t* model, msurface_t* surf, byte* dest, int stride);
void R_RenderDynamicLightmaps(qmodel_t* model, msurface_t* fa);
void R_UploadLightmaps();

void R_DrawWorld_ShowTris();
void R_DrawBrushModel_ShowTris(entity_t* e);
void R_DrawAliasModel_ShowTris(entity_t* e);
void R_DrawParticles_ShowTris();

GLint GL_GetUniformLocation(GLuint* programPtr, const char* name);

void GLWorld_CreateShaders();
void GLAlias_CreateShaders();
void GL_DrawAliasShadow(entity_t* e);
void DrawGLTriangleFan(glpoly_t* p);
void DrawGLPoly(glpoly_t* p);
void DrawWaterPoly(glpoly_t* p);
void GL_MakeAliasModelDisplayLists(qmodel_t* m, aliashdr_t* hdr);

void Sky_Init();
void Sky_DrawSky();
void Sky_NewMap();
void Sky_LoadTexture(
    texture_t* mt, srcformat fmt, unsigned int width, unsigned int height);
void Sky_LoadSkyBox(const char* name);
extern bool skyroom_drawn;   // we draw a skyroom this frame
extern bool skyroom_enabled; // we know where the skyroom is ...
extern qvec4 skyroom_origin; //... and it is here. [3] is paralax scale
extern qvec4 skyroom_orientation;

void TexMgr_RecalcWarpImageSize();

void R_ClearTextureChains(qmodel_t* mod, texchain_t chain);
void R_ChainSurface(msurface_t* surf, texchain_t chain);
void R_DrawTextureChains(qmodel_t* model, entity_t* ent, texchain_t chain);
void R_DrawWorld_Water();

void GL_BindBuffer(GLenum target, GLuint buffer);
void GL_ClearBufferBindings();

void GLSLGamma_DeleteTexture();
void GLSLGamma_GammaCorrect();

void R_ScaleView_DeleteTexture();

float GL_WaterAlphaForSurface(msurface_t* fa);
