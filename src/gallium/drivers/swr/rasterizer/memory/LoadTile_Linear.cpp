/****************************************************************************
* Copyright (C) 2016 Intel Corporation.   All Rights Reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice (including the next
* paragraph) shall be included in all copies or substantial portions of the
* Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*
* @file LoadTile.cpp
*
* @brief Functionality for Load
*
******************************************************************************/
#include "LoadTile.h"

PFN_LOAD_TILES sLoadTilesColorTable_SWR_TILE_NONE[NUM_SWR_FORMATS];
PFN_LOAD_TILES sLoadTilesDepthTable_SWR_TILE_NONE[NUM_SWR_FORMATS];

//////////////////////////////////////////////////////////////////////////
/// @brief Sets up tables for LoadTile
void InitLoadTilesTable_Linear()
{
    InitLoadTileColorTable<SWR_TILE_NONE>(sLoadTilesColorTable_SWR_TILE_NONE);
    InitLoadTileDepthTable<SWR_TILE_NONE>(sLoadTilesDepthTable_SWR_TILE_NONE);
}
