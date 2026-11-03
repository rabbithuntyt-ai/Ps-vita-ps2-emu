# VitaStation2 compatibility

## The top 50 PS2 games without a Vita version

The 50 best-selling PS2 games ([Wikipedia's list](https://en.wikipedia.org/wiki/List_of_best-selling_PlayStation_2_video_games))
after removing games that have a Vita version (Final Fantasy X/X-2 HD,
Metal Gear Solid 2 and 3, God of War 1 and 2, Jak and Daxter, Ratchet &
Clank 1-3) and games that need a PS2 peripheral (EyeToy: Play, Guitar Hero
II and III). Silent Hill 2, the game VitaStation2 has been developed and
measured with, is added at the end.

**Play! core** is the game's best status in the
[Play! compatibility tracker](https://github.com/jpd002/Play-Compatibility)
(the emulator VitaStation2 is built on), looked up November 2026:
*playable* (start to finish), *in game* (gets into gameplay, with known
issues), *intro* (stops early), *untested* (no report). A game can't work
better on the Vita than in the core, so this sets the ceiling; speed then
decides how playable it is.

**Measured** is filled in from `tools/vitalab_compat.sh` runs on a real
Vita (`docs/perf/compat_runs.csv`). Only Silent Hill 2 has been measured so
far.

| # | Game | Play! core | Measured on a Vita |
|---|---|---|---|
| 1 | Grand Theft Auto: San Andreas | in game | - |
| 2 | Gran Turismo 3: A-Spec | in game | - |
| 3 | Grand Theft Auto: Vice City | intro (US release in game) | - |
| 4 | Gran Turismo 4 | in game | - |
| 5 | Grand Theft Auto III | in game | - |
| 6 | Tekken 5 | playable | - |
| 7 | Final Fantasy XII | playable | - |
| 8 | Kingdom Hearts | playable | - |
| 9 | Kingdom Hearts II | playable | - |
| 10 | Dragon Quest VIII | playable | - |
| 11 | Madden NFL 2005 | untested | - |
| 12 | Tekken 4 | playable | - |
| 13 | Tekken Tag Tournament | playable | - |
| 14 | Madden NFL 2004 | untested | - |
| 15 | Madden NFL 06 | untested | - |
| 16 | Need for Speed: Underground | in game | - |
| 17 | Star Wars: Battlefront | playable | - |
| 18 | Star Wars: Battlefront II | untested | - |
| 19 | Medal of Honor: Frontline | playable | - |
| 20 | Lego Star Wars: The Video Game | in game | - |
| 21 | Pro Evolution Soccer 3 (Winning Eleven 7) | playable | - |
| 22 | Star Wars: Episode III – Revenge of the Sith | in game | - |
| 23 | Madden NFL 2003 | untested | - |
| 24 | Pro Evolution Soccer 4 (Winning Eleven 8) | in game | - |
| 25 | The Getaway | in game | - |
| 26 | Sonic Heroes | in game | - |
| 27 | WWE SmackDown! vs. Raw 2006 | intro | - |
| 28 | Pro Evolution Soccer 2008 | untested | - |
| 29 | Pro Evolution Soccer 2 (Winning Eleven 6) | untested | - |
| 30 | Spider-Man | playable | - |
| 31 | Madden NFL 07 | untested | - |
| 32 | SOCOM U.S. Navy SEALs | intro | - |
| 33 | Crash Bandicoot: The Wrath of Cortex | in game | - |
| 34 | Ace Combat 04: Shattered Skies | in game | - |
| 35 | Dragon Ball Z: Budokai | playable | - |
| 36 | The Simpsons: Hit & Run | in game | - |
| 37 | Need for Speed: Most Wanted | in game | - |
| 38 | Medal of Honor: Rising Sun | in game | - |
| 39 | The Lord of the Rings: The Two Towers | in game | - |
| 40 | Driver 3 | in game | - |
| 41 | Dynasty Warriors 4 | playable | - |
| 42 | Tony Hawk's Underground | intro | - |
| 43 | Tony Hawk's Pro Skater 3 | in game | - |
| 44 | Devil May Cry 3: Dante's Awakening | playable | - |
| 45 | Madden NFL 2002 | untested | - |
| 46 | Resident Evil 4 | playable | - |
| 47 | Dynasty Warriors 3 | playable | - |
| 48 | The Simpsons: Road Rage | playable | - |
| 49 | Ratchet: Deadlocked | intro | - |
| 50 | SOCOM II U.S. Navy SEALs | untested | - |
| - | Silent Hill 2 | playable | boots, menus at 8-32 fps, opening movie 14.8 emulated fps (Nov 2, before the movie HLE and the menu and JIT work) |

**Core status across the 50:** 17 playable, 19 in game (counting Vice
City's US release), 4 intro, 10 untested (all six Madden releases,
Battlefront II, PES 2 and 2008 and SOCOM II have no tracker reports at all).
So 36 of the 50 get into gameplay in the core today.

## Beyond the top 50

Well-known PS2 games without a Vita or PSP version, from the same tracker.
(Persona 3 and 4, Odin Sphere and Disgaea 1-2 are playable in the core too
but already have versions that run on a Vita.)

| Play! core | Games |
|---|---|
| playable | Shadow Hearts, Shadow Hearts: Covenant, Viewtiful Joe, Viewtiful Joe 2, Prince of Persia: The Sands of Time, Grandia II, Dragon Ball Z: Budokai Tenkaichi 3, Shin Megami Tensei: Digital Devil Saga 1 and 2 |
| in game | Katamari Damacy, Okami, Ar tonelico I and II, Atelier Iris 1-3, Suikoden V, Rogue Galaxy, Dark Cloud, Dark Cloud 2, Klonoa 2, Ico, Bully, Xenosaga I-III, Grandia III, Grandia Xtreme, Burnout 3, Shin Megami Tensei: Nocturne, Onimusha 2, Psychonauts |
| intro | Prince of Persia: Warrior Within and The Two Thrones, Final Fantasy XI |

**Best bets on the Vita:** turn-based RPGs that are playable in the core:
they stay enjoyable below full speed, where action games don't. From both
lists: Dragon Quest VIII, Final Fantasy XII, Kingdom Hearts I and II,
Shadow Hearts and Covenant, Grandia II, Digital Devil Saga 1 and 2.

## What to expect on the Vita

Speed is the limit, not the core: the Vita's 444 MHz Cortex-A9 executes
5-8 ARM instructions per PS2 instruction today (README, performance gap),
and Silent Hill 2 runs at roughly a third of full speed. Commercial 3D
games should be expected to boot and run, but slowly, until the work in
docs/ROADMAP.md (Library-wide plan) lands:

| Bottleneck | Typical games | What closes the gap |
|---|---|---|
| EE (game logic, physics) | GTA III / Vice City / San Andreas, Driver 3, The Getaway, Battlefront | On-device tier-2 recompiler and the persistent JIT cache (docs/AOT_TRANSLATION.md) |
| VU1 (3D geometry) | Gran Turismo 3/4, Tekken, Dynasty Warriors, Ace Combat | VU1 microprograms on the Vita's GPU |
| GS (fill, effects) | Final Fantasy XII, Kingdom Hearts, Silent Hill 2's fog | GPU renderer work: menus already 2-3x cheaper; effect detection |
| Movies | Every game with FMVs (libmpeg) | Native movie decoder (`mpeg_hle=1`, being finished) |
| Scene loads | All | Persistent JIT cache (stalls once per game instead of every load) |

## Running a compatibility test

With Vita Lab connected and games in `ux0:data/VitaPS2/games/`:

```bash
bash tools/vitalab_compat.sh runs/compat 300
```

Each game is booted, played for 300 seconds with START and CROSS pressed
now and then, and recorded (screenshots, perf.csv, crash dumps). The result
goes into `docs/perf/compat_runs.csv`: crash / no-boot / boots, seconds to
the first frame, median emulated and shown fps, EE / VU1 / GS-wait shares
and JIT cost per block. First-boot tuning runs during these sessions, so
the numbers are each game's tuned speed.

## Measured runs

Latest run of each game (tools/vitalab_compat.sh).

<!-- compat-runs-start -->

No runs yet: see "Running a compatibility test".

<!-- compat-runs-end -->
