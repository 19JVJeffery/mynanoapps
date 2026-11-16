# MyNanoApps — 22 Custom Apps for iPod nano 7th Gen

A collection of **22 standalone apps** built for the **[NanoApps](https://github.com/nfzerox/NanoApps)** homebrew runtime on the **iPod nano 7th Generation** (`240x432` touch display, accelerometer, audio stack, and OpenGL ES 1.1 GPU).

> **Repository Scope Notice**  
> **Only the apps themselves live in this repository** (`Apps/`, `packages/`, and standalone `src/<app>/` app sources).  
> The NanoApps firmware payload, home launcher, SDK, LVGL integration, build system, toolchain scripts, and original built-in apps are **not** included here — they live in the original upstream repository:  
> **https://github.com/nfzerox/NanoApps**

---

## Repository Contents

| Folder | Contents | Purpose |
| :--- | :--- | :--- |
| `Apps/*.nanoapp/` | 22 ready-to-drop app bundles | Drag & drop directly into `/Apps` on your iPod |
| `packages/*.zip` | 22 standalone ZIP archives | Download & unzip individual apps |
| `src/<app>/` | `Makefile`, `Info.plist`, `<app>.c` | Self-contained source for each of the 22 apps |

Every app compiles cleanly against stock upstream `nfzerox/NanoApps` with zero modifications to `sdk/`.

---

## How to Install on iPod nano (7th Gen)

### 1. Prerequisite: Install NanoApps Upstream Base
Before installing any of these apps, your iPod nano 7th Gen must already be running **NanoApps**. Follow the official firmware, DFU/bootstrap, and disk setup instructions in the original repository:  
https://github.com/nfzerox/NanoApps

### 2. Copy Apps to the iPod
Once NanoApps is installed and working on your device:

1. Connect your iPod nano to your computer in **Disk Mode** (USB storage mounted).
2. Open the **`Apps`** folder at the root of the iPod drive.
3. Choose either method:
   - **From `Apps/` (fastest):** Copy any `<Name>.nanoapp` folder(s) from [`Apps/`](Apps) directly into `/Apps` on the iPod.
   - **From `packages/`:** Download any `.zip` from [`packages/`](packages), extract it, and copy the resulting `.nanoapp` folder into `/Apps`.
4. **Safely eject** your iPod. The NanoApps home screen detects newly added `.nanoapp` bundles automatically.

> **Upgrading vs. Stock Apps:** `Wallet.nanoapp` and `Calculator.nanoapp` share the bundle folder names of the original stock Wallet and Calculator apps so they act as direct drop-in replacements (back up your originals from the iPod's `/Apps` folder first if you wish to keep them).

---

## Included Apps (22 Total)

### Major Overhauls
| App | Folder | Package | Highlights |
| :--- | :--- | :--- | :--- |
| **Wallet** | `Apps/Wallet.nanoapp` | `packages/wallet.zip` | 60fps direct-framebuffer stacked card fan animation & 3D card flip (zero LVGL overdraw lag), touch-drag card scrubbing, pass details, and QR/barcode rendering |
| **Calculator** | `Apps/Calculator.nanoapp` | `packages/calculator.zip` | Auto-rotating accelerometer landscape scientific mode (`sin`, `cos`, `tan`, `log`, `ln`, powers, roots), crisp 7-segment display with thousands commas, EPX-smoothed key labels |

### Productivity & Utilities
| App | Folder | Package | Highlights |
| :--- | :--- | :--- | :--- |
| **Pomodoro** | `Apps/Pomodoro.nanoapp` | `packages/pomodoro.zip` | Focus / Short Break / Long Break countdown ring with 4 session cycle dots and throttled arc redraws |
| **Converter** | `Apps/Converter.nanoapp` | `packages/converter.zip` | Multi-category unit converter (Length, Weight, Temp, Speed, Data, Volume) with full unit names and live `1 unit = X unit` conversion reference |
| **Habits** | `Apps/Habits.nanoapp` | `packages/habits.zip` | Daily habit checklist (with T9 add + streak tracking) and 28-day bitmap heatmap displaying habit names, streaks (`Nd`), rounded day tiles, and highlighted today marker |
| **Flashcards** | `Apps/Flashcards.nanoapp` | `packages/flashcards.zip` | Built-in & custom (`/Data/Flashcards/custom.txt`) Leitner spaced-repetition decks with lag-free card flip animation and drop-shadow styling |
| **Expenses** | `Apps/Expenses.nanoapp` | `packages/expenses.zip` | Monthly budget tracker with `$ left / over budget` readout, multi-color segmented category spending-share bar, breakdown bars, transaction history, and quick cents numpad |
| **Metronome** | `Apps/Metronome.nanoapp` | `packages/metronome.zip` | Microsecond-accurate swinging pendulum (30–240 BPM), tempo markings (`LARGO`–`PRESTO`), beat accent LEDs, time signatures, tap tempo, and shaded 3D pendulum bob |
| **Pedometer** | `Apps/Pedometer.nanoapp` | `packages/pedometer.zip` | 60Hz 3D accelerometer step counter, live cadence (`spm`) bar, goal progress ring, distance/calories/timer, and 7-day history chart |
| **Flashlight** | `Apps/Flashlight.nanoapp` | `packages/flashlight.zip` | Full-brightness LCD torch (`WHITE`, `WARM`, `RED`, `SOS`, `STROBE`), live brightness slider with split-contrast `%` label, tap-to-hide dock |
| **Morse Code** | `Apps/Morse Code.nanoapp` | `packages/morse.zip` | Telegraph keyer pad with live dot/dash hold-duration meter, visual lamp transmitter (`SOS` + T9 text), and A–Z / 0–9 reference chart |

### Puzzle & Strategy Games
| App | Folder | Package | Highlights |
| :--- | :--- | :--- | :--- |
| **Picross** | `Apps/Picross.nanoapp` | `packages/picross.zip` | 24 built-in 10x10 nonogram puzzles, finger-drag painting, 5x5 quadrant shading, live `MM:SS` timer, and `[HINT]` solver assist |
| **Blackjack** | `Apps/Blackjack.nanoapp` | `packages/blackjack.zip` | Casino 21 with drop-shadow playing cards, dual corner ranks, suit glyphs, stacked colored chips on felt table, Hit/Stand/Double/Split |
| **Reversi** | `Apps/Reversi.nanoapp` | `packages/reversi.zip` | 8x8 Othello with Easy/Hard alpha-beta AI & 2-player mode, legal-move gold dots, undo stack, and 3D-shaded glossy discs |
| **Mastermind** | `Apps/Mastermind.nanoapp` | `packages/mastermind.zip` | Secret 4-peg code breaker with 6 colors, recessed peg sockets, 3D specular pegs, duplicates toggle, and win/streak persistence |
| **Checkers** | `Apps/Checkers.nanoapp` | `packages/checkers.zip` | 8x8 Draughts with forced captures, multi-jump chaining, alpha-beta AI & 2-player mode, 3D-shaded pieces, and pixel King crowns |

### Creative, Showcase & 3D GPU Apps
| App | Folder | Package | Highlights |
| :--- | :--- | :--- | :--- |
| **Falling Sand** | `Apps/Falling Sand.nanoapp` | `packages/sand.zip` | 60fps cellular automata (Sand, Water, Fire, Wood, Plant, Oil, Acid, Lava, Gunpowder), accelerometer tilt gravity, Game of Life mode, and `[SPT]` top emitter spouts |
| **Asteroids** | `Apps/Asteroids.nanoapp` | `packages/asteroids.zip` | Glowing 2px vector wireframe shooter with splitting rocks, spark particles, optional tilt steering, warp, and periodic +250 pt flying UFO saucer |
| **Starfield 3D** | `Apps/Starfield 3D.nanoapp` | `packages/starfield.zip` | Hardware OpenGL ES 1.1 neon tunnel & gate runner with scrolling 3D perspective floor/ceiling grid, flickering starfighter thruster, tilt/drag controls, and pixel-text HUD |
| **Cover Flow** | `Apps/Cover Flow.nanoapp` | `packages/coverflow.zip` | 3D perspective cover carousel with glossy frames, floor reflections, peeking vinyl record disc, 4 audio visualizers (`BARS`, `WAVE`, `RING`, `VU`), and OS media controls |
| **Pocket Synth** | `Apps/Pocket Synth.nanoapp` | `packages/synth.zip` | 16x8 polyphonic step sequencer synthesizing 22.05 kHz WAV loops to OS audio, live waveform oscilloscope preview, and rounded LED pads |
| **Globe 3D** | `Apps/Globe 3D.nanoapp` | `packages/globe.zip` | Hardware OpenGL ES 1.1 interactive Earth with day/night shading, dual cyan atmospheric halo rings, starfield, touch inertia, world city selector, local time & lat/lon HUD |

---

## Building from Source

Because this repository intentionally excludes the upstream SDK, toolchain, LVGL, and firmware code, build these apps inside a clone of the upstream **[nfzerox/NanoApps](https://github.com/nfzerox/NanoApps)** repository:

```bash
# 1. Clone and set up the upstream NanoApps SDK & toolchain
git clone https://github.com/nfzerox/NanoApps.git
cd NanoApps
# Follow nfzerox/NanoApps README to install arm-none-eabi-gcc, Python deps, LVGL, and FontAwesome

# 2. Copy all custom app folders from this repo's src/ into NanoApps/apps/
cp -R /path/to/mynanoapps/src/* ./apps/

# 3. Build & package any app
./start build wallet calculator starfield
./start package wallet
```

Packaged `.zip` archives are output to `NanoApps/build/packages/<app>.zip`.

---

## Credits & Attribution

- **Original NanoApps Runtime, SDK, Firmware Payload, Tools & Original Apps:** [nfzerox/NanoApps](https://github.com/nfzerox/NanoApps)
- **Custom & Overhauled App Pack:** [19JVJeffery/mynanoapps](https://github.com/19JVJeffery/mynanoapps)
