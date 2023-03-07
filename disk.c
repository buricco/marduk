/*
 * Copyright 2023 S. V. Nickolas.
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

#include <stdint.h>
#include <stdio.h>

#ifdef __MSDOS__
#define diag_printf(...)
#else
#define diag_printf printf
#endif

typedef enum
{
 DISK_NONE,
 DISK_525SS,
 DISK_525DS,
 DISK_35DS
} DISKTYPE;

static FILE *disk[2];
static DISKTYPE disktype[2];

static uint8_t trk, sec, dat, stat;

#define DSK_ENRDY 0x80  /* Drive not ready             */
#define DSK_WRPRT 0x40  /* Write protect               */
#define DSK_ETYPE 0x20  /* Data mark deleted           */
#define DSK_EWFAU 0x20  /* Write fault                 */
#define DSK_HLOAD 0x20  /* Head loaded                 */
#define DSK_ESEEK 0x10  /* Seek error/Sector not found */
#define DSK_ECRC  0x08  /* Data CRC error              */
#define DSK_ELOST 0x04  /* Data was lost               */
#define DSK_TRK0  0x04  /* Head has reached track 0    */
#define DSK_DRQ   0x02  /* Ready for data read/write   */
#define DSK_INDEX 0x02  /* Index hole detected         */
#define DSK_BUSY  0x01  /* Busy                        */

static void disksys_do (uint8_t data)
{
 diag_printf ("FDC: command $%02X, T=$%02X S=$02X D=$02X\n", data,
              trk, sec, dat);
}

uint8_t disksys_read (uint8_t port)
{
 switch (port&0x0F)
 {
  case 0x0:
   return stat;
  case 0x1:
   return trk;
  case 0x2:
   return sec;
  case 0x3:
   return dat;
  case 0xF:
   return 0x10;
 }
 diag_printf ("FDC: IN: access to unknown port $%02X\n", port);
 return 255;
}

void disksys_write (uint8_t port, uint8_t data)
{
 switch (port&0x0F)
 {
  case 0x0:
   disksys_do(data);
   break;
  case 0x1:
   trk=data;
   break;
  case 0x2:
   sec=data;
   break;
  case 0x3:
   dat=data;
   break;
  case 0xF:
   diag_printf ("FDC CARD: received message $%02X\n", data);
   break;
  default:
   diag_printf ("FDC: OUT: access to unknown port $%02X with data $%02X\n", 
                port, data);
   break;
 }
}

void disksys_eject (int drive)
{
 if ((drive!=0)&&(drive!=1)) return;
 if (disk[drive])
 {
  fclose(disk[drive]);
  disk[drive]=0;
  disktype[drive]=DISK_NONE;
  printf ("Ejected disk in drive %c:\n", drive+'A');
 }
 else
  printf ("Drive %c: is already empty.  Denied!\n", drive+'A');
}

int disksys_insert (int drive, char *filename)
{
 size_t s;
 
 if (!filename) return -1;
 if (!*filename) return -1;
 if ((drive!=0)&&(drive!=1)) return -1;
 if (disk[drive])
 {
  diag_printf ("There's already a disk in that drive.  Denied!\n");
  return -1;
 }
 
 disk[drive]=fopen(filename, "r+b");
 if (!disk[drive])
 {
  perror(filename);
  return -1;
 }
 
 fseek(disk[drive], 0, SEEK_END);
 s=ftell(disk[drive]);
 fseek(disk[drive], 0, SEEK_SET);
 switch (s)
 {
  case 204800:
   disktype[drive]=DISK_525SS;
   break;
  case 409600:
   disktype[drive]=DISK_525DS;
   break;
  case 819200:
   disktype[drive]=DISK_35DS;
   break;
  default:
   diag_printf ("That's not a disk image.  Denied!\n");
   fclose(disk[drive]);
   disk[drive]=0;
   return -1;
 }
 
 diag_printf ("Inserted '%s' in virtual drive %c:\n", filename, drive+'A');
 return 0;
}

int disksys_init (void)
{
 disk[0]=disk[1]=NULL;
 disktype[0]=disktype[1]=DISK_NONE;
}

int disksys_deinit (void)
{
 if (disk[0]) fclose(disk[0]);
 if (disk[1]) fclose(disk[1]);
 disktype[0]=disktype[1]=DISK_NONE;
}

