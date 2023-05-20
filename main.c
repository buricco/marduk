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
 */

#define VERSION "0.26e"

/* C99 includes */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * MS-DOS (DJGPP):
 *   Use BIOS to access hardware, redirected through DPMI
 *   XXX: we need to completely disable sigint, sigquit etc. in DJGPP
 * 
 * Otherwise:
 *   Use SDL for hardware abstraction
 */
#ifdef __MSDOS__
#include <dpmi.h>
#include <pc.h>
#include <sys/nearptr.h>
#define diag_printf(...)
#else
/* SDL2 include */
#include <SDL.h>

/* XXX: and for OSX we use...? */
#if (!defined(_WIN32))&&(!defined(__APPLE__))
#include <gtk/gtk.h>
#endif
#define diag_printf printf
#endif

#ifdef _WIN32
#include <windows.h>
#endif

/* Chipset includes */
#include "tms9918.h"
#include "tms_util.h"
#include "emu2149.h"
#include "z80.h"

/* FDC include */
#include "disk.h"

/* Cable modem include */
#include "modem.h"

/* Alterable filenames */
#include "paths.h"

/*
 * Forward declarations.
 */
static void reinit_cpu(void);
void fatal_diag(int, char *);

/* Extern declaration */
void cpustatus (z80 *cpu);

int trace;

/*
 * Speed control.
 *
 * Not exact, but it helps keep the speed more or less even based on how long
 * it takes to run one scanline's worth of code.
 * 
 * XXX: no support for this on MS-DOS.
 */
#ifdef __MSDOS__

#else
#define FIRE_TICK 63492
unsigned long long next_fire;
struct timespec timespec;
#endif

#ifdef _WIN32
LARGE_INTEGER currenttime,throttlerate;
long long wantedtime, looptimedesired;
#endif

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
unsigned dog_speed;

/* Next cycle for scanline loop. */
unsigned long next;

/* Set to nonzero to tell the emulator to exit. */
int death_flag;

/* To kick the dog */
unsigned next_watchdog;

/* keyboard/joystick? */
int keyjoy;
uint8_t joybyte;
#define JOY_THRESH 2048 /* distance from center to "trip"; 0..32767 */

int dojoy;
void add_gamecontroller(int joystick_index);

FILE *lpt;
uint8_t lpt_data;

#ifdef __MSDOS__
/*
 * display is an offscreen buffer which is blitted to the screen every frame.
 * vgamem is a pointer to 0x000A0000 absolute, translated through the DJGPP
 * extender.
 */
uint8_t *display, *vgamem;
int ttyup;
#else
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
SDL_GameController *pad;
SDL_Joystick *joystick;

uint32_t *display;
#endif

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
    *Q0 = *Q1 = *Q2 = 1;
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
  z80_gen_int(&cpu, !GS, psg_portb & 0x0e);
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
  
  if ((port&0xF0)==0xC0) return disksys_read(port);

  switch (port)
  {
  case 0x40: /* read register from PSG */
    t = PSG_readReg(psg, psg_reg_address);
    return t;
  case 0x41:
    fatal_diag(-1, "IO read from 0x41, this shouldn't happen, exiting!");
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

  if ((port&0xF0)==0xC0) disksys_write(port, val);

  switch (port)
  {
  case 0x00:
    if ((val&0x04)&&(!(ctrlreg&0x04))&&(lpt)) fputc(lpt_data, lpt);
    ctrlreg = val;
    return;
  case 0x40: /* write data to PSG */
    psg_reg7 = PSG_readReg(psg, 7);
    if (psg_reg_address == 0x0E)
    {
      if (!(psg_reg7 & 0x40))
      {
        diag_printf("Writing to PORTA when it's set to input, DENIED!\r\n");
        diag_printf("psg_reg7 = %02X\r\n", psg_reg7);
      }
      if (val & 0x10)
      {

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
        diag_printf("Writing to PORTB when it's set to input, DENIED!\r\n");
        diag_printf("psg_reg7 = %02X\r\n", psg_reg7);
      }
    }
    PSG_writeReg(psg, psg_reg_address, val);
    return;
  case 0x41: /* write address to PSG */
    if (val > 0x1f)
    {
      fatal_diag(-1, "PSG reg address > 0x1f when writing, exiting!");
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
  case 0xB0:
    if (lpt) lpt_data=val;
    return;
#ifdef DEBUG
  case 0xBF: /* debug port */
    trace=val;
    return;
#endif
#ifdef PORT_DEBUG
  default:
    printf("WARNING: unknown port write (0x%02X): 0x%02X\n", port, val);
#endif
  }
}

#ifdef __MSDOS__

/*
 * This is a cruder version of the SDL code found below.  Many comments from
 * that version will still be relevant here.
 * 
 * XXX: We need a better way to do this to detect make and break from the 
 *      relevant keys.  Like, say, grabbing INT9.  (But I'm not familiar with
 *      that kind of stuff in 386 mode... -uso.)
 */
void keyboard_poll(void)
{
 __dpmi_regs regs;
 uint16_t k;
 
 /* Key in the buffer?  If not, do nothing */
 regs.h.ah=0x01;
 __dpmi_int(0x16, &regs);
 if (regs.x.flags&0x40) return; /* Z */
 
 regs.h.ah=0x00;
 __dpmi_int(0x16, &regs);
 k=regs.x.ax;
 
 if (keyjoy&&((k&0xFF)==0x20))
 {
 }
 
 if (k==0x0E08) /* BkSp */
 {
  keyboard_buffer_put(0x7F);
  return;
 }

 /* Plain, ordinary, ASCII */
 if ((k&0xFF)&&(!(k&0x80)))
 {
  keyboard_buffer_put(k&0x7F);
  return;
 }
 
 switch (k>>8)
 {
  case 0x48: /* up */
   if (keyjoy)
   {
   }
   else
   {
    keyboard_buffer_put(0xE2);
    keyboard_buffer_put(0xF2);
   }
   break;
  case 0x50: /* down */
   if (keyjoy)
   {
   }
   else
   {
    keyboard_buffer_put(0xE3);
    keyboard_buffer_put(0xF3);
   }
   break;
  case 0x4B: /* left */
   if (keyjoy)
   {
   }
   else
   {
    keyboard_buffer_put(0xE1);
    keyboard_buffer_put(0xF1);
   }
   break;
  case 0x4D: /* right */
   if (keyjoy)
   {
   }
   else
   {
    keyboard_buffer_put(0xE0);
    keyboard_buffer_put(0xF0);
   }
   break;
  case 0x49: /* PgUp */
   keyboard_buffer_put(0xE5);
   keyboard_buffer_put(0xF5);
   break;
  case 0x51: /* PgDn */
   keyboard_buffer_put(0xE4);
   keyboard_buffer_put(0xF4);
   break;
  case 0x52: /* Ins */
   keyboard_buffer_put(0xE7);
   keyboard_buffer_put(0xF7);
   break;
  case 0x53: /* Del */
   keyboard_buffer_put(0xE6);
   keyboard_buffer_put(0xF6);
   break;
  case 0x4F: /* End */
   keyboard_buffer_put(0xEA);
   keyboard_buffer_put(0xFA);
   break;
  
  /* Special Keys */
  case 0x3D: /* F3 */
#if 0 /* DJGPP doesn't have this */
   clock_gettime(CLOCK_REALTIME, &timespec);
   next_fire = timespec.tv_nsec + FIRE_TICK;
#endif
   reinit_cpu();
   break;
  case 0x40: /* F6 */
   keyjoy=!keyjoy;
   break;
 }
 
 if (k==0x4400) death_flag=1;
}
#else
void add_gamecontroller(int joystick_index)
{
    if (joystick != NULL)
      return;
    
    printf("Controller %d added: %s\n", joystick_index, SDL_JoystickNameForIndex(joystick_index));
    pad=SDL_GameControllerOpen(joystick_index);
    joystick=SDL_GameControllerGetJoystick(pad);
    SDL_JoystickEventState(SDL_ENABLE);
    SDL_GameControllerEventState(SDL_ENABLE);
}

void remove_gamecontroller(int joystick_index, bool last)
{
  printf("Controller %d removed\n", joystick_index);
  SDL_JoystickClose(joystick);
  SDL_GameControllerClose(pad);
  joystick = NULL;
  if (last)
  {
    /* .... No more rainbows for us to chase .... */
    /* .... No more time to playyyyyyyyyyyyy .... */
    SDL_GameControllerEventState(SDL_DISABLE);
    SDL_JoystickEventState(SDL_DISABLE);
  }
}

void send_joybyte() {
  keyboard_buffer_put(0x80);
  keyboard_buffer_put(joybyte|0xA0);
}

void keyboard_poll(void)
{
  SDL_Event event;
  /* eat up all events */
  while (SDL_PollEvent(&event))
  {
    /* These are irrelevant if the keyboard is emulating the joystick */
    if (!keyjoy)
    {
     /* Don't care what stick or what button.  Nabu only has one. */
     switch (event.type)
     {
      /* All new controller fancy: */
      case SDL_CONTROLLERDEVICEADDED:
        add_gamecontroller(event.jdevice.which);
        break;
      case SDL_CONTROLLERDEVICEREMOVED:
        remove_gamecontroller(event.jdevice.which, SDL_NumJoysticks()==0);
        break; 
      
      
      case SDL_JOYBUTTONDOWN:
        joybyte|=0x10;
        send_joybyte();
        break;
        

      case SDL_JOYBUTTONUP:
        joybyte&=0xEF;
        send_joybyte();
        break;

      case SDL_JOYHATMOTION:
        joybyte&=0xF0;
        switch (event.jhat.value)
        {
          case SDL_HAT_LEFTUP:
            joybyte|=0x09;
            break;
          case SDL_HAT_UP:
            joybyte|=0x08;
            break;
          case SDL_HAT_RIGHTUP:
            joybyte|=0x0C;
            break;
          case SDL_HAT_LEFT:
            joybyte|=0x01;
            break;
          case SDL_HAT_CENTERED:
            /* Already done */
            break;
          case SDL_HAT_RIGHT:
            joybyte|=0x04;
            break;
          case SDL_HAT_LEFTDOWN:
            joybyte|=0x03;
            break;
          case SDL_HAT_DOWN:
            joybyte|=0x02;
            break;
          case SDL_HAT_RIGHTDOWN:
            joybyte|=0x06;
            break;
        }
        send_joybyte();
        break;
      
      case SDL_JOYAXISMOTION:
       joybyte&=0xF0;
       switch (event.caxis.axis)
       {
        case 0: /* X */
         if (event.jaxis.value<-JOY_THRESH)
          joybyte|=0x01;
         else if (event.jaxis.value>JOY_THRESH)
          joybyte|=0x04;
         break;
        case 1: /* Y */
         if (event.jaxis.value<-JOY_THRESH)
          joybyte|=0x08;
         else if (event.jaxis.value>JOY_THRESH)
          joybyte|=0x02;
         break;
       }
       send_joybyte();
       break;
      /* END new controller fancy */

     }
     
    }
    switch (event.type)
    {
    /*
     * "Break key" codes for arrows.
     */
    case SDL_KEYUP:
      switch (event.key.keysym.sym)
      {
       case SDLK_LALT: /* Alt for Sym */
       case SDLK_RALT:
        keyboard_buffer_put(0xF8);
        break;
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
       case SDLK_LALT: /* Alt for Sym */
       case SDLK_RALT:
        keyboard_buffer_put(0xE8);
        break;
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
      if ((event.key.keysym.sym < 128) && 
          (event.key.keysym.sym != SDLK_BACKSPACE)) /* urk */
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
        if ((k==' ')&&keyjoy) break; /* we already handled this. */
        
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
        case SDLK_F1: /* F1 - set A: */
         break;
        case SDLK_F2: /* F2 - set B: */
         break;
        case SDLK_F3: /* F3 - reset */
         diag_printf("Reset pressed\n");
#ifndef _WIN32
         clock_gettime(CLOCK_REALTIME, &timespec);
         next_fire = timespec.tv_nsec + FIRE_TICK;
#endif
         reinit_cpu();
         break;
        case SDLK_F4: /* Alt-F4 - exit */
         if (SDL_GetModState() & KMOD_ALT)
           death_flag = 1;
         break;
        case SDLK_F6: /* F6 - enable keyboard joystick */
         keyjoy=!keyjoy;
         joybyte=0;
         diag_printf ("Arrows and Space are %s\n",
                      keyjoy?"JOYSTICK":"KEYBOARD");
         break;
        case SDLK_F7: /* F7 - trace (later will be enter debugger) */
         trace=!trace;
         diag_printf ("CPU Trace is now %s\n", trace?"ON":"OFF");
         break;
#ifdef DEBUG
        /*
         * F9 - creates a command line to load a file.
         * This will be folded into the debugger eventually, but it is a good
         * way to test certain things before the disk system is ready.
         */
        case SDLK_F9:
        {
         FILE *file;
         char buf1[128],buf2[128];
         uint16_t sa, a, s;
         int c;
         
         if (SDL_GetModState() & KMOD_CTRL)
         {
          file=fopen("marduk.dmp", "wb");
          if (file)
          {
           fwrite(RAM, 1, 65536, file);
           fclose(file);
           printf ("dumped RAM to marduk.dmp\n");
          }
          break;
         }
         
         s=0;
         printf ("import file>");
         fgets(buf1,127,stdin);
         buf1[strlen(buf1)-1]=0;
         if (!*buf1) break;
         
         printf ("import addr>0x");
         fgets(buf2,127,stdin);
         buf2[strlen(buf2)-1]=0;
         if (!*buf2) break;
        
         sa=a=strtol(buf2,0,16);
         file=fopen(buf1,"rb");
         if (!file)
         {
          perror(buf1);
          break;
         }
         printf ("import '%s' $%04X ", buf1, a);
         while (1)
         {
          c=fgetc(file);
          if (c<0) break;
          s++;
          mem_write(NULL, (a++)&0xFFFF, c);
         }
         fclose(file);
         printf ("L=$%04X\n", s);
         printf ("go (y/n)? ");
         fgets(buf1,127,stdin);
         if ((*buf1=='y')||(*buf1=='y'))
         {
          cpu.pc=sa;
          printf ("go to $%04X\n", sa);
         }
         break;
        }
#endif
        case SDLK_F10: /* F10 - also exit */
         death_flag = 1;
         break;
       }
      break;
     case SDL_QUIT: /* someone killed our window */
      death_flag = 1;
      break;
    }
  }
}
#endif

/*
 * Speed control. XXX: this is missing on MS-DOS.
 * 
 * On Windows, the routines we use elsewhere do exist, in libpthread, but in
 * my experience, they're flaky.  Substitute some code that is known to work.
 */
#ifdef _WIN32
/* Sloppy - from modapple */
void throttle (void)
{
  
 QueryPerformanceCounter(&currenttime);
 while (currenttime.QuadPart<wantedtime)
 {
   QueryPerformanceCounter(&currenttime);
   SwitchToThread();
 }
 wantedtime=currenttime.QuadPart+looptimedesired;
}
#else
# ifdef __MSDOS__
void throttle (void)
{
}
# else
/* POSIX version */
void throttle (void)
{
 struct timespec n;

 clock_gettime(CLOCK_REALTIME, &timespec);
 n.tv_sec = 0;
 n.tv_nsec = next_fire - timespec.tv_nsec;
 next_fire = timespec.tv_nsec + FIRE_TICK;
 if (next_fire > n.tv_nsec)
 {
  nanosleep(&n, 0);
 }
}
# endif
#endif

/*
 * Things to do once per scanline, like poll the keyboard, joystick, etc.
 * 
 * XXX: Although the entire functionality for slowing the system down is
 *      CLAIMED to be present in -lpthread on Windows, it doesn't actually
 *      seem to do anything, as the emulation still appears to run completely
 *      unthrottled.  This really needs a system-specific implementation on
 *      "#ifdef _WIN32" rather than relying on -lpthread.
 */
void every_scanline(void)
{
  keyboard_poll();
  throttle();
}

/*
 * Exactly what it says on the tin.
 * Call the TMS9918 emulator to generate the next scanline into the offscreen.
 */
#ifdef __MSDOS__ /* Simplified 320x200 raw-memory version */
void render_scanline(int line)
{
  int x;
  uint32_t r;
  uint32_t bg;
  uint8_t a_scanline[256];
  uint8_t g_scanline[320];
  if ((line<0)||(line > 199)) return;

  /*
   * To note:
   * The background color is register 7, AND 0x0F.
   */
  bg = vrEmuTms9918RegValue(vdp, 7) & 0x0F;
  memset(g_scanline, 0, 320);
  for (x = 28; x < 291; x++)
    g_scanline[x] = bg;
  if ((line >= 4) && (line < 196))
  {
    vrEmuTms9918ScanLine(vdp, line - 4, a_scanline);
    for (x=0; x<256; x++) g_scanline[32+x]=a_scanline[x];
  }

  /* Double-scan. */
  r = line * 320;
  for (x=0; x<320; x++)
   display[r+x]=g_scanline[x];

  /* Apparently some third-party software flips this bit incorrectly. */
#ifdef ALLOW_NTSC_NOISE
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

    r = line * 320;
    for (x = 0; x < 320; x++)
    {
      c = rand() & 0xFF;
      display[r + x] = c?0x1F:0x10;
    }
  }
#endif

  /*
   * Draw the LEDs.
   * XXX: Should do something fancier for the joystick
   */
  
  if (keyjoy) display[63643]=0x1F;
  
  display[63645]=(ctrlreg&0x20)?0x1E:0x10; /* Yellow LED */
  display[63647]=(ctrlreg&0x10)?0x1C:0x10; /* Red LED */
  display[63649]=(ctrlreg&0x08)?0x1A:0x10; /* Green LED */
}
#else /* The full 640x480 SDL version */
void render_scanline(int line)
{
  int x;
  int t;
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

  /* Apparently some third-party software flips this bit incorrectly. */
#ifdef ALLOW_NTSC_NOISE
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
#endif

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
    
    if (disksys_light&0x01)
    {
     for (t=8; t<16; t++)
      display[r+t]=display[r+640+t]=0xFFCC0000;
    }
    
    if (disksys_light&0x02)
    {
     for (t=24; t<32; t++)
      display[r+t]=display[r+640+t]=0xFFCC0000;
    }
    
    if (keyjoy)
    {
     uint16_t c;
     
     /* 576-583 */
     if (line==235)
     {
      for (c=576; c<584; c++)
      {
       display[r+c]=display[r+640+c]=0xFF333333;
      }
     }
     else if (line==232)
     {
      display[r+579]=display[r+580]=0xFFCC0000;
      display[r+579+640]=display[r+580+640]=0xFF333333;
     }
     else
     {
      display[r+579]=display[r+579+640]=0xFF333333;
      display[r+580]=display[r+580+640]=0xFF333333;
     }
      
     if (line==234) display[r+577]=display[r+577+640]=0xFFCC0000;
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
#endif

/*
 * End of frame.  Blit it out.
 * Also for anything that needs done every 1/60 second.
 */
#ifdef __MSDOS__
void next_frame(void)
{
  memcpy (vgamem, display, 64000);
}
#else
void next_frame(void)
{
  SDL_UpdateTexture(texture, 0, display, 640 * sizeof(uint32_t));
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, 0, 0);
  SDL_RenderPresent(renderer);
}
#endif

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

#ifndef ROM_PATHSPEC
#define	ROM_PATHSPEC	NULL
#endif

#define	ROM_PATH_ENV_VAR	"MARDUK_ROM_PATH"

static char ** get_rom_paths(void)
{
  char **ret_array;
  char *cp;
  char *pathspec = NULL;
  const char *ccp;
  const char *const_pathspec;
  size_t size = 0;
  int count, i;

  const_pathspec = getenv(ROM_PATH_ENV_VAR);
  if (!const_pathspec)
  {
    const_pathspec = ROM_PATHSPEC;
  }
  if (const_pathspec)
  {
    size = strlen(const_pathspec) + 1;
  }

  /* Count the number of paths in the pathspec. */
  for (ccp = const_pathspec, count = 0;
       ccp != NULL && *ccp != '\0'; ccp = cp)
  {
    count++;
    cp = strchr(ccp, ':');
    if (cp != NULL)
    {
      cp++;
    }
  }

  size += sizeof(char *) * (count + 1);
  ret_array = calloc(1, size);

  /* This is a safe strcpy(). */
  if (const_pathspec)
  {
    pathspec = (char *)&ret_array[count + 1];
    strcpy(pathspec, const_pathspec);
  }

  for (cp = pathspec, count = 0; cp != NULL && *cp != '\0';)
  {
    ret_array[count++] = cp;
    cp = strchr(cp, ':');
    if (cp != NULL)
    {
      *cp++ = '\0';
    }
  }
  ret_array[count] = NULL;

  return ret_array;
}

/*
 * Open ROM.
 */
static int init_rom(char *filename)
{
  int e;
  FILE *file;
  char **rom_paths = get_rom_paths();
  char **saved_rom_paths = rom_paths;
  char rom_path[PATH_MAX];

  for (;; rom_paths++)
  {
    snprintf(rom_path, sizeof(rom_path), "%s%s%s",
            *rom_paths ? *rom_paths : "",
	    *rom_paths ? "/" : "",
	    filename);
    printf("trying '%s'\n", rom_path);

    file = fopen(rom_path, "rb");
    if (file)
    {
      free(saved_rom_paths);
      break;
    }
    if (!*rom_paths)
    {
      fatal_diag(1, "FATAL: Failed to open ROM file");
      free(saved_rom_paths);
      return 1;
    }
  }

  fseek(file, 0, SEEK_END);
  romsize = ftell(file);
  if ((romsize != 4096) && (romsize != 8192))
  {
    fclose(file);
    fatal_diag(2, "FATAL: Size of ROM file is incorrect"
                  "  (expected size is 4096 or 8192 bytes)");

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

#ifndef __MSDOS__
void audio_callback(void *userdata, Uint8 *stream, int len)
{
  int i;
  int16_t sample;
  for (i = 0; i < len; i += 2) {
    sample = PSG_calc(psg);
    stream[i] = sample & 0xff;
    stream[i + 1] = sample >> 8;
  }
}
#endif

#ifdef __MSDOS__
/*
 * SDL is operated in 32 BPP mode.
 * On the contrary, however, the MS-DOS version uses an indexed mode with only
 * 256 colors (and in fact we only assign 32 of them currently).
 * 
 * Translate RGB24 to RGB18 and then output it directly to the VGA registers.
 */
void palette (uint8_t c, uint8_t r, uint8_t g, uint8_t b)
{
 r>>=2;
 g>>=2;
 b>>=2;
 
 outportb(0x03C8, c);
 outportb(0x03C9, r);
 outportb(0x03C9, g);
 outportb(0x03C9, b);
}

void initty(void)
{
 __dpmi_regs regs;
 int t;
 
 /* Unlock conventional memory, like DOS4G */
 if (!__djgpp_nearptr_enable())
 {
  fatal_diag (2, "FATAL: Could not get access to 8086 memory");
 }
 
 /* Enter MCGA graphics mode */
 regs.x.ax=0x0013;
 __dpmi_int(0x10, &regs);
 
 /* Get pointer to video memory */
 vgamem=((uint8_t *)0x000A0000)+__djgpp_conventional_base;
 
 /* Set palette entries $00-$0F to the TMS9918 colors */
 for (t=0; t<16; t++) 
  palette(t, vrEmuTms9918Palette[t]>>24, vrEmuTms9918Palette[t]>>16, 
          vrEmuTms9918Palette[t]>>8);
 
 /* Set palette entries $10-$1F to the CGA colors for UI elements */
 palette (0x10, 0x00, 0x00, 0x00);
 palette (0x11, 0x00, 0x00, 0xCC);
 palette (0x12, 0x00, 0xCC, 0x00);
 palette (0x13, 0x00, 0xCC, 0xCC);
 palette (0x14, 0xCC, 0x00, 0x00);
 palette (0x15, 0xCC, 0x00, 0xCC);
 palette (0x16, 0xCC, 0x77, 0x00);
 palette (0x17, 0xCC, 0xCC, 0xCC);
 palette (0x18, 0x80, 0x80, 0x80);
 palette (0x19, 0x00, 0x00, 0xFF);
 palette (0x1A, 0x00, 0xFF, 0x00);
 palette (0x1B, 0x00, 0xFF, 0xFF);
 palette (0x1C, 0xFF, 0x00, 0x00);
 palette (0x1D, 0xFF, 0x00, 0xFF);
 palette (0x1E, 0xFF, 0xFF, 0x00);
 palette (0x1F, 0xFF, 0xFF, 0xFF);
}

/* Restore last tty state */
void reinitty(void)
{
 initty();
 memcpy (vgamem, display, 64000);
}

void deinitty(void)
{
 __dpmi_regs regs;

 regs.x.ax=0x0003;
 __dpmi_int(0x10, &regs);
}
#endif

/*
 * On MS-DOS: 
 *   Turn off the graphics subsystem, write an error to stderr and die.
 * 
 * On Windows and *x not Apple:
 *   Display a message dialog box with the Fatal bit set and die.
 *   (The Fatal bit should show a "stop" icon of some sort.  On Windows this
 *    is a red circle with an X.  On Gtk and Qt this is a Do Not Enter sign.
 *    XXX: Our GTK error dialog doesn't display these things correctly.)
 * 
 * On Apple:
 *   XXX
 *   Currently we just write an error to stderr and die, but we should do the
 *   same thing as other unices.
 */
void fatal_diag (int code, char *message)
{
#ifdef __MSDOS__
 if (ttyup) deinitty();
#endif
#ifdef _WIN32
 /* 16 = stop / red X */
 MessageBox(0, message, "Marduk", 16);
#elif (!defined(__APPLE__))&&(!defined(__MSDOS__))
 GtkWidget *widget;
 
 widget=gtk_message_dialog_new(0, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, 
                               GTK_BUTTONS_CLOSE, "Marduk");
 gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(widget), "%s",
                                          message);
 
 gtk_dialog_run(GTK_DIALOG(widget));
 gtk_widget_destroy(widget);
#else
 fprintf(stderr, "%s\n", message);
#endif
 exit(code);
}

/*
 * This is a stub.
 * 
 * Currently, when "trace" is on, we just dump the registers once a Z80
 * operation.  This may be extended at some point in the future into a
 * framework for a proper debugger.
 * 
 * This code is called from nowhere.
 */
void debugger (void)
{
 char buf[128];
 
#ifdef __MSDOS__
 deinitty();
#endif
 
 while (1)
 {
  cpustatus(&cpu);
  putchar ('-');
top:
  fgets(buf, 127, stdin);
  if (feof(stdin))
  {
   death_flag=1;
   return;
  }
  buf[strlen(buf)-1]=0;
  if (!*buf) return;
  
  if (*buf=='q')
  {
   death_flag=1;
   return;
  }
 }
 
#ifdef __MSDOS__
 reinitty();
#endif
}

int main(int argc, char **argv)
{
  int e;

  char *bios;
  char *server, *port;
  int scanline;
  int noinitmodem;
  char *inita, *initb;
  char *cpmexec;
  
#ifdef __MSDOS__
  ttyup=0;
#else
  SDL_version sdlver;
  
  SDL_GetVersion(&sdlver);
#endif

  /* Defaults */
  dojoy=1;
  trace=0;
  noinitmodem=0;
  dog_speed=58000;
  lpt=NULL;
  inita=initb=NULL;
  cpmexec=NULL;

  /* This is still relevant for MS-DOS, thank you Watt-32 */
  server = "127.0.0.1";
  port = "5816";
  
  /*
   * Default ROM is now OpenNabu (opennabu.bin).
   * You can use actual Nabu firmware with the -4, -8 and -B switches.
   */
  bios = OPENNABU;
  while (-1 != (e = getopt(argc, argv, "48B:jJS:P:Np:a:b:x:")))
  {
   switch (e)
   {
    case '4':
      bios = ROMFILE1;
      break;
    case '8':
      bios = ROMFILE2;
      break;
    case 'j':
     dojoy=0;
     break;
    case 'J':
     dojoy=1;
     break;
    case 'B':
      bios = optarg;
      break;
    case 'N': /* Not currently documenting this */
      noinitmodem=1;
      break;
    case 'S':
      server = optarg;
      break;
    case 'P':
      port = optarg;
      break;
    case 'p':
      if (lpt) fclose(lpt); /* in case multiple times specified */
      lpt=fopen(optarg, "wb");
      break;
    case 'a':
      inita = optarg;
      break;
    case 'b':
      initb = optarg;
      break;
    case 'x':
      cpmexec = optarg;
      break;
    default:
      fprintf(stderr, 
              "usage: %s [-4 | 8 | -B filename] [-S server] [-P port]"
              " [-p file]\n",
              argv[0]);
      return 1;
   }
  }

  /* Copyrights for all components */
#ifdef __MSDOS__
  printf("Marduk version D-" VERSION " NABU Emulator\n"
#else
  printf("Marduk version " VERSION " NABU Emulator\n"
#endif
         "  Copyright 2022, 2023 S. V. Nickolas.\n"
         "  Copyright 2023 Marcin Woloszczuk.\n"
         "  Z80 emulation code copyright 2019 Nicolas Allemand.\n"
         "  Includes vrEmuTms9918 copyright 2021, 2022 Troy Schrapel.\n"
         "  Includes emu2149 copyright 2001-2022 Mitsutaka Okazaki.\n");
#ifndef __MSDOS__
  printf("  Uses SDL %u.%u.%u.  See documentation for copyright details.\n",
         sdlver.major, sdlver.minor, sdlver.patch);
#endif
  printf("  All third-party code is used under license.  "
         "See license.txt for details.\n\n");

  /* Only used for the white noise generator; a good RNG isn't necessary. */
  srand(time(0));

#ifndef __MSDOS__ /* Wait to set MS-DOS video up until later. */
  /*
   * Get SDL2 up and running.
   *
   * First, initalize the library.  Then create our window, renderer, blit
   * surface, and allocate memory for our offscreen buffer.
   *
   * If any of this fails, die screaming.
   * 
   * This will be interrupted by Gtk initialization because we might need to
   * display an error dialog.
   */
  e=SDL_Init(SDL_INIT_EVERYTHING);

  /*
   * SDL MUST be initialized before Gtk, or attempts to use Gtk with SDL will
   * segvee.  https://discourse.libsdl.org/t/gtk2-sdl2-partial-fail/19274
   * If SDL fails, we're probably safe to try initializing Gtk anyway but it
   * will most likely die its own screaming death.
   * 
   * Gtk wants to peek at our command line... I say NUTS!  Especially since
   * hacking on our command line could cause Gtk and SDL to go out of synch.
   */
#if (!defined(_WIN32))&&(!defined(__APPLE__))
  gtk_init(NULL, NULL);
#endif
  if (e)
   fatal_diag(2, "FATAL: Could not start SDL");

  /*
   * Must be done as soon as possible after setting up SDL, especially on
   * Windows.  The Gtk voodoo can come earlier; it won't run on Windows.
   */

  /* 
    If you attempt in initialize a non-existent joystick, and then an
    xinput device, it only emits some fraction of events, or none at all.
  */
  if (dojoy && SDL_NumJoysticks() > 0) { 
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
      add_gamecontroller(i);  
    }
  } else
   joystick=NULL;

  /*
   * Load the ROM, then set it visible. 
   * Originally done after we set up SDL, but that results in a half-drawn
   * window and an error diagnostic, so moved up here.
   */
  if (init_rom(bios))
    return 1;
  printf("ROM size: %u KB\n", romsize >> 10);

  /*
   * Now ready to set up our window and the necessary resources to actually do
   * stuff with it.  If at any time this process fails, die screaming.
   */
  screen = SDL_CreateWindow("Marduk", SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, 640, 480, 0);
  if (!screen)
  {
    fatal_diag(2, "FATAL: Could not create display");
    return 2;
  }
  renderer = SDL_CreateRenderer(screen, -1, 0);
  if (!renderer)
  {
    fatal_diag(2, "FATAL: Could not set up renderer");
    return 2;
  }
  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, 640, 480);
  if (!texture)
  {
    fatal_diag(2, "FATAL: Could not create canvas");
    return 2;
  }
#endif

  /*
   * Rejoin the common core.
   * Even on MS-DOS we will use an offscreen buffer; this will be beneficial
   * with the debugger when we need to flick in and out of text mode.
   * If we can't set aside enough memory for a full offscreen, die screaming.
   */
#ifdef __MSDOS__
  display = malloc(64000);
#else
  display = calloc(307200, sizeof(uint32_t));
#endif
  if (!display)
  {
    fatal_diag(2, "FATAL: Not enough memory for offscreen buffer");
    return 2;
  }

  /*
   * Set up the sound driver.
   * Currently this only works with SDL, but that's everything that isn't DOS.
   */
#ifndef __MSDOS__
  SDL_zero(audio_spec);
  audio_spec.freq = 44100;
  audio_spec.format = AUDIO_S16LSB;
  audio_spec.channels = 1;
  audio_spec.samples = 512;
  audio_spec.callback = audio_callback;

  audio_device = SDL_OpenAudioDevice(NULL, 0, &audio_spec, NULL, 0);
  SDL_PauseAudioDevice(audio_device, 0);
#endif

  /*
   * Set up the chipset.
   * Note that the PSG still has to run even if there isn't a sound driver,
   * because it takes care of other things than just the sound (nonobvious).
   */
  
  /* Set up the VDP emulation.  If it fails, die screaming. */
  vdp = vrEmuTms9918New();
  if (!vdp)
    fatal_diag(3, "FATAL: Could not set up VDP emulation");
  vrEmuTms9918Reset(vdp);

  /* Set up the PSG emulation.  If it fails, die screaming. */
  psg = PSG_new(1789772, 44100);
  if (!psg)
  {
    fatal_diag(4, "FATAL: Could not set up PSG emulation");
  }
  PSG_setVolumeMode(psg, 2);
  PSG_reset(psg);
  
  /*
   * Set up the modem.
   *
   * modem_init() returns 0=success, -1=failure, but our internal flag needs
   * 1=success, 0=failure so use ! to quickly make that change.
   * 
   * noinitmodem exists because of a BUG on MS-DOS: I don't currently know how
   * to do an initialization of Watt-32 that doesn't die screaming if it can't
   * set up the stack.  For this app, a working TCP stack isn't mandatory, so
   * Watt-32 failing to initialize should not be fatal.
   */
  if (noinitmodem)
    e = -1;
  else
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

#ifdef __MSDOS__
  initty(); /* Now we're ready to kick into MCGA mode. */
  ttyup=1;
#endif
  
  /*
   * Set up the disk system and mount the disks.
   */
  disksys_init();
  
  if (inita)
   disksys_insert(0, inita);
  if (initb)
   disksys_insert(1, inita);

  /*
   * Get ready to start the emulated Z80.
   *
   * Timings are for a 3.58 (ish) MHz CPU on an NTSC signal.  This is natural
   * because Canada uses the same video standards as the United States.
   */
  init_cpu();

  death_flag = scanline = 0;
  
  /* Reset our timer counters. */
#ifdef _WIN32
  wantedtime=0;
  QueryPerformanceFrequency(&throttlerate);
  looptimedesired=throttlerate.QuadPart/15720;
#else
# ifndef __MSDOS__
  clock_gettime(CLOCK_REALTIME, &timespec);
  next_fire = timespec.tv_nsec + FIRE_TICK;
# endif
#endif
  next_watchdog = 0;
  keyjoy = joybyte = 0;
  int i;

  int16_t sound_sample = 0;
  
  /*
   * A quick and dirty way to run certain apps from the command line.
   * No, I am NOT documenting the "-x" switch in the manual.
   */
  if (cpmexec)
  {
    size_t l;
    FILE *file;
    printf ("CP/M application: %s\n", cpmexec);
    file = fopen(cpmexec, "rt");
    if (!file)
    {
      fatal_diag(1, "FATAL: Could not read CP/M application");
    }
    fseek (file, 0, SEEK_END);
    l=ftell(file);
    fseek (file, 0, SEEK_SET);
    fread (&(RAM[0x0100]), 1, l, file);
    fclose (file);
    ctrlreg |= 0x01; /* Turn off the ROM */
    cpu.pc = 0x0100; /* Skip all initialization, enter the program */
  }

  /* Main event loop */
  while (!death_flag)
  {
    if (cpu.cyc > next)
    {
      disksys_tick();
      
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
      }

      every_scanline();

      /* ready to kick the dog? */
      if (keyboard_buffer_empty())
      {
        next_watchdog++;
        if (next_watchdog >= dog_speed)
        {
          next_watchdog = 0;
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
          }
        }
      }
      next += 228;
    }
#ifndef __MSDOS__
    if (trace) cpustatus(&cpu);
#endif
    z80_step(&cpu);
  }
  
#ifdef __MSDOS__ /* Return to text mode */
  deinitty();
#endif

  /* Clean up and exit properly. */
  printf("Shutting down emulation\n");
  if (lpt) fclose(lpt);
  if (gotmodem)
    modem_deinit();
  PSG_delete(psg);
  vrEmuTms9918Destroy(vdp);
  free(display);
  disksys_deinit();
#ifndef __MSDOS__
  if (joystick) {
   SDL_JoystickClose(joystick);
   SDL_GameControllerClose(pad);
  }
  SDL_Quit();
#endif

  return 0;
}
