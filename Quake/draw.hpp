/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
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

#include "screen.hpp"

struct qpic_t;

// draw.h -- these are the only functions outside the refresh allowed
// to touch the vid buffer

extern qpic_t* draw_disc; // also used on sbar

void Draw_Init();
void Draw_Character(int x, int y, int num);
void Draw_DebugChar(char num);
void Draw_Pic(int x, int y, qpic_t* pic);
void Draw_TransPicTranslate(int x, int y, qpic_t* pic, int top,
    int bottom);                   // johnfitz -- more parameters
void Draw_ConsoleBackground(); // johnfitz -- removed parameter int lines
void Draw_TileClear(int x, int y, int w, int h);
void Draw_Fill(
    int x, int y, int w, int h, int c, float alpha); // johnfitz -- added alpha
void Draw_FadeScreen();
void Draw_String(int x, int y, const char* str);
qpic_t* Draw_PicFromWad(const char* name);
qpic_t* Draw_CachePic(const char* path);
void Draw_NewGame();

void GL_SetCanvas(canvastype newcanvas); // johnfitz
