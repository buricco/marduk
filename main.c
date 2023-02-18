/*
 * Copyright 2022, 2023 S. V. Nickolas.
 * Copyright 2023 Marcin Wołoszczuk.
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

/*
 * Note: Some parts of this program have other copyrights.  See the individual
 *       files for specific copyright information.  SDL2 has different but
 *       similar license terms.
 *
 * Testing has currently only been done under Debian 11 Linux.
 */

/* C99 includes */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* SDL2 include */
#include <SDL.h>

/* Chipset includes */
#include "tms9918.h"
#include "tms_util.h"
#include "emu2149.h"
#include "z80.h"

/* Cable modem include */
#include "modem.h"

/* Alterable filenames */
#include "paths.h"

#define VERSION "0.23"

/*
 * Forward declaration.
 */
static void reinit_cpu(void);

/*
 * Speed control.
 *
 * Not exact, but it helps keep the speed more or less even based on how long
 * it takes to run one scanline's worth of code.
 */
#define FIRE_TICK 63492
unsigned long long next_fire;
struct timespec timespec;

/*
 * The NABU has 64K RAM.
 *
 * Early models have 4K ROM, there is also a version with 8K ROM.
 */
static uint8_t RAM[65536], ROM[8192];
int romsize;

/*
 * Internal state for emulated chips.
 *
 * This means both chips with external cores and internal cores.
 */
z80 cpu;
VrEmuTms9918 *vdp;
PSG *psg;
int ctrlreg;

int gotmodem;

/* Next cycle for scanline loop. */
unsigned long next;

/* Set to nonzero to tell the emulator to exit. */
int death_flag;

/* To kick the dog */
unsigned next_watchdog;

/* keyboard/joystick? */
int keyjoy;
uint8_t joybyte;

/*
 * SDL2 structure pointers.
 *
 * display is a dynamically allocated offscreen buffer used for rendering the
 * VDP data, or the generated NTSC noise.
 */
SDL_Window *screen;
SDL_Renderer *renderer;
SDL_Texture *texture;
SDL_AudioDeviceID audio_device;
SDL_AudioSpec audio_spec;

uint32_t *display;

int psg_calc_flag = 0;

/*
 * Emulation of the NABU memory map.
 *
 * Unlike 6502 and 68000 systems, but like x86, the Z80 has two separate
 * memory maps - one for RAM and ROM, and one for I/O devices.  This makes it
 * much easier to interface a full 64K of RAM to a Z80, where double-banking
 * is absolutely necessary to do that on a 6502 (witness the double Dxxx on an
 * Apple ][).
 *
 * Emulation of basic memory reads and writes is mindlessly simple; if the ROM
 * is banked in, it overrides reads from RAM - otherwise operate on RAM.  The
 * entire memory map is otherwise filled with RAM.
 *
 * What the following functions do should be self-evident.  The void pointers
 * are unused but required by the CPU core.
 */

uint8_t mem_read(void *blob, uint16_t addr)
{
  if ((!(ctrlreg & 0x01)) && (addr < romsize))
    return ROM[addr];

  return RAM[addr];
}

void mem_write(void *blob, uint16_t addr, uint8_t val)
{
  RAM[addr] = val;
}

/*
 * Emulation of port I/O is a lot more complicated, and handled by the
 * following two functions.
 *
 * As with the above, the CPU core passes an unnecessary (but potentially
 * useful on other architectures) pointer to the CPU struct; it is not used.
 */

/*
 * 00 - control register (write)
 * 40 - AY-8910 data port
 * 41 - AY-8910 latch (?)
 *      The PSG ports are BACKWARD from other systems!  Or at least from the
 *      MSX and the Arcade Board.
 * 80 - cable modem
 * 90 - keyboard (mostly ASCII)
 * 91 - keyboard strobe (also written to, not sure what for yet)
 *      This should call an interrupt but not sure how interrupts are supposed
 *      to work because I'm a 65C02 person, not a Z80 person.
 * A0 - TMS9918 read/write data
 * A1 - TMS9918 write control register
 * B0 - parallel port data
 *
 * Control reg:
 * 01 - ROM disable
 * 02 - enable video
 * 04 - parallel port strobe
 * 08 - green (check) LED
 * 10 - red (alert) LED
 * 20 - yellow (pause) LED
 *
 * The keyboard sends 0x95 when powering up.
 * Every so often (~3.7 sec.) it should send 0x94 to kick the dog.
 *
 * https://vintagecomputer.ca/files/Nabu/
 *   Nabu_Computer_Technical_Manual_by_MJP-compressed.pdf
 */

/* priority encoder */
void int_prio_enc(int EI, int I0, int I1, int I2, int I3, int I4, int I5, int I6, int I7,
                  int *GS, int *Q0, int *Q1, int *Q2, int *EO)
{
  /* most often and default values, ease the typing later on ;) */
  *GS = 0;
  *EO = 1;
  *Q0 = *Q1 = *Q2 = 0;
  if (EI == 1)
  {
    *GS = 1;
    *Q0 = 1;
    *Q1 = 1;
    *Q2 = 1;
  }
  else if (I0 & I1 & I2 & I3 & I4 & I5 & I6 & I7)
  {
    *GS = *Q0 = *Q1 = *Q2 = 1;
    *EO = 0;
  }
  else if (!I7)
  { /* nop */
  }
  else if (!I6)
  {
    *Q0 = 1;
  }
  else if (!I5)
  {
    *Q1 = 1;
  }
  else if (!I4)
  {
    *Q0 = *Q1 = 1;
  }
  else if (!I3)
  {
    *Q2 = 1;
  }
  else if (!I2)
  {
    *Q0 = *Q2 = 1;
  }
  else if (!I1)
  {
    *Q1 = *Q2 = 1;
  }
  else
  {
    *Q0 = *Q1 = *Q2;
  }
}

/* basically a macro to accept an interrupt vector instead of single bits */
void int_prio_enc_alt(int EI, int interrupts, int *GS, int *Q0,
                      int *Q1, int *Q2, int *EO)
{
  int_prio_enc(EI, interrupts & 0x01, (interrupts & 0x02) >> 1,
               (interrupts & 0x04) >> 2, (interrupts & 0x08) >> 3,
               (interrupts & 0x10) >> 4, (interrupts & 0x20) >> 5,
               (interrupts & 0x40) >> 6, (interrupts & 0x80) >> 7,
               GS, Q0, Q1, Q2, EO);
}

/* fed into PSG's PORTB via PSG_writeReg,
 since NABU never writes to PORTB, this should be safe */
uint8_t psg_portb = 0;

/* keep the current PSG's PORTA in this scope */
uint8_t psg_porta = 0;

/* interrupt variables */
uint8_t hccarint = 0;
uint8_t hccatint = 0;
uint8_t keybdint = 0;
uint8_t vdpint = 0;
uint8_t interrupts = 0;
uint8_t prev_int_line = 0;

void update_interrupts()
{
  if (hccarint)
  {
    interrupts |= 0x80;
  }
  else
  {
    interrupts &= ~0x80;
  }
  if (hccatint)
  {
    interrupts |= 0x40;
  }
  else
  {
    interrupts &= ~0x40;
  }
  if (keybdint)
  {
    interrupts |= 0x20;
  }
  else
  {
    interrupts &= ~0x20;
  }
  if (vdpint)
  {
    interrupts |= 0x10;
  }
  else
  {
    interrupts &= ~0x10;
  }
  int int_prio = ~(interrupts & psg_porta);
  int GS, Q0, Q1, Q2, EO;
  int_prio_enc_alt(0, int_prio, &GS, &Q0, &Q1, &Q2, &EO);
  psg_portb &= 0xf0;
  psg_portb |= EO | (Q0 << 1) | (Q1 << 2) | (Q2 << 3);
  PSG_writeReg(psg, 15, psg_portb);
  /*
  A0 - D7
  A1 - D2
  A2 - D8
  */
  if (!GS)
  {
    // z80_gen_int(&cpu, (Q0 << 6) | (Q1 << 1) | (Q2 << 7));
    z80_gen_int(&cpu, psg_portb & 0x0e);
    // z80_gen_int(&cpu, prev_int_line);
    // prev_int_line = (Q0 << 5) | (Q1 << 6) | (Q2 << 7);
    // z80_gen_int(&cpu, !GS);
  }
}

char keyboard_buffer[256];
uint8_t keyboard_buffer_write_ptr = 0;
uint8_t keyboard_buffer_read_ptr = 0;

void keyboard_buffer_put(uint8_t code)
{
  keyboard_buffer[keyboard_buffer_write_ptr++] = code;
}

int keyboard_buffer_empty()
{
  if (keyboard_buffer_read_ptr == keyboard_buffer_write_ptr) 
  {
    return 1;
  }
  return 0;
}

uint8_t keyboard_buffer_get()
{
  if (keyboard_buffer_read_ptr != keyboard_buffer_write_ptr) 
  {
    return keyboard_buffer[keyboard_buffer_read_ptr++];
  }
  return 255;
}

/* used for latching PSG's register address */
uint8_t psg_reg_address = 0x00;

uint8_t port_read(z80 *mycpu, uint8_t port)
{
  uint8_t t, b;

  switch (port)
  {
  case 0x40: /* read register from PSG */
    t = PSG_readReg(psg, psg_reg_address);
    return t;
  case 0x41:
    printf("IO read from 0x41, this shouldn't happen, exiting!\r\n");
    exit(-1);
    return 0;
  case 0x80:
    if (gotmodem)
    {
      t = modem_read(&b);
      if (t)
      {
        hccarint = 0;
        update_interrupts();
        return b;
      }
    }
    return 0;
  case 0x90: /* Not sure if this is the right action */
    t = keyboard_buffer_get();
    // printf("KEYBOARD returned 0x%02X\r\n", t);
    keybdint = 0;
    update_interrupts();
    if (t == 255)
      return 0;
    else
      return t;
  case 0x91: /* Not sure if this is the right action */
    return keyboard_buffer_empty() ? 0x00 : 0xff;
  case 0xA0:
    return vrEmuTms9918ReadData(vdp);
  case 0xA1: /* Not sure if this is the right action */
    b = vrEmuTms9918ReadStatus(vdp);
    // printf("VDP STATUS: 0x%02X\r\n", b);
    vdpint = 0;
    update_interrupts();
    return b;
  default:
#ifdef PORT_DEBUG
    printf("WARNING: unknown port read (0x%02X)\n", port);
#endif
    return 0;
  }
}

void port_write(z80 *mycpu, uint8_t port, uint8_t val)
{
  uint8_t psg_reg7;
  switch (port)
  {
  case 0x00:
    ctrlreg = val;
    return;
  case 0x40: /* write data to PSG */
    psg_reg7 = PSG_readReg(psg, 7);
    if (psg_reg_address == 0x0E)
    {
      if (!(psg_reg7 & 0x40))
      {
        printf("Writing to PORTA when it's set to input, DENIED!\r\n");
        printf("psg_reg7 = %02X\r\n", psg_reg7);
        // return;
      }
      if (val & 0x10)
      {
        // printf("Bro, we want VDPINT for real, psg_reg7: 0x%02X, interrupts: 0x%02X\r\n", psg_reg7, interrupts);
      }
      if (psg_porta != val)
      {
        psg_porta = val;
        update_interrupts();
      }
    }
    if (psg_reg_address == 0x0F)
    {
      if (!(psg_reg7 & 0x80))
      {
        printf("Writing to PORTB when it's set to input, DENIED!\r\n");
        printf("psg_reg7 = %02X\r\n", psg_reg7);
        // return;
      }
    }
    PSG_writeReg(psg, psg_reg_address, val);
    return;
  case 0x41: /* write address to PSG */
    if (val > 0x1f)
    {
      printf("PSG reg address > 0x1f when writing, exiting!\r\n");
      exit(-1);
    }
    psg_reg_address = val;
    return;
  case 0x80:
    if (gotmodem)
    {
      modem_write(val);
    }
    return;
  case 0xA0:
    vrEmuTms9918WriteData(vdp, val);
    return;
  case 0xA1:
    vrEmuTms9918WriteAddr(vdp, val);
    return;
#ifdef PORT_DEBUG
  default:
    printf("WARNING: unknown port write (0x%02X): 0x%02X\n", port, val);
#endif
  }
}

void keyboard_poll()
{
  SDL_Event event;
  /* eat up all events */
  while (SDL_PollEvent(&event))
  {
    switch (event.type)
    {
    /*
     * "Break key" codes for arrows.
     */
    case SDL_KEYUP:
      switch (event.key.keysym.sym)
      {
       case ' ':
        if (keyjoy)
        {
         joybyte &= 0xEF;
         keyboard_buffer_put(0x80);
         keyboard_buffer_put(joybyte|0xA0);
        }
        break;
      case SDLK_UP:
        if (keyjoy)
        {
         joybyte &= 0xF7;
         keyboard_buffer_put(0x80);
         keyboard_buffer_put(joybyte|0xA0);
        }
        else
         keyboard_buffer_put(0xF2);
        break;
      case SDLK_DOWN:
        if (keyjoy)
        {
         joybyte &= 0xFD;
         keyboard_buffer_put(0x80);
         keyboard_buffer_put(joybyte|0xA0);
        }
        else
         keyboard_buffer_put(0xF3);
        break;
      case SDLK_LEFT:
        if (keyjoy)
        {
         joybyte &= 0xFE;
         keyboard_buffer_put(0x80);
         keyboard_buffer_put(joybyte|0xA0);
        }
        else
         keyboard_buffer_put(0xF1);
        break;
      case SDLK_RIGHT:
        if (keyjoy)
        {
         joybyte &= 0xFB;
         keyboard_buffer_put(0x80);
         keyboard_buffer_put(joybyte|0xA0);
        }
        else
         keyboard_buffer_put(0xF0);
        break;
      case SDLK_PAGEUP: /* « */
        keyboard_buffer_put(0xF5);
        break;
      case SDLK_PAGEDOWN: /* » */
        keyboard_buffer_put(0xF4);
        break;
      case SDLK_INSERT: /* YES */
        keyboard_buffer_put(0xF7);
        break;
      case SDLK_DELETE: /* NO */
        keyboard_buffer_put(0xF6);
        break;
      case SDLK_PAUSE:
        keyboard_buffer_put(0xF9);
        break;
      case SDLK_END:
        keyboard_buffer_put(0xFA);
        break;
      }
      break;
    case SDL_KEYDOWN:
      /*
         * "Make key" codes for arrows.
         */
      switch (event.key.keysym.sym)
      {
       case ' ':
        if (keyjoy)
        {
         joybyte |= 0x10;
         keyboard_buffer_put(0x80);
         keyboard_buffer_put(joybyte|0xA0);
        }
        break;
      case SDLK_UP:
        if (keyjoy)
        {
         joybyte |= 0x08;
         keyboard_buffer_put(0x80);
         keyboard_buffer_put(joybyte|0xA0);
        }
        else
         keyboard_buffer_put(0xE2);
        break;
      case SDLK_DOWN:
        if (keyjoy)
        {
         joybyte |= 0x02;
         keyboard_buffer_put(0x80);
         keyboard_buffer_put(joybyte|0xA0);
        }
        else
         keyboard_buffer_put(0xE3);
        break;
      case SDLK_LEFT:
        if (keyjoy)
        {
         joybyte |= 0x01;
         keyboard_buffer_put(0x80);
         keyboard_buffer_put(joybyte|0xA0);
        }
        else
         keyboard_buffer_put(0xE1);
        break;
      case SDLK_RIGHT:
        if (keyjoy)
        {
         joybyte |= 0x04;
         keyboard_buffer_put(0x80);
         keyboard_buffer_put(joybyte|0xA0);
        }
        else
         keyboard_buffer_put(0xE0);
        break;
      case SDLK_PAGEUP: /* « */
        keyboard_buffer_put(0xE5);
        break;
      case SDLK_PAGEDOWN: /* » */
        keyboard_buffer_put(0xE4);
        break;
      case SDLK_INSERT: /* YES */
        keyboard_buffer_put(0xE7);
        break;
      case SDLK_DELETE: /* NO */
        keyboard_buffer_put(0xE6);
        break;
      case SDLK_PAUSE:
        keyboard_buffer_put(0xE9);
        break;
      case SDLK_END:
        keyboard_buffer_put(0xEA);
        break;
        
      case SDLK_BACKSPACE:
       keyboard_buffer_put(0x7F);
       break;
      }
      if (event.key.keysym.sym < 128)
      {
        static char *shiftnums = ")!@#$%^&*(";
        SDL_Keymod m;
        int k;
        /*
         * Shift/Ctrl translation.  Not the most efficient method.
         *
         * The NABU keyboard looks like a standard modern ASCII layout (not the
         * strict ASCII layout used by some older computers).  Caps Lock is off
         * by default.
         *
         * The NABU keyboard doesn't actually have a |\ key, but we'll just
         * forget that for now.
         */

        k = event.key.keysym.sym;
        m = SDL_GetModState();
        if (m & KMOD_CTRL)
        {
          if (k == '[')
            k = 0x1B;
          if (k == '\\')
            k = 0x1C;
          if (k == ']')
            k = 0x1D;
          if (k == '-')
            k = 0x1F;
        }
        if (m & KMOD_SHIFT)
        {
          if (k == '`')
            k = '~';
          if (k == '-')
            k = '_';
          if (k == '=')
            k = '+';
          if (k == '[')
            k = '{';
          if (k == ']')
            k = '}';
          if (k == '\\')
            k = '|';
          if (k == ';')
            k = ':';
          if (k == '\'')
            k = '"';
          if (k == ',')
            k = '<';
          if (k == '.')
            k = '>';
          if (k == '/')
            k = '?';
        }
        if ((k >= 'a') && (k <= 'z'))
        {
          if (m & KMOD_CAPS)
            k ^= 32;
          if (m & KMOD_SHIFT)
            k ^= 32;
          if (m & KMOD_CTRL)
            k &= 0x1F;
        }
        else if ((k >= '0') && (k <= '9'))
        {
          if (m & KMOD_CTRL)
          {
            if (k == '2')
              k = 0xFF; /* interpreted as 0x00 when read */
            else if (k == '6')
              k = 0x1E;
          }
          else
          {
            if (m & KMOD_SHIFT)
              k = shiftnums[k & 0x0F];
          }
        }
        keyboard_buffer_put(k);
      }
      else
        switch (event.key.keysym.sym)
        {
        /* F3 - reset */
         case SDLK_F5:
          keyjoy=0;
          joybyte=0;
          printf ("Arrows and Space are KEYBOARD\n");
          break;
         case SDLK_F6:
          keyjoy=1;
          printf ("Arrows and Space are JOYSTICK\n");
          break;
        case SDLK_F3:
          printf("Reset pressed\n");
          clock_gettime(CLOCK_REALTIME, &timespec);
          next_fire = timespec.tv_nsec + FIRE_TICK;
          reinit_cpu();
          break;
        /* Alt-F4 - exit */
        case SDLK_F4:
          if (SDL_GetModState() & KMOD_ALT)
            death_flag = 1;
          break;
        /* F10 - also exit */
        case SDLK_F10:
          death_flag = 1;
          break;
        }
      break;
    case SDL_QUIT:
      death_flag = 1;
      break;
    }
  }
}

/*
 * Things to do once per scanline, like poll the keyboard, joystick, etc.
 */
void every_scanline(void)
{
  struct timespec n;

  keyboard_poll();
  clock_gettime(CLOCK_REALTIME, &timespec);
  n.tv_sec = 0;
  n.tv_nsec = next_fire - timespec.tv_nsec;
  next_fire = timespec.tv_nsec + FIRE_TICK;
  if (next_fire > n.tv_nsec)
  {
    nanosleep(&n, 0);
  }
}

/*
 * Exactly what it says on the tin.
 *
 * Call the TMS9918 emulator to generate the next scanline into the offscreen.
 */
void render_scanline(int line)
{
  int x;
  uint32_t r;
  uint32_t bg;
  uint8_t a_scanline[256];
  uint32_t g_scanline[320];
  if (line > 239)
    return;

  /*
   * To note:
   *
   * The background color is register 7, AND 0x0F.
   * The border is 64 pels left and right, 48 top and bottom, thus 512x384 in a
   * 640x480 window.
   *
   * The palette is stored RGBA, but we use ARGB; accomodate it.
   */
  bg = 0xFF000000 | (vrEmuTms9918Palette[vrEmuTms9918RegValue(vdp, 7) & 0x0F] >> 8);
  for (x = 0; x < 320; x++)
    g_scanline[x] = bg;
  if ((line >= 24) && (line < 216))
  {
    vrEmuTms9918ScanLine(vdp, line - 24, a_scanline);
    for (x = 0; x < 256; x++)
      g_scanline[x + 32] = vrEmuTms9918Palette[a_scanline[x]] >> 8;
  }

  /* Double-scan. */
  r = line * 1280;
  for (x = 0; x < 320; x++)
  {
    display[r + (x << 1)] = display[r + 1 + (x << 1)] =
        display[r + 640 + (x << 1)] = display[r + 641 + (x << 1)] = g_scanline[x];
  }

  /*
   * If the display is in "TV" mode, just spew some NTSC noise into the buffer.
   *
   * This actually looks pretty realistic (I grew up in the days of aerials and
   * 3 major TV networks, and am well acquainted with the appearance of NTSC
   * noise).
   */
  if (!(ctrlreg & 0x02))
  {
    uint32_t c;

    r = line * 1280;
    for (x = 0; x < 1280; x++)
    {
      c = rand() & 0xFF;
      display[r + x] = 0xFF000000 | (c << 16) | (c << 8) | (c);
    }
  }

  /*
   * Draw the LEDs.
   *
   * They will appear in the bottom right corner, in the order in which
   * they appear on the system unit.  The current code generates a sort of
   * rounded or "chewed-out" rectangle.  Not efficient code.
   */
  if ((line >= 232) && (line < 236))
  {
    uint32_t le[3], ri[3];

    r = line * 1280;
    
    if (keyjoy)
    {
     uint16_t c;
     
     /* 576-583 */
     if (line==235)
     {
      for (c=576; c<584; c++)
      {
       display[r+c]=display[r+640+c]=0xFFFFFFFF;
      }
     }
     else
     {
      display[r+579]=display[r+579+640]=0xFFFFFFFF;
      display[r+580]=display[r+580+640]=0xFFFFFFFF;
     }
      
     if (line==234) display[r+577]=display[r+577+640]=0xFFFFFFFF;
    }

    if (line == 232)
    {
      le[0] = display[r + 592];
      ri[0] = display[r + 599];
      le[1] = display[r + 608];
      ri[1] = display[r + 615];
      le[2] = display[r + 624];
      ri[2] = display[r + 631];
    }
    else if (line == 235)
    {
      le[0] = display[r + 640 + 592];
      ri[0] = display[r + 640 + 599];
      le[1] = display[r + 640 + 608];
      ri[1] = display[r + 640 + 615];
      le[2] = display[r + 640 + 624];
      ri[2] = display[r + 640 + 631];
    }

    for (x = 592; x < 600; x++) /* Yellow LED */
      display[r + x] = display[r + 640 + x] = (ctrlreg & 0x20) ? 0xFFFFFF00 : 0;
    for (x = 608; x < 616; x++) /* Red LED */
      display[r + x] = display[r + 640 + x] = (ctrlreg & 0x10) ? 0xFFFF0000 : 0;
    for (x = 624; x < 632; x++) /* Green LED */
      display[r + x] = display[r + 640 + x] = (ctrlreg & 0x08) ? 0xFF00FF00 : 0;

    if (line == 232)
    {
      display[r + 592] = le[0];
      display[r + 599] = ri[0];
      display[r + 608] = le[1];
      display[r + 615] = ri[1];
      display[r + 624] = le[2];
      display[r + 631] = ri[2];
    }
    else if (line == 235)
    {
      display[r + 640 + 592] = le[0];
      display[r + 640 + 599] = ri[0];
      display[r + 640 + 608] = le[1];
      display[r + 640 + 615] = ri[1];
      display[r + 640 + 624] = le[2];
      display[r + 640 + 631] = ri[2];
    }
  }
}

/*
 * End of frame.  Blit it out.
 *
 * Also for anything that needs done every 1/60 second.
 */
void next_frame(void)
{
  SDL_UpdateTexture(texture, 0, display, 640 * sizeof(uint32_t));
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, 0, 0);
  SDL_RenderPresent(renderer);
}

/*
 * Set up the CPU emulation.
 *
 * Initialize the function pointers and call the CPU emulator's init code.
 */
static void init_cpu(void)
{
  z80_init(&cpu);

  cpu.read_byte = mem_read;
  cpu.write_byte = mem_write;
  cpu.port_in = port_read;
  cpu.port_out = port_write;

  next = 228;
  keyboard_buffer_put(0x95);

  psg_portb = 0;
  psg_porta = 0;
  hccarint = 0;
  vdpint = 0;
  interrupts = 0;
  /* we fire the keyboard interrupt, to make the CPU read the 0x95 code */
  keybdint = 1;
  /* we are keeping the TX BUFFER EMPTY always high since this is an emu env */
  hccatint = 1;
  /* fire the above interrupts */
  update_interrupts();
}

/*
 * Reset the CPU emulation.
 */
static void reinit_cpu(void)
{
  void *tmp;

  tmp = cpu.userdata;
  init_cpu();
  cpu.userdata = tmp;
}

/*
 * Open ROM.
 */
static int init_rom(char *filename)
{
  int e;
  FILE *file;

  file = fopen(filename, "rb");
  if (!file)
  {
    fprintf(stderr, "FATAL: Failed to open ROM file\n");
    return 1;
  }
  fseek(file, 0, SEEK_END);
  romsize = ftell(file);
  if ((romsize != 4096) && (romsize != 8192))
  {
    fclose(file);
    fprintf(stderr, "FATAL: Size of ROM file is incorrect\n"
                    "  (expected size is 4096 or 8192 bytes)\n");

    return 2;
  }
  fseek(file, 0, SEEK_SET);
  e = fread(ROM, 1, romsize, file);
  if (e < romsize)
  {
    fprintf(stderr, "WARNING: Short read on ROM file\n"
                    "  (got %d bytes, expected %u)\n",
            e, romsize);
  }
  fclose(file);
  return 0;
}

void audio_callback(void*  userdata, Uint8* stream, int len)
{
  int i;
  int16_t sample;
  for (i = 0; i < len; i += 2) {
    sample = PSG_calc(psg);
    stream[i] = sample & 0xff;
    stream[i + 1] = sample >> 8;
  }
}

int main(int argc, char **argv)
{
  int e;

  char *bios;
  char *server, *port;

  SDL_version sdlver;
  int scanline;

  SDL_GetVersion(&sdlver);

  server = "127.0.0.1";
  port = "5816";
  bios = ROMFILE1;
  while (-1 != (e = getopt(argc, argv, "48B:S:P:")))
  {
    switch (e)
    {
    case '4':
      bios = ROMFILE1;
      break;
    case '8':
      bios = ROMFILE2;
      break;
    case 'B':
      bios = optarg;
      break;
    case 'S':
      server = optarg;
      break;
    case 'P':
      port = optarg;
      break;
    default:
      fprintf(stderr, 
              "usage: %s [-4 | 8 | -B filename] [-S server] [-P port]\n",
              argv[0]);
      return 1;
    }
  }

  /* Copyrights for all components */
  printf("Marduk version " VERSION " NABU Emulator\n"
         "  Copyright 2022, 2023 S. V. Nickolas.\n"
         "  Copyright 2023 Marcin Wołoszczuk.\n"
         "  Z80 emulation code copyright 2019 Nicolas Allemand.\n"
         "  Includes vrEmuTms9918 copyright 2021, 2022 Troy Schrapel.\n"
         "  Includes emu2149 copyright 2001-2022 Mitsutaka Okazaki.\n");
  printf("  Uses SDL %u.%u.%u.  See documentation for copyright details.\n",
         sdlver.major, sdlver.minor, sdlver.patch);
  printf("  All third-party code is used under license.  "
         "See license.txt for details.\n\n");

  /* Only used for the white noise generator; a good RNG isn't necessary. */
  srand(time(0));

  /*
   * Get SDL2 up and running.
   *
   * First, initalize the library.  Then create our window, renderer, blit
   * surface, and allocate memory for our offscreen buffer.
   *
   * If any of this fails, die screaming.
   */
  if (SDL_Init(SDL_INIT_EVERYTHING))
  {
    fprintf(stderr, "FATAL: Could not start SDL\n");
    return 2;
  }

  screen = SDL_CreateWindow("Marduk", SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, 640, 480, 0);
  if (!screen)
  {
    fprintf(stderr, "FATAL: Could not create display\n");
    return 2;
  }
  renderer = SDL_CreateRenderer(screen, -1, 0);
  if (!renderer)
  {
    fprintf(stderr, "FATAL: Could not set up renderer\n");
    return 2;
  }
  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, 640, 480);
  if (!texture)
  {
    fprintf(stderr, "FATAL: Could not create canvas\n");
    return 2;
  }
  display = calloc(307200, sizeof(uint32_t));
  if (!display)
  {
    fprintf(stderr, "FATAL: Not enough memory for offscreen buffer\n");
    return 2;
  }

  /* audio stuff */
  SDL_zero(audio_spec);
  audio_spec.freq = 44100;
  audio_spec.format = AUDIO_S16LSB;
  audio_spec.channels = 1;
  audio_spec.samples = 512;
  audio_spec.callback = audio_callback;

  audio_device = SDL_OpenAudioDevice(NULL, 0, &audio_spec, NULL, 0);
  SDL_PauseAudioDevice(audio_device, 0);
  /*
   * Set up the VDP emulation.
   *
   * If it fails, die screaming.
   */
  vdp = vrEmuTms9918New();
  if (!vdp)
  {
    fprintf(stderr, "FATAL: Could not set up VDP emulation\n");
    return 3;
  }
  vrEmuTms9918Reset(vdp);

  /*
   * Set up the PSG emulation.
   *
   * If it fails, die screaming.
   */
  psg = PSG_new(1789772, 44100);
  if (!psg)
  {
    fprintf(stderr, "FATAL: Could not set up PSG emulation\n");
    return 4;
  }
  PSG_setVolumeMode(psg, 2);
  PSG_reset(psg);

  /*
   * Load the ROM, then set it visible.
   */
  if (init_rom(bios))
    return 1;
  printf("ROM size: %u KB\n", romsize >> 10);

  /*
   * Set up the modem.
   *
   * modem_init() returns 0=success, -1=failure, but our internal flag needs
   * 1=success, 0=failure so use ! to quickly make that change.
   */
  e = modem_init(server, port);
  if (e)
  {
    fprintf(stderr, "Modem will not be available.\n");
  }
  gotmodem = !e;

  /*
   * The first thing the ROM does is initialize the control register, which
   * will flick off the lights and unset TV mode - we intentionally set them on
   * as the initial status.
   */
  ctrlreg = 0x3A;
  printf("Emulation ready to start\n");

  /*
   * Get ready to start the emulated Z80.
   *
   * Timings are for a 3.58 (ish) MHz CPU on an NTSC signal.  This is natural
   * because Canada uses the same video standards as the United States.
   */
  init_cpu();

  death_flag = scanline = 0;
  clock_gettime(CLOCK_REALTIME, &timespec);
  next_fire = timespec.tv_nsec + FIRE_TICK;
  next_watchdog = 0;
  keyjoy = joybyte = 0;
  int i;

  int16_t sound_sample = 0;

  while (!death_flag)
  {
    if (cpu.cyc > next)
    {
      /* if there are bytes available in the modem,
       generate the buffer ready interrupt */
      if (modem_bytes_available())
      {
        hccarint = 1;
        update_interrupts();
      }

      if (!keyboard_buffer_empty() && !keybdint) 
      {
        keybdint = 1;
        update_interrupts();
        // printf("KEYBOARD: int! %d %d\r\n", keyboard_buffer_read_ptr, keyboard_buffer_write_ptr);
      }

      every_scanline();

      /* ready to kick the dog? */
      if (keyboard_buffer_empty())
      {
        next_watchdog++;
        if (next_watchdog >= 58000)
        {
          next_watchdog = 0;
          printf("Keyboard: kicking the dog\n");
          keyboard_buffer_put(0x94);
        }
      }
      else
        next_watchdog = 0;
      scanline++;
      if (scanline < 240)
        render_scanline(scanline);
      if (scanline > 261)
      {
        scanline = 0;
        next_frame();

        if (vrEmuTms9918RegValue(vdp, TMS_REG_1) & 0x20)
        {
          if (vdpint == 0)
          {
            vdpint = 1;
            update_interrupts();
            // printf("vdpint!\r\n");
          }
        }
      }
      next += 228;
    }
    z80_step(&cpu);
  }

  /* Clean up and exit properly. */
  printf("Shutting down emulation\n");
  if (gotmodem)
    modem_deinit();
  PSG_delete(psg);
  vrEmuTms9918Destroy(vdp);
  free(display);
  SDL_Quit();

  return 0;
}
