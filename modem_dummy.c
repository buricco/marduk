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

#include <stdint.h>
#include <stdio.h>

/*
 * This will probably have to repeat DJ Sures' reverse-engineering of the Nabu
 * cable modem, since as of present that is only available in the form of a
 * .NET binary.
 * 
 * The architecture of this file is intended to allow for at least some degree
 * of portability, such that the specific calls to the TCP stack can be
 * abstracted, since of our target operating systems, while most use the BSD
 * socket API as-is, one uses a modified version (Winsock) and thus needs to
 * be accomodated with #ifdefs.
 * 
 * Anything in C involving the Internet is a pain in the kiester.
 */

/*
 * Dummy functions.
 * 
 * modem_read is called when the core requests an IN from port 0x80.
 * modem_write is called when the core requests an OUT to port 0x80.
 */
uint8_t modem_read (void)
{
 printf ("Read from modem port\n");
 return 0;
}

void modem_write (uint8_t data)
{
 printf ("Write 0x%02X to modem port\n", data);
 return;
}

/*
 * Set up the emulation (currently a stub that returns fail).
 * 
 * Initialize the TCP stack, if necessary, and prepare the connection to the
 * virtual head-end server.
 */
int modem_init (void)
{
 return -1;
}

/*
 * Clean up and shut down the emulation (currently a stub).
 */
void modem_deinit (void)
{
}

uint8_t modem_bytes_available (void)
{
 return 0;
}

