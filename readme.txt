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
  PSG - Initial code to interface to the core.
        Not hooked up to anything yet.
  Console lights - Tested, working.
  Keyboard - Partially implemented; not working.
  Joysticks - No attempt to implement these yet.
  Cable modem - No attempt to implement this yet.
  Strict speed control - No attempt to implement this yet.

Key bindings
============

  Note: The keyboard interface code does not appear to be correct.

  F3 = Reset
  F10 = Exit
  Ins and Del = Yes and No
  Home and End = << and >>

  Everything else should be obvious.

License
=======

  Marduk is released under the terms commonly known as the "MIT" license; you
  will find them attached to every source file as well as in "license.txt".

  Basically, give credit where credit's due.  It's a little more technical
  than that though.

  (Note that SDL uses a different license, but with the same goals.)

Uh...
=====

  Yeah.

  I know my inclusion of the system firmware is technically a violation of
  copyright, but with the company long-defunct and the fact that this program
  is mainly intended as a revival of a dead platform, it is included for the
  sake of preservation.

  (For WWW-browsable source releases, this file is not included.)
