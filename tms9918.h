/*
 * Copyright (c) 2021 Troy Schrapel.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following condition:  The
 * above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef _VR_EMU_TMS9918_H_
#define _VR_EMU_TMS9918_H_

/* ------------------------------------------------------------------
 * LINKAGE MODES:
 * 
 * Default (nothing defined):    When your executable is using vrEmuTms9918 as a DLL
 * VR_6502_EMU_COMPILING_DLL:    When compiling vrEmuTms9918 as a DLL
 * VR_6502_EMU_STATIC:           When linking vrEmu6502 statically in your executable
 */

#include <stdint.h>
#include <stdbool.h>

/* PRIVATE DATA STRUCTURE
 * ---------------------------------------- */
struct vrEmuTMS9918_s;
typedef struct vrEmuTMS9918_s VrEmuTms9918;

typedef enum
{
  TMS_MODE_GRAPHICS_I,
  TMS_MODE_GRAPHICS_II,
  TMS_MODE_TEXT,
  TMS_MODE_MULTICOLOR,
} vrEmuTms9918Mode;

typedef enum
{
  TMS_TRANSPARENT = 0,
  TMS_BLACK,
  TMS_MED_GREEN,
  TMS_LT_GREEN,
  TMS_DK_BLUE,
  TMS_LT_BLUE,
  TMS_DK_RED,
  TMS_CYAN,
  TMS_MED_RED,
  TMS_LT_RED,
  TMS_DK_YELLOW,
  TMS_LT_YELLOW,
  TMS_DK_GREEN,
  TMS_MAGENTA,
  TMS_GREY,
  TMS_WHITE,
} vrEmuTms9918Color;

typedef enum
{
  TMS_REG_0 = 0,
  TMS_REG_1,
  TMS_REG_2,
  TMS_REG_3,
  TMS_REG_4,
  TMS_REG_5,
  TMS_REG_6,
  TMS_REG_7,
  TMS_NUM_REGISTERS,
  TMS_REG_NAME_TABLE        = TMS_REG_2,
  TMS_REG_COLOR_TABLE       = TMS_REG_3,
  TMS_REG_PATTERN_TABLE     = TMS_REG_4,
  TMS_REG_SPRITE_ATTR_TABLE = TMS_REG_5,
  TMS_REG_SPRITE_PATT_TABLE = TMS_REG_6,
  TMS_REG_FG_BG_COLOR       = TMS_REG_7,
} vrEmuTms9918Register;

#define TMS9918_PIXELS_X 256
#define TMS9918_PIXELS_Y 192


/* PUBLIC INTERFACE
 * ---------------------------------------- */

 /* Function:  vrEmuTms9918New
  * --------------------
  * create a new TMS9918
  */

VrEmuTms9918* vrEmuTms9918New();

/* Function:  vrEmuTms9918Reset
  * --------------------
  * reset the new TMS9918
  */

void vrEmuTms9918Reset(VrEmuTms9918* tms9918);

/* Function:  vrEmuTms9918Destroy
 * --------------------
 * destroy a TMS9918
 *
 * tms9918: tms9918 object to destroy / clean up
 */

void vrEmuTms9918Destroy(VrEmuTms9918* tms9918);

/* Function:  vrEmuTms9918WriteAddr
 * --------------------
 * write an address (mode = 1) to the tms9918
 *
 * uint8_t: the data (DB0 -> DB7) to send
 */

void vrEmuTms9918WriteAddr(VrEmuTms9918* tms9918, uint8_t data);

/* Function:  vrEmuTms9918WriteData
 * --------------------
 * write data (mode = 0) to the tms9918
 *
 * uint8_t: the data (DB0 -> DB7) to send
 */

void vrEmuTms9918WriteData(VrEmuTms9918* tms9918, uint8_t data);

/* Function:  vrEmuTms9918ReadStatus
 * --------------------
 * read from the status register
 */

uint8_t vrEmuTms9918ReadStatus(VrEmuTms9918* tms9918);

/* Function:  vrEmuTms9918ReadData
 * --------------------
 * read data (mode = 0) from the tms9918
 */

uint8_t vrEmuTms9918ReadData(VrEmuTms9918* tms9918);

/* Function:  vrEmuTms9918ReadDataNoInc
 * --------------------
 * read data (mode = 0) from the tms9918
 * don't increment the address pointer
 */

uint8_t vrEmuTms9918ReadDataNoInc(VrEmuTms9918* tms9918);


/* Function:  vrEmuTms9918ScanLine
 * ----------------------------------------
 * generate a scanline
 *
 * pixels to be filled with TMS9918 color palette indexes (vrEmuTms9918Color)
 */

void vrEmuTms9918ScanLine(VrEmuTms9918* tms9918, uint8_t y, uint8_t pixels[TMS9918_PIXELS_X]);

/* Function:  vrEmuTms9918RegValue
 * ----------------------------------------
 * return a reigister value
 */

uint8_t vrEmuTms9918RegValue(VrEmuTms9918* tms9918, vrEmuTms9918Register reg);

/* Function:  vrEmuTms9918WriteRegValue
 * ----------------------------------------
 * write a reigister value
 */

void vrEmuTms9918WriteRegValue(VrEmuTms9918* tms9918, vrEmuTms9918Register reg, uint8_t value);


/* Function:  vrEmuTms9918VramValue
 * ----------------------------------------
 * return a value from vram
 */

uint8_t vrEmuTms9918VramValue(VrEmuTms9918* tms9918, uint16_t addr);


/* Function:  vrEmuTms9918DisplayEnabled
  * --------------------
  * check BLANK flag
  */

bool vrEmuTms9918DisplayEnabled(VrEmuTms9918* tms9918);


#endif // _VR_EMU_TMS9918_H_
