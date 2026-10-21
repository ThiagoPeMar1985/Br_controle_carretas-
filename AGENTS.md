# br_controle_de_carretas (ETS2 1.61.1.9s)

Fork brasileiro de ts-extra-utilities / trailer axles control. Singleplayer.

## Build

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target br_controle_de_carretas
```

Install: copy `build/Release/br_controle_de_carretas.dll` to
`<ETS2>/bin/win_x64/plugins/` — on this machine
`D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins`.
Game log: `C:\Users\PC\Documents\Euro Truck Simulator 2\game.log.txt`.

Caution: `build/` keeps absolute paths in CMakeCache — after copying/moving the
project folder, delete `build/` and reconfigure or it compiles the old tree.
Runtime offsets live in `src/prism/offsets.hpp` (exe 1.61.1.9s — `.pdata` at
VA `0x1436e3000`; re-derive function ranges from the PE headers if it changes).

## Chase camera (camera 2) — vehicle_behind_rotation_camera

Reverse-engineered in eurotrucks2.exe 1.61.1.9s:

- Camera manager global: `exe+0x36AF830` (plain struct). The chase camera
  (vtable `exe+0x23126F0`) is found at `manager+0x38` -> slot array `[1]`.
- `camera + 0x3F0` = anchor trailer — **the CHAIN HEAD (first trailer), not the
  last**. The camera measures the combination length by walking
  `trailer_slave` links STARTING AT the anchor, so anchoring on the last
  trailer measures only that trailer and the camera parks behind the truck.
  `null` = native "solo" state → camera follows the truck.
- The native resolver writes `+0x3F0` directly (raw store), NOT through the
  vfunc-31 setter — refcounted writes/releases here underflow refcounts and
  corrupt trailer objects. Raw writes only; use the real setter (vfunc 31,
  `0x14063C2D0`) only when the field is already null.
- Camera vtable: slot 14 = recompute `0x140AC7290` (reads anchor's joint
  `+0xFE0` to pick solo vs jointed distance set, resets orbit state — call
  ONCE per transition, never per frame), slot 31 = set_target, slot 32 =
  update `0x140AC75A0`.
- Zoom fields: `+0x4D8` target distance, `+0x4DC` smoothed distance (lerp),
  `+0x480` min clamp. The native update clamps `+0x4D8` to `[+0x480, base]` —
  raising `+0x480` above base pins the distance at an arbitrary value =
  unlimited wheel zoom via field writes, no code patch. Restore `+0x480` on
  recouple.
- Wheel input: `CCore::on_mouse_input` receives `DIMOFS_Z` through the
  DirectInput `GetDeviceData` hook (vtable slot 10).
- `hk_cam_set_target` (vfunc-31 hook) swaps a dead-joint tracked trailer for
  null — keeps the camera off plugin-detached trailers.
- `hk_combo_length` (`0x140789AB0`) walks `trailer_slave` and stops at the
  first dead joint — the camera (and other consumers) then measures only the
  live combination.

## Logging conventions

Keep only event logs (couple/decouple, anchor transitions, restores). No
per-frame dumps — they were used for reverse engineering and removed.
