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

#ifndef H_PATHS
#define H_PATHS

/*
 * ROMBASE  = path to ROM files.  Default is current directory.
 *            Define this if you want them elsewhere (e.g., /etc/marduk,
 *            /usr/lib/marduk, /usr/share/marduk).
 * ROMFILE1 = ROM A (the one that comes with mjp's stash of machines), -4
 * ROMFILE2 = ROM B (8K revision with floppy disk boot support), -8
 * OPENNABU = location of OpenNabu IPL (default ROM).
 */

#ifndef ROMBASE
# define ROMBASE "."
#endif

#ifdef __MSDOS__
# ifndef ROMFILE1
#  define ROMFILE1 ROMBASE "/nabu4k.bin"
# endif /* ROMFILE1 */
# ifndef ROMFILE2
#  define ROMFILE2 ROMBASE "/nabu8k.bin"
# endif /* ROMFILE2 */
# ifndef OPENNABU
#  define OPENNABU ROMBASE "/opennabu.bin"
# endif
#else
# ifndef ROMFILE1
#  define ROMFILE1 ROMBASE "/NabuPC-U53-90020060-RevA-2732.bin"
# endif /* ROMFILE1 */
# ifndef ROMFILE2
#  define ROMFILE2 ROMBASE "/NabuPC-U53-90020060-RevB-2764.bin"
# endif /* ROMFILE2 */
# ifndef OPENNABU
#  define OPENNABU ROMBASE "/opennabu.bin"
# endif
#endif

#endif /* H_PATHS */
