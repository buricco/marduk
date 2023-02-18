What is Marduk?
===============

  Marduk is an attempt to emulate the obscure Canadian NABU Personal Computer.
  The code is in a very early state and very little works yet, but it's off to
  a pretty good start.

What is NABU?
=============

  NABU was both a company, and the name of the computer they are best known
  for.

  In the early 1980s, they created a computer that would interface with the
  local cable television service to download content rather than using local
  storage - an idea that was radical for the time, and only in very recent
  years becoming common.

  The computer, and the service that it interfaced with, were available in a
  couple cities in Canada, and apparently also had a small rollout in one
  Japanese location, but was generally unsuccessful.

How did this become a thing?
============================

  In late 2022, one of these computers appeared on the YouTube channel
  "Adrian's Digital Basement", giving it a boost of notoriety that led to many
  people finding out about it for the first time, including myself.  Shortly
  thereafter, the protocols were cracked and the computer was brought to life.

  Realizing that the main hardware of the NABU was all stuff I had familiarity
  with, I decided to try writing an emulator for it.

Status
======

  The CPU, VDP and PSG are emulated via third-party code, which I have
  imported with minimal adaptation.  Also, libsdl2 is used for the front end
  I/O code.

  CPU - Tested, working.
  VDP - Tested, working.
  PSG - Tested, working.
  Console lights - Tested, working.
  Keyboard - Tested, working.
  Joystick - Implemented through keyboard; see below.
  Cable modem - Working, more or less.
  Strict speed control - Tested (mostly with another emulator), working
                         (except on MS-DOS and Windows).

Key bindings
============

  Note: The keyboard interface code does not appear to be correct.

  F3 = Reset
  F5 = Arrows and space route to the keyboard.
  F6 = Arrows and space route to the P1 joystick.
  F10 = Exit
  Ins and Del = Yes and No
  PgUp and PgDn = << and >>

  Everything else should be obvious.

ROM Files
=========

  No ROM files are included.
  
  One of the following is expected:
  
    NabuPC-U53-90020060-RevA-2732.bin (-4, default)
    NabuPC-U53-90020060-RevB-2764.bin (-8)

  If you have a different firmware you can try it with the -B switch.

Using a Virtual Adapter (Cable Modem Emulator)
==============================================

  The nabu.ca virtual adapter defaults to listening on 127.0.0.1:5816.
  
  If you need a different address, use the -S switch (e.g., -S 192.168.0.2).
  If you need a different port, use the -P switch (e.g., -P 5815).

License
=======

  Marduk is released under the terms commonly known as the "MIT" license; you
  will find them attached to every source file as well as in "license.txt".

  Basically, give credit where credit's due.  It's a little more technical
  than that though.

  (Note that SDL uses a different license, but with the same goals.)
