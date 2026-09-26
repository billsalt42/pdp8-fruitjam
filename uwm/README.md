# UWM TSS/8.25 for the Fruit Jam

This folder builds the University of Wisconsin–Milwaukee version of TSS/8 ("UWM'S TSS-8/E.25").
Richard Bartlein extended and corrected DEC's TSS/8 monitor at UWM in 1974–76. The build is
assembled from source and produces two files for the SD card:

* `sdcard/uwm_init.bin`: the INIT loader tape (BIN format), which is loaded into field 2 and
  started at 24200
* `sdcard/uwm_rf.dsk`: the RF08 system disk. The UWM monitor is on tracks 0–4, and the file system
  follows.

Rebuild with `make uwm` from the project root. It needs only Python 3 and a C compiler; the
`palbart` cross-assembler is built from `tools/palbart`.

## Where the pieces come from

| Part | Source |
|---|---|
| `SI`, `FIP`, `INIT`, `PARA`, `PARB`, `PARC`, `PHAM`, `UWM` | bitsavers `bits/DEC/pdp8/ascii/` (UWM V25 sources, CR line endings converted to LF) → `orig/` |
| `TS8`, `TS8II` | not in the bitsavers ASCII set; taken from the UWM DECtapes as transcribed in Brad Parker's [cpus-pdp8](https://github.com/lisper/cpus-pdp8) (`tss8_uwm/orig-dectapes`) → `orig/` |
| File system and library (BASIC, FOCAL, EDIT, PIP, PALD, FORT, COPY, CAT, …) | DEC TSS/8.24 (`sdcard/tss8_rf.dsk`). The UWM sources include no library. |
| `SYSTAT`, `LOGOUT` | DEC TSS/8.24 library sources (`lib/dec/`), ported to UWM's monitor tables (`lib/`) |
| `LOGIN` | written for this build: the message of the day that UWM runs after each login |

## Changes for this build

Every change is listed in `fruitjam.patch`. In summary:

* **Configuration** (`src/FRUITJAM.PA`, based on UWM's `UWM.PA`): PDP-8/E with 32K, RF08 with one
  RS08, console plus 4 KL8E lines, high-speed reader and punch, LE8 printer, and no DECtape or
  RK05. Other settings: 20 job slots, no billing account, and production mode (`DEBUG=0`).
* **Clock:** UWM ran 20 system ticks per second from its clock. This build uses 10 per second from
  the 60 Hz DK8-E, like DEC TSS/8.24 (`PARA`, plus a 10/sec time-conversion table in `SI`).
* **DEC library compatibility:** added DEC 8.24's `WHO` IOT (6616, return account and password) to
  `FIP` and the `TS8` IOT table, so that DEC's CAT, COPY, GRIPE and LOGOUT run unchanged. Unknown
  user IOTs are ignored, as they are under DEC 8.24, instead of stopping the job.
* **No billing system:** defined `LOGACT` for `BILLNG=0` in `FIP`.
* **Library:** SYSTAT uses UWM's `JOBTBL` offset, and LOGOUT reads UWM's day clock, which is stored
  low word first at 33/34. Both, plus CAT, carry UWM's privileged extension code (34). LOGIN takes
  over the directory slot of DTTEST, a DECtape test that cannot run here.

The emulator core also needed one fix for UWM. At power-up and CAF, the RF08 now comes up with its
completion flag clear, as the hardware does. SIMH sets the flag, and UWM's start-up treats it as a
disc interrupt and halts. DEC TSS/8.24 works either way, and still matches SIMH exactly.

## Using it

At the countdown press **U**, or choose **U** in the F12 menu (or set `system=uwm` in `pdp8.cfg`).

```
LOAD, DUMP, START?? START
MONTH-DAY-YEAR: 9-25-84          (years 74-84)
HR:MIN - 09:35

TSS/8 RESTARTED
PLEASE LOGIN
.
```

UWM requires a **Ctrl-B** in front of `LOGIN`. It stops a program from faking the login prompt to
capture a password. Type Ctrl-B, then `LOGIN 2 LXHE`:

```
.^
UWM'S TSS-8/E.25   JOB 01 [0002] ON K00    09:35:04

WELCOME TO UWM TSS/8.25 ON THE ADAFRUIT FRUIT JAM
...
```

After that it works much like DEC TSS/8: `SYSTAT`, `R CAT`, `R BASIC`, `R FOCAL`, `R EDIT`, `R PALD`,
`^B S` to stop a program, and `LOGOUT`. Its command set also includes `TALK`, `BROADCAST`, `WHERE`, `SEGS`,
`VERSION`, `KJOB`, `FORCE`, `ON`/`OFF` and `DUPLEX`/`UNDUPLEX`. The console and
terminals K01–K04 work the same way as under DEC TSS/8 (Alt-F1 … Alt-F5 and the USB serial ports).

## Files

```
orig/             UWM sources as found (bitsavers + DECtape TS8/TS8II), LF line endings
src/              sources as assembled (orig + fruitjam.patch) and FRUITJAM.PA config
lib/              LOGIN.PA, and SYSTAT/LOGOUT ported from DEC (lib/dec/ = DEC originals)
fruitjam.patch    every change, as a unified diff (whitespace-insensitive)
build_uwm.py      assembles the monitor, iterating the PARB/PARC linkage until it is
                  stable, and lays the tracks onto the RF08 image
make_uwm_disk.py  build_uwm.py + installs the library programs into the file system
tssfs.py          TSS/8 RF08 file-system reader, plus in-place file replace/rename
binload.py        BIN-format tape reader
```

What are the major feature differences between the UWM version and DEC version of TSS/8 that we put together?

Ran 11 commands
The two systems are much closer than I made them sound earlier. Both run DEC's TSS/8 monitor, share the same file system and program library, and take the same logins. The UWM version is DEC's monitor as reworked by Richard Bartlein at the University of Wisconsin–Milwaukee in 1974–76. The practical differences:

Differences you'll notice

Ctrl-B before LOGIN. UWM only accepts LOGIN if you press Ctrl-B first. It's a security fix: an idle program can't fake the login prompt and capture passwords.
Message of the day. UWM runs a LOGIN program after each login, which prints a welcome message. We wrote that program for this build, since the UWM sources came without one. DEC just logs you in.
Typing a program name runs it. If UWM doesn't recognise a command, it treats it as R name, so BASIC works like R BASIC. DEC gives an error.
Extra UWM commands:
SEGS shows free disk space.
ODT loads a debugger.
XOPEN opens a file for exclusive use.
INHIBIT / REENABLE block TALK messages to your terminal.
HALF / FULL set how your terminal echoes.
Different sign-on banner. UWM shows UWM'S TSS-8/E.25 JOB 01 [0002] ON K00, and DEC shows TSS/8.24 JOB 01 [00,02] K01.
Under the hood

UWM's many bug fixes and changes to the scheduler, swapping and terminal buffering are the "extensively modified and corrected" part of its source header. You mostly won't see them, except that it should be more robust under load.
UWM added supervisor calls, such as one that reports free disk space, and it can check that a program is privileged. Some library programs have to be marked privileged to read the monitor's tables. That's why I had to adjust SYSTAT, CAT and LOGOUT for it.
UWM can use an RK05 or TD8E DECtape as the system device, and it has hooks for a billing system. This build doesn't use those options: it's set up with the same RF08 disk, console and four terminal lines as the DEC version.
What's the same in our builds

The library (BASIC, FOCAL, EDIT, FORTRAN, PAL-D, PIP, CAT, COPY) is DEC's in both, because the UWM sources didn't include one.
To make DEC's programs run unchanged under UWM, I added DEC's WHO call to UWM. I also made UWM ignore unknown calls the way DEC does, instead of stopping the program.
Both run the clock at 10 ticks per second (UWM originally used 20), use the same accounts, and accept the same 1974–84 date range.
Many commands I listed earlier as UWM additions are also in DEC 8.24: TALK, BROADCAST, WHERE, KJOB, FORCE, VERSION, ON/OFFLINE and DUPLEX. I checked the DEC source and they're there, so the UWM README overstates the difference. I can correct it if you like.
For everyday use, DEC 8.24 is the "standard" TSS/8. It's also the one checked to match SIMH exactly, since SIMH can't run UWM's monitor unmodified.
