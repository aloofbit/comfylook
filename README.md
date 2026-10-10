# Comfy Look

> **Bugs, questions and screenshots. Ty for testing!: [join our Discord](https://discord.gg/uhefX2efB7).**
>
> [![Discord](https://img.shields.io/badge/Discord-ComfyCraft-5865F2?logo=discord&logoColor=white&style=for-the-badge)](https://discord.gg/uhefX2efB7)

Mouselook fix for the World of Warcraft 1.12 client. The camera turns by how far your hand moves, not by how fast.

## Features
- Raw mouse input: no Windows mouse acceleration on the camera
- Fixes the camera spin with Synergy and other remote mouse apps
- The cursor stays where you started the drag

## Compatibility

| | Tested with |
| --- | --- |
| Client | `WoW.exe` 1.12.1 (build 5875), OctoWoW |
| Launcher | VanillaFixes |

On another client, it does nothing and writes `unknown client` to `Logs\comfylook.log`.

## Install

Download the zip from [Releases](https://github.com/aloofbit/comfylook/releases), or build it (below).

1. Copy `comfylook.dll` to the client folder.
2. Add the line `comfylook.dll` to `dlls.txt`.
3. Start the game with `VanillaFixes.exe`.

## Turn speed

| Command | |
| --- | --- |
| `/console mouseSpeed <n>` | Turn speed, 0.1 to 2.0. Same as **Mouse Sensitivity** in Interface Options |
| `/console cameraYawMoveSpeed <n>` | Left and right only |
| `/console cameraPitchMoveSpeed <n>` | Up and down only |

A higher number turns faster. `/run print(GetCVar("mouseSpeed"))` shows the current value.

## Build

Visual Studio 2022 and CMake. **32-bit only**: the 1.12 client is x86.

```
cmake -B build -A Win32
cmake --build build --config Release
```

`comfylook.dll` is written to the project root. How it works is in [src/README.md](src/README.md).

## Licence

GPL-3.0. See [LICENSE](LICENSE).
