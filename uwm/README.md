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
