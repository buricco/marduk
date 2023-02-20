# Copyright 2022, 2023 S. V. Nickolas.
# Copyright 2023 Marcin Wołoszczuk.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to
# deal in the Software without restriction, including without limitation the
# rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
# sell copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following condition:  The above copyright
# notice and this permission notice shall be included in all copies or
# substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

# This should also work with Windows, using MinGW, if you do LIBS="-lws2_32"
# Build with CFLAGS=-DDEBUG for CPU trace (will be better integrated later)

CFLAGS := $(CFLAGS) `sdl2-config --cflags`
LIBS   := $(LIBS) `sdl2-config --libs`

all:	marduk

marduk:	dasm80.o emu2149.o main.o modem.o tms9918.o tms_util.o z80.o
	$(CC) $(CFLAGS) -o marduk dasm80.o emu2149.o main.o modem.o tms9918.o tms_util.o z80.o $(LIBS)

dasm80.o:	dasm80.c z80.h
	$(CC) $(CFLAGS) -c -o dasm80.o dasm80.c

emu2149.o:	emu2149.c emu2149.h
	$(CC) $(CFLAGS) -c -o emu2149.o emu2149.c

main.o:	main.c emu2149.h modem.h tms9918.h tms_util.h z80.h
	$(CC) $(CFLAGS) -c -o main.o main.c

modem.o:	modem.c modem.h
	$(CC) $(CFLAGS) -c -o modem.o modem.c

tms9918.o:	tms9918.c tms9918.h
	$(CC) $(CFLAGS) -c -o tms9918.o tms9918.c

tms_util.o:	tms_util.c tms9918.h tms_util.h
	$(CC) $(CFLAGS) -c -o tms_util.o tms_util.c

z80.o:	z80.c z80.h
	$(CC) $(CFLAGS) -c -o z80.o z80.c

clean:
	rm -f marduk dasm80.o emu2149.o main.o modem.o tms9918.o tms_util.o z80.o
