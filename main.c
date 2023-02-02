/*
 * Copyright 2022 S. V. Nickolas.
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

#define VERSION "0.10"

/*
 * Forward declaration.
 */
static void reinit_cpu (void);

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
int next_key;

int gotmodem;

/* Next cycle for scanline loop. */
unsigned long next;

/* Set to nonzero to tell the emulator to exit. */
int death_flag;

/* To kick the dog */
unsigned next_watchdog;

/*
 * SDL2 structure pointers.
 *
 * display is a dynamically allocated offscreen buffer used for rendering the
 * VDP data, or the generated NTSC noise.
 */
SDL_Window *screen;
SDL_Renderer *renderer;
SDL_Texture *texture;

uint32_t *display;

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

uint8_t mem_read (void *blob, uint16_t addr)
{
 if ((!(ctrlreg&0x01)) && (addr<romsize)) return ROM[addr];

 return RAM[addr];
}

void mem_write (void *blob, uint16_t addr, uint8_t val)
{
 RAM[addr]=val;
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

void int_prio_enc(int EI, int I0, int I1, int I2, int I3, int I4, int I5, int I6, int I7,
    int *GS, int *Q0, int *Q1, int *Q2, int *EO)
{
  /* most often and default values, ease the typing later on ;) */
  *GS = 0;
  *EO = 1;
  *Q0 = *Q1 = *Q2 = 0;
  /* external input is high, case (a) */
  if (EI == 1) { *GS = 1; *Q0 = 1; *Q1 = 1; *Q2 = 1;}
  /* external input is low, inputs 0-7 are high, case (b) */
  else if (I0 & I1 & I2 & I3 & I4 & I5 & I6 & I7) {
      *GS = *Q0 = *Q1 = *Q2 = 1; *EO = 0;
  }
  /* I7 is low, case (c) */
  else if (!I7) { /* nop */ }
  /* I6 is low, case (d) */
  else if (!I6) { *Q0 = 1; }
  /* I5 is low, case (e) */
  else if (!I5) { *Q1 = 1; }
  /* I4 is low, case (f) */
  else if (!I4) { *Q0 = *Q1 = 1; }
  /* I3 is low, case (g) */
  else if (!I3) { *Q2 = 1; }
  /* I2 is low, case (h) */
  else if (!I2) { *Q0 = *Q2 = 1; }
  /* I1 is low, case (i) */
  else if (!I1) { *Q1 = *Q2 = 1; }
  /* I0 is low (no other choice), case (j) */
  else { *Q0 = *Q1 = *Q2; }
}

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

uint8_t hccarint = 0;
uint8_t hccatint = 0;
uint8_t keybdint = 0;
uint8_t vdpint = 0;
uint8_t interrupts = 0x40;

void update_interrupts()
{
  int int_prio = ~(interrupts & psg_porta);
  int GS, Q0, Q1, Q2, EO;
  int_prio_enc_alt(0, interrupts, &GS, &Q0, &Q1, &Q2, &EO);
  psg_portb &= 0xf0;
  psg_portb |= EO | (Q0 << 1) | (Q1 << 2) | (Q2 << 3);
  PSG_writeReg(psg, 15, psg_portb);
  if (!GS) {
    printf("Z80 INTERRUPT\r\n");
    z80_gen_int(&cpu, (Q0 << 5) | (Q1 << 6) | (Q2 << 7));
  }
}

uint8_t tmp_psg_address = 0x00;

uint8_t port_read (z80 *mycpu, uint8_t port)
{
 uint8_t t;

 switch (port)
 {
  case 0x40: /* read register from PSG */
   t = PSG_readReg(psg, tmp_psg_address);
   //printf("PSG read %02X from %02X\r\n", t, tmp_psg_address);
   return t; 
  case 0x41:
   /* manual assert, WIP */
   printf("WTF read from 0x41\r\n");
   exit(-1);
   return 0;
  case 0x80:
   return gotmodem?modem_read():0;
  case 0x90: /* Not sure if this is the right action */
   printf("KEYBOARD DATA READ\r\n");
   t=next_key;
   next_key=0;
   keybdint = 0;
   update_interrupts();
   if (t==255) return 0; else return t;
  case 0x91: /* Not sure if this is the right action */
  printf("KEYBOARD STATUS READ\r\n");
   return next_key?0xFF:0x00;
  case 0xA0:
   return vrEmuTms9918ReadData(vdp);
  case 0xA1: /* Not sure if this is the right action */
   return vrEmuTms9918ReadStatus(vdp);
  default:
#ifdef PORT_DEBUG
   printf ("WARNING: unknown port read (0x%02X)\n", port);
#endif
   return 0;
 }
}

void port_write (z80 *mycpu, uint8_t port, uint8_t val)
{
 switch (port)
 {
  case 0x00:
   ctrlreg=val;
   return;
  case 0x40: /* write data to PSG */
   //printf("PSG writing %02X to %02X\r\n", val, tmp_psg_address);
   if (tmp_psg_address == 0x0E) {
    if (psg_porta != val) {
      psg_porta = val;
      update_interrupts();
    }
   }
   PSG_writeReg(psg, tmp_psg_address, val);
   return;
  case 0x41: /* write address to PSG */
   tmp_psg_address = val;
   if (val == 0x07) {
    //printf("PSG ADDR - IO PORT SETTINGS and others...\r\n");
   }
   else if (val == 0x0E) {
    //printf("PSG ADDR - PORT A\r\n");
   }
   else if (val == 0x0F) {
    //printf("PSG ADDR - PORT B\r\n");
   }
   else {
    //printf("PSG ADDR %02X\r\n", val);
   }
   /* manual assert for the moment, WIP */
   if (val > 0x1f) {
    printf("WTF PSG address write > 0x1f\r\n");
    exit(-1);
   }
   //printf("PORT WRITE 0x%02X = 0x%02X\r\n", port, val);
   return;
  case 0x80:
   if (gotmodem) modem_write(val);
   return;
  case 0xA0:
   vrEmuTms9918WriteData(vdp, val);
   return;
  case 0xA1:
   vrEmuTms9918WriteAddr(vdp, val);
   return;
#ifdef PORT_DEBUG
  default:
   printf ("WARNING: unknown port write (0x%02X): 0x%02X\n", port, val);
#endif
 }
}

/*
 * Things to do once per scanline, like poll the keyboard, joystick, etc.
 */
void every_scanline (void)
{
 SDL_Event event;
 struct timespec n;

 clock_gettime(CLOCK_REALTIME, &timespec);
 n.tv_sec=0;
 n.tv_nsec=next_fire-timespec.tv_nsec;
 next_fire=timespec.tv_nsec+FIRE_TICK;
 if (next_fire>n.tv_nsec) nanosleep(&n, 0);

 while (SDL_PollEvent(&event))
 {
  switch (event.type)
  {
   /*
    * "Break key" codes for arrows.
    */
   case SDL_KEYUP:
    keybdint = 1;
    update_interrupts();
    switch (event.key.keysym.sym)
    {
     case SDLK_UP:
      next_key=0xF2;
      break;
     case SDLK_DOWN:
      next_key=0xF3;
      break;
     case SDLK_LEFT:
      next_key=0xF1;
      break;
     case SDLK_RIGHT:
      next_key=0xF0;
      break;
     case SDLK_HOME: /* « */
      next_key=0xF5;
      break;
     case SDLK_END: /* » */
      next_key=0xF4;
      break;
     case SDLK_INSERT: /* YES */
      next_key=0xF7;
      break;
     case SDLK_DELETE: /* NO */
      next_key=0xF6;
      break;
     case SDLK_PAUSE:
      next_key=0xF9;
      break;
     case SDLK_PAGEDOWN:
      next_key=0xFA;
      break;
    }
    break;
   case SDL_KEYDOWN:
    if (event.key.keysym.sym<128)
    {
     static char *shiftnums=")!@#$%^&*(";
     SDL_Keymod m;
     int k;
     keybdint = 1;
     update_interrupts();
     /*
      * "Make key" codes for arrows.
      */
     switch (event.key.keysym.sym)
     {
      case SDLK_UP:
       next_key=0xE2;
       break;
      case SDLK_DOWN:
       next_key=0xE3;
       break;
      case SDLK_LEFT:
       next_key=0xE1;
       break;
      case SDLK_RIGHT:
       next_key=0xE0;
       break;
      case SDLK_HOME: /* « */
       next_key=0xE5;
       break;
      case SDLK_END: /* » */
       next_key=0xE4;
       break;
      case SDLK_INSERT: /* YES */
       next_key=0xE7;
       break;
      case SDLK_DELETE: /* NO */
       next_key=0xE6;
       break;
      case SDLK_PAUSE:
       next_key=0xE9;
       break;
      case SDLK_PAGEDOWN:
       next_key=0xEA;
       break;
     }

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

     k=event.key.keysym.sym;
     m=SDL_GetModState();
     if (m&KMOD_CTRL)
     {
      if (k=='[') k=0x1B;
      if (k=='\\') k=0x1C;
      if (k==']') k=0x1D;
      if (k=='-') k=0x1F;
     }
     if (m&KMOD_SHIFT)
     {
      if (k=='`') k='~';
      if (k=='-') k='_';
      if (k=='=') k='+';
      if (k=='[') k='{';
      if (k==']') k='}';
      if (k=='\\') k='|';
      if (k==';') k=':';
      if (k=='\'') k='"';
      if (k==',') k='<';
      if (k=='.') k='>';
      if (k=='/') k='?';
     }
     if ((k>='a')&&(k<='z'))
     {
      if (m&KMOD_CAPS) k^=32;
      if (m&KMOD_SHIFT) k^=32;
      if (m&KMOD_CTRL) k&=0x1F;
     }
     else if ((k>='0')&&(k<='9'))
     {
      if (m&KMOD_CTRL)
      {
       if (k=='2')
        k=0xFF; /* interpreted as 0x00 when read */
       else if (k=='6')
        k=0x1E;
      }
      else
      {
       if (m&KMOD_SHIFT) k=shiftnums[k&0x0F];
      }
     }
     next_key=k;
    }
    else switch (event.key.keysym.sym)
    {
     /* F3 - reset */
     case SDLK_F3:
      printf ("Reset pressed\n");
      clock_gettime(CLOCK_REALTIME, &timespec);
      next_fire=timespec.tv_nsec+FIRE_TICK;
      reinit_cpu();
      break;
     /* Alt-F4 - exit */
     case SDLK_F4:
      if (SDL_GetModState()&KMOD_ALT) death_flag=1;
      break;
     /* F10 - also exit */
     case SDLK_F10:
      death_flag=1;
      break;
    }
    break;
   case SDL_QUIT:
    death_flag=1;
    break;
  }
 }
}

/*
 * Exactly what it says on the tin.
 * 
 * Call the TMS9918 emulator to generate the next scanline into the offscreen.
 */
void render_scanline (int line)
{
 int x;
 uint32_t r;
 uint32_t bg;
 uint8_t a_scanline[256];
 uint32_t g_scanline[320];
 if (line>239) return;

 /*
  * To note:
  *
  * The background color is register 7, AND 0x0F.
  * The border is 64 pels left and right, 48 top and bottom, thus 512x384 in a
  * 640x480 window.
  *
  * The palette is stored RGBA, but we use ARGB; accomodate it.
  */
 bg=0xFF000000 | (vrEmuTms9918Palette[vrEmuTms9918RegValue(vdp, 7)&0x0F]>>8);
 for (x=0; x<320; x++) g_scanline[x]=bg;
 if ((line>=24)&&(line<216))
 {
  vrEmuTms9918ScanLine (vdp, line-24, a_scanline);
  for (x=0; x<256; x++)
   g_scanline[x+32]=vrEmuTms9918Palette[a_scanline[x]]>>8;
 }

 /* Double-scan. */
 r=line*1280;
 for (x=0; x<320; x++)
 {
  display[r+(x<<1)]=display[r+1+(x<<1)]=
   display[r+640+(x<<1)]=display[r+641+(x<<1)]=g_scanline[x];
 }

 /*
  * If the display is in "TV" mode, just spew some NTSC noise into the buffer.
  *
  * This actually looks pretty realistic (I grew up in the days of aerials and
  * 3 major TV networks, and am well acquainted with the appearance of NTSC
  * noise).
  */
 if (!(ctrlreg&0x02))
 {
  uint32_t c;

  r=line*1280;
  for (x=0; x<1280; x++)
  {
   c=rand()&0xFF;
   display[r+x]=0xFF000000 | (c<<16) | (c<<8) | (c);
  }
 }

 /*
  * Draw the LEDs.
  *
  * They will appear in the bottom right corner, in the order in which
  * they appear on the system unit.  The current code generates a sort of
  * rounded or "chewed-out" rectangle.  Not efficient code.
  */
 if ((line>=232)&&(line<236))
 {
  uint32_t le[3], ri[3];

  r=line*1280;

  if (line==232)
  {
   le[0]=display[r+592];  ri[0]=display[r+599];
   le[1]=display[r+608];  ri[1]=display[r+615];
   le[2]=display[r+624];  ri[2]=display[r+631];
  }
  else if (line==235)
  {
   le[0]=display[r+640+592];  ri[0]=display[r+640+599];
   le[1]=display[r+640+608];  ri[1]=display[r+640+615];
   le[2]=display[r+640+624];  ri[2]=display[r+640+631];
  }

  for (x=592; x<600; x++) /* Yellow LED */
   display[r+x]=display[r+640+x]=(ctrlreg&0x20)?0xFFFFFF00:0;
  for (x=608; x<616; x++) /* Red LED */
   display[r+x]=display[r+640+x]=(ctrlreg&0x10)?0xFFFF0000:0;
  for (x=624; x<632; x++) /* Green LED */
   display[r+x]=display[r+640+x]=(ctrlreg&0x08)?0xFF00FF00:0;

  if (line==232)
  {
   display[r+592]=le[0];  display[r+599]=ri[0];
   display[r+608]=le[1];  display[r+615]=ri[1];
   display[r+624]=le[2];  display[r+631]=ri[2];
  }
  else if (line==235)
  {
   display[r+640+592]=le[0];  display[r+640+599]=ri[0];
   display[r+640+608]=le[1];  display[r+640+615]=ri[1];
   display[r+640+624]=le[2];  display[r+640+631]=ri[2];
  }
 }
}

/*
 * End of frame.  Blit it out.
 * 
 * Also for anything that needs done every 1/60 second.
 */
void next_frame (void)
{
 SDL_UpdateTexture(texture, 0, display, 640*sizeof(uint32_t));
 SDL_RenderClear(renderer);
 SDL_RenderCopy(renderer, texture, 0, 0);
 SDL_RenderPresent(renderer);
}

/*
 * Set up the CPU emulation.
 *
 * Initialize the function pointers and call the CPU emulator's init code.
 */
static void init_cpu (void)
{
 z80_init (&cpu);
 cpu.read_byte = mem_read;
 cpu.write_byte = mem_write;
 cpu.port_in = port_read;
 cpu.port_out = port_write;
 next=228;
 next_key=0x95;
}

/*
 * Reset the CPU emulation.
 */
static void reinit_cpu (void)
{
 void *tmp;

 tmp=cpu.userdata;
 init_cpu();
 cpu.userdata=tmp;
}

/*
 * Open ROM.
 */
static int init_rom (char *filename)
{
 int e;
 FILE *file;

 file=fopen(filename, "rb");
 if (!file)
 {
  fprintf (stderr, "FATAL: Failed to open ROM file\n");
  return 1;
 }
 fseek(file, 0, SEEK_END);
 romsize=ftell(file);
 if ((romsize!=4096)&&(romsize!=8192))
 {
  fclose(file);
  fprintf (stderr, "FATAL: Size of ROM file is incorrect\n"
           "  (expected size is 4096 or 8192 bytes)\n");

  return 2;
 }
 fseek(file, 0, SEEK_SET);
 e=fread(ROM, 1, romsize, file);
 if (e<romsize)
 {
  fprintf (stderr, "WARNING: Short read on ROM file\n"
           "  (got %d bytes, expected %u)\n", e, romsize);
 }
 fclose(file);
 return 0;
}

int main (int argc, char **argv)
{
 int e;

 char *bios;

 SDL_version sdlver;
 int scanline;

 SDL_GetVersion(&sdlver);
 
 bios=ROMFILE1;
 while (-1!=(e=getopt(argc, argv, "48B:")))
 {
  switch (e)
  {
   case '4':
    bios=ROMFILE1;
    break;
   case '8':
    bios=ROMFILE2;
    break;
   case 'B':
    bios=optarg;
    break;
   default:
    fprintf (stderr, "usage: %s [-4 | 8 | -B filename]\n", argv[0]);
    return 1;
  }
 }

 /* Copyrights for all components */
 printf ("Marduk version " VERSION " NABU Emulator\n"
         "  Copyright 2022 S. V. Nickolas.\n"
         "  Z80 emulation code copyright 2019 Nicolas Allemand.\n"
         "  Includes vrEmuTms9918 copyright 2021, 2022 Troy Schrapel.\n"
         "  Includes emu2149 copyright 2001-2022 Mitsutaka Okazaki.\n");
 printf ("  Uses SDL %u.%u.%u.  See documentation for copyright details.\n",
         sdlver.major, sdlver.minor, sdlver.patch);
 printf ("  All third-party code is used under license.  "
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
  fprintf (stderr, "FATAL: Could not start SDL\n");
  return 2;
 }

 screen=SDL_CreateWindow("Marduk", SDL_WINDOWPOS_UNDEFINED,
                         SDL_WINDOWPOS_UNDEFINED, 640, 480, 0);
 if (!screen)
 {
  fprintf (stderr, "FATAL: Could not create display\n");
  return 2;
 }
 renderer=SDL_CreateRenderer(screen, -1, 0);
 if (!renderer)
 {
  fprintf (stderr, "FATAL: Could not set up renderer\n");
  return 2;
 }
 texture=SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                           SDL_TEXTUREACCESS_STREAMING, 640, 480);
 if (!texture)
 {
  fprintf (stderr, "FATAL: Could not create canvas\n");
  return 2;
 }
 display=calloc(307200, sizeof(uint32_t));
 if (!display)
 {
  fprintf (stderr, "FATAL: Not enough memory for offscreen buffer\n");
  return 2;
 }

 /*
  * Set up the VDP emulation.
  *
  * If it fails, die screaming.
  */
 vdp=vrEmuTms9918New();
 if (!vdp)
 {
  fprintf (stderr, "FATAL: Could not set up VDP emulation\n");
  return 3;
 }
 vrEmuTms9918Reset(vdp);

 /*
  * Set up the PSG emulation.
  *
  * If it fails, die screaming.
  */
 psg=PSG_new(1789772, 44100);
 if (!psg)
 {
  fprintf (stderr, "FATAL: Could not set up PSG emulation\n");
  return 4;
 }
 PSG_setVolumeMode(psg, 2);
 PSG_reset(psg);
 psg->reg[15] = 0xf0;

 /*
  * Load the ROM, then set it visible.
  */
 if (init_rom(bios)) return 1;
 printf ("ROM size: %u KB\n", romsize>>10);
 
 /*
  * Set up the modem.
  * 
  * modem_init() returns 0=success, -1=failure, but our internal flag needs
  * 1=success, 0=failure so use ! to quickly make that change.
  */
 e=modem_init();
 if (e)
 {
  fprintf (stderr, "Modem will not be available.\n");
 }
 gotmodem=!e;

 /*
  * The first thing the ROM does is initialize the control register, which
  * will flick off the lights and unset TV mode - we intentionally set them on
  * as the initial status.
  */ 
 ctrlreg=0x3A;
 printf ("Emulation ready to start\n");

 /*
  * Get ready to start the emulated Z80.
  *
  * Timings are for a 3.58 (ish) MHz CPU on an NTSC signal.  This is natural
  * because Canada uses the same video standards as the United States.
  */
 init_cpu();

 death_flag=scanline=0;
 clock_gettime(CLOCK_REALTIME, &timespec);
 next_fire=timespec.tv_nsec+FIRE_TICK;
 next_watchdog=0;

 /*
  volatile for the moment, we don't use it, but want to call PSG_calc()
 */
 volatile uint16_t sound_sample = 0;

 while (!death_flag)
 {
  if (cpu.cyc>next)
  {
   every_scanline();
   
   /* ready to kick the dog? */
   if (!next_key)
   {
    next_watchdog++;
    if (next_watchdog>=58000)
    {
     next_watchdog=0;
     printf ("Keyboard: kicking the dog\n");
     next_key=0x94;
    }
   }
   else next_watchdog=0;
   scanline++;
   if (scanline<240)
    render_scanline(scanline);
   if (scanline>261)
   {
    scanline=0;
    
    next_frame();
   }
   next+=228;
  }
  sound_sample = PSG_calc(psg);
  z80_step(&cpu);
 }

 /* Clean up and exit properly. */
 printf ("Shutting down emulation\n");
 if (gotmodem) modem_deinit();
 PSG_delete(psg);
 vrEmuTms9918Destroy(vdp);
 free(display);
 SDL_Quit();

 return 0;
} 
