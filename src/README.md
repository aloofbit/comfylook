# comfylook: development notes

## Build

```
cmake -B build -A Win32
cmake --build build --config Release
```

The DLL lands in the mod root. It must be 32-bit, because the client is.

## How the client does mouselook

All addresses are for the 1.12.1 build 5875 `WoW.exe`. They were read with capstone from the octow client (md5 `d7668033e4fe2fe01057893f4a151368`) and are the same in `octow - Copy` (md5 `a1c1cd13283197c271bda619ae089ed1`).

| Address | What it is | How it was found |
| --- | --- | --- |
| `0x42CC90` | `SetMouseMode(ecx)`. 1 turns mouselook on: it stores 1 at `0x884E5C` and calls `0x42CCD0`, which moves the stored centre in from the window edge and puts the cursor there. 0 turns it off: it stores 0 and calls `0x42CD20` | The only code that writes `0x884E5C` |
| `0x884E5C` | 1 while mouselook is on | Read by the `WM_MOUSEMOVE` case of the window procedure at `0x42D31C`, and by the button cases at `0x42D54F` and `0x42DD06` |
| `0x884E44`, `0x884E48` | The cursor centre, x and y, in client coordinates | Written by `0x42CE30`, read by the `WM_MOUSEMOVE` case |
| `0x42D312` to `0x42D354` | The `WM_MOUSEMOVE` case in mouselook. `GetCursorPos`, `ScreenToClient` (in a code cave at `0x755DDC` that the exe already has), then if the cursor is not at the centre, posts event `0xC` with dx and dy and calls `0x42CCD0` to put the cursor back | Follows the `GetCursorPos` call at `0x42D316` |
| `0x42D780` | Adds an event to the input queue. `__fastcall(type, edx, a, b, c)`, `ret 0xC`. Only `a` and `b` are stored | Disassembly. The only caller with type `0xC` is `0x42D34F` |
| `0x884CC0` | The input queue: 16 events of 5 ints (type, edx, a, b, time). The client writes at `0x884E50` and reads at `0x884E34`. When it is full, a new event overwrites the oldest | `0x42D780` writes it, `0x42CC00` reads it: it copies one event and moves the read index on |
| `0x42CD20` | Puts the cursor at the stored centre: `ClientToScreen` and `SetCursorPos` | Disassembly |
| `0x884CB4` | The `mouseSpeed` CVar times 10, as a Windows pointer speed 1 to 20 | `0x42CD70` clamps the CVar to 0.1..2.0 (`0x801620`, `0x801628`), multiplies by 10.0 (`0x801624`) and stores it here |

The client also gives `0x884CB4` to `SystemParametersInfo(SPI_SETMOUSESPEED)` while its window is active (`0x42D0A7`), and puts the old speed (`0x884CB0`) back when it is not. So `mouseSpeed` changes the Windows pointer speed while the game has focus.

`ClientMatches()` checks the first bytes of `0x42CC90`, `0x42D780`, `0x42CD20`, `0x42D340` and `0x42CDA6`. These bytes contain the addresses of `0x884E5C`, `0x884E50`, `0x884E44`, `0x884E48` and `0x884CB4`, so a client with other data addresses fails the check.

## What comfylook does

- **It patches no code.** It puts its own window procedure on the `GxWindowClass*` window and calls the old one for each message it does not take.
- **Raw input is registered from the window procedure,** on its first message, so it is on the window thread. No other DLL in the octow client imports `RegisterRawInputDevices`. Registration is one per process for each usage, so a later mod that registers the mouse takes it from comfylook.
- **Each `WM_INPUT` in mouselook is one turn.** The raw counts are multiplied by `0x884CB4 / 10`, and the fraction below a pixel is kept for the next one. If the queue has an unread event and the newest one is a turn, the delta is added to it. Otherwise `0x42D780` posts a new one. The window procedure and the queue reader run on the same thread, so the reader cannot take the event while it is changed. A 1000 Hz mouse gives about 16 `WM_INPUT` for each frame at 60 fps. Without the merge, they would fill the 16 slots and overwrite other events.
- **`WM_MOUSEMOVE` in mouselook does not reach the client.** comfylook calls `0x42CD20` if the cursor is not at the centre and returns 0. The client would do the same move, and would also post the cursor distance as a second turn.
- **An absolute event turns by the distance from the previous position.** Synergy 1.14 (as a client) sends each mouse move as an absolute `SendInput`, with no device handle. The sender does not see the client put the cursor back at the centre, so without comfylook each move turns the camera by the full distance from the centre to Synergy's position, and the camera spins at any hand speed. comfylook turns by the distance between two absolute positions. After a pause of 250 ms or more, the first position gives no turn, because a touch screen starts where the finger lands. Synergy stops its own cursor at the screen edge during a drag, so one drag can turn only until that cursor reaches an edge.
- **After a drag with absolute input, a low-level mouse hook keeps the cursor where the drag started.** The client puts the cursor back at the start (`0x42CD20`), but Synergy's next move would put it at Synergy's own position. The hook maps each injected move to a cursor position and calls `SetCursorPos` in place of the move, while the game window is in front and mouselook is off. The map has two straight parts on each axis: Synergy's position at the end of the drag goes to the drag start, and each screen edge goes to the same edge. So Synergy reaches its edge, and changes screen, when the visible cursor reaches the screen edge. When Synergy reaches an edge, the map on that axis ends. The map changes the cursor speed on each side of the drag start. If a part on either axis would move the cursor more than 1.25 pixels or less than 0.8 pixel for each Synergy pixel, there is no map and the next move puts the cursor at Synergy's position. A larger change is easy to feel: a slope of 4 gave 0.39 and 2.0 times the hand speed after a long drag. So the cursor stays at the drag start after a short drag and jumps to Synergy's position after a long one. The hook starts on the first absolute raw event, on its own thread with a message loop.
- **A fixed offset in place of the map trapped the cursor.** With an offset of half the screen on each axis, Synergy's range gave a quarter of the screen. Each drag also added Synergy's travel to the offset, because the drag start already had the old offset in it.
- **The log names each device** on its first raw event, and writes one `drag` line for each mouselook drag: raw events and counts, and the cursor distance the client would have used. After a drag with absolute input, the line also gives Synergy's position, the drag start, the offset between them, and whether the cursor is `held` or `follows Synergy`.

## Not checked yet

- That a turn from comfylook feels the same as a turn from the cursor at `mouseSpeed` 1.0 with "Enhance pointer precision" off. It should, because Windows pointer speed 10 moves the cursor one pixel for each count.
- Windowed mode, and a resolution change while the game runs.
