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

## Trailer teardown / liveness

- **`trailer_is_live` walks UP, never DOWN.** `+0x1080` (`trailer_parent`)
  and `+0xFE0` (joint) are only written on attach/detach, while our
  maintenance rewrites `trailer_slave` every frame — a down-walk sees a
  half-written chain and misclassifies a jointed trailer as parked for one
  frame (the lamp pin then darkens it = the flicker).
- **`vfunc_disconnect` impl = `exe+0x789C20`** (vfunc 99, shared by every
  trailer-class vtable): releases `+0xFE0`, feeds the brake channel,
  writes `+0x2A0=0` while walking `trailer_slave`, then clears `+0x1080`.
  Every caller lands there — the T dispatcher (`call [rax+0x318]` at
  `exe+0x5DDF03`), the queued sim-thread section teardown (via
  `exe+0x4CF9E0`), and our own vcall. `hk_trailer_disconnect` severs the
  chain at the first member that is jointless (our parked visibility
  claims) or not a trailer-class object before calling the original —
  both killed the native walker mid-teardown (fault at `0x140789CB7`,
  `mov [rcx+0x2A0], edi` on a KERNELBASE pointer).
- **`vfunc_connect` impls = `exe+0x639090` / `exe+0x7890A0`** (vfunc 98 —
  different trailer classes use different impls, so BOTH are hooked). The
  join walks `trailer_slave` through SELF-RECURSIVE helpers
  (`exe+0x6390C0`/`exe+0x639220` call themselves on slave links) — a
  parked claim to a limbo trailer aborts it mid-join with NO dump, and a
  cyclic chain overflows the stack. `sever_parked_claims()` hides every
  jointless slave edge for the duration of the call; maintenance rebuilds
  the claims next frame.
- **`is_trailer_obj(p)` = the real type check.** `vtbl[99] ==
  exe+0x789C20` — every trailer class shares that disconnect impl.
  `readable_ptr`/`plausible_object` are NOT enough: a freed trailer slot
  recycled by a foreign allocation (observed: a pointer into KERNELBASE's
  own pages) stays readable and can even hold exe-range data. The survey
  `walk`, `canon_dead`, the claim severs and the lamp propagator all use
  it — a dead canon entry used to get written into `trailer_slave` links
  by maintenance and then killed the native walkers mid-teardown (the
  race showed up as `X.slave` changing between the teardown loop's two
  reads of `[r8]`).

## Logging conventions

Keep only event logs (couple/decouple, anchor transitions, restores). No
per-frame dumps — they were used for reverse engineering and removed.

## Steerable trailer axles (Oversize panel)

- `trailer + 0x6B8` = current steering angle float -1..1,
  `trailer + 0x6C0` = `vehicle_wheel_steering_data_t*` (null = not steerable).
- Native `set_individual_steering(data*, angle)` runs every steering advance;
  pattern `48 89 5c 24 08 48 89 74 24 10 57 48 83 ec ? 8b 41 ? 48 8b d9 0f 29 74`.
- Hook swaps the incoming angle for the atomic user angle when `data` matches
  a locked trailer, then ALWAYS calls the original — never skip it.
- `locked_steering[i]` = atomic {data ptr, angle}: the hook runs on the game
  thread, UI on another — all exchange is via atomics, pointers are republished
  every frame so trailer swaps/reloads never leave stale pointers.
- Steering a trailer auto-locks it; center eases to 0 at `steering_speed`.
- The source project clamped the right arrow at -1.f (bug) — fixed here to +1.f.
- Settings: `steering_speed`, `oversize_collapsed`, `keys_width` in
  settings.ini; UI = collapsible Oversize pane beside the (resizable) keys pane.

## Trailer brake bias ("Carreta freia mais")

- The truck broadcasts THREE channels per frame (`0x14064B7A9` block):
  vfunc `0x178` → trailer `+0x730` = SERVICE demand (the pedal signal),
  vfunc `0x180` → `+0x760` = the factor the wheel solver consumes,
  vfunc `0x188` → `+0x764` byte = forced flag.
- **+0x730 is a dead demand mirror** — verified empirically: holding it at
  10.0 produced zero physical effect. Do NOT hook/bias it. The only field
  that becomes real torque is `+0x760`: the per-wheel solver
  (`0x14090C570`) applies `max(+0x760, per-wheel floor) × grip/load` with
  NO upper clamp, so >1 produces real extra torque — wheels lock under
  load transfer exactly like a trailer without EBS. Same field the spring
  brake uses on detached trailers.
- Implementation: `update()` writes
  `+0x760 = max(native, +0x730 × (1 + trailer_boost/100))` every frame for
  jointed trailers only (`[brake] trailer_boost`, 0-200). Raising-only
  preserves native spring-brake writes; the multiplier caps the max.
  `biased_brake()` amplifies legitimate broadcast writes the same way.
  The `+0x760`/`+0x764` setter hooks also carry the foreign-write guard
  (stale parked-set spring pushes).
- **Braked-wheel mask**: `trailer+0x150` is the wheel-set object —
  `+0x178` wheel count, `+0x2BC` bitmask of wheels that take the factor.
  The solver only applies `+0x760` to masked wheels, so stock trailers
  may brake just one axle (e.g. vanderleia mask `0xC` = middle axle only).
  `update_wheel_brake_mask()` rewrites it per frame when boost > 0: all
  axles down → every wheel gets a bit; any axle lifted (lift controller
  progress > 0.5) → falls back to `native_mask & grounded` (the native
  mask is cached per wheelset on first sight in `native_mask_ws/`_val`).
  Wheels map to axles evenly (`wheel i → axle i*axles/n`).
- **Lifted-wheel dead-stop fix**: the wheel solver `0x14090C510`
  (pattern `48 89 5c 24 18 48 89 74 24 20 57 48 83 ec 50 48 8b 81 78 01`)
  runs on the sim thread — rcx = wheelset, `+0x10` vehicle, `+0x178`
  wheel count, `+0x260/+0x268` per-wheel output array. Wheels outside the
  mask still receive their `+0x738` floor × grip factor, so an airborne
  wheel dead-stops on any pedal input. `hk_wheel_brake` calls the
  original then zeroes `out[w]` for wheels of raised axles — only when
  `index_of(veh)` matches a tracked trailer (the same solver serves the
  truck). Doing this in `update()` races: the solver rewrites the array
  every physics tick.
- The truck's own channel is skipped via the game_actor+0x18 pointer check
  which MUST run before `joint()`: +0xFE0 is a trailer field and holds
  unrelated live data on the truck object.
- Dead ends (don't retry): `+0x149C`/`+0x14D8`/`+0x1160`/`+0x1168` are
  truck-only fields (garbage on trailers); chassis sub-objects
  `+0x1F8`/`+0x200` hold no brake state — the trailer has no pneumatic
  brake model, factor→torque is direct.

## Air compressor boost ("Compressor" slider)

`truck.brake.air.pressure` IS a single field — reverse-engineered from the
channel registration at `0x1405A49E3` (the `lea` right before it loads the
`truck.brake.air.pressure` string @ `0x14223CEC0`): the getter callback
`0x14064B850` computes

    psi = [[truck+0x200] + 0x458] * [truck+0x1148] * 14.5

- `game_actor + 0x18` = truck physics object; `+0x200` = air/brake
  sub-object; `+0x458` = tank MAX pressure (~10.0 bar constant — do NOT
  write, earlier builds corrupted it to 21 and got auto-healed)
- `truck + 0x1148` = live fill fraction (0..~0.93 cutout) — THE writable
  production target; `14.5` = the getter's bar→psi constant
- `update_air()` resolves `game_actor → +0x18 → +0x200` directly (no
  hooks), verifies `max × frac × 14.5 ≈ psi` once, then runs a closed
  loop: pushes the deficit between the achieved telemetry rate and the
  target `native_rate × (boost+1)` into `+0x1148` (units = max×14.5 psi
  per 1.0 fraction).
  `native_rate` is an EMA of the rising telemetry slope (minus our own
  contribution), falling back to `0.0003 × rpm`. Sanity check refuses to
  write if the layout moved. Settings `[air] boost` 0-5, 0 = native
  (N = N+1× total production).

Dead ends kept for reference — do NOT reuse:
- The `+0xA8/+0xB0` and `+0xD0/+0xD8` float arrays (with cached means at
  `+0x168/+0x16C`, getters `0x140667880`/`0x140667A40`) belong to a
  different air subsystem — they read all-zero on the live objects while
  psi is full.
- `[[[exe+0x3046780]+0x18]+0xA0]+0x10` (dispatcher chain at `0x1405ABF55`)
  and `truck+0x1F8` (chassis model) both resolve template-looking
  instances whose air fields stay zero — the player's pressure does not
  live there.
- The earlier `0x1405AB850` "getter" is actually a record builder, not a
  value reader.
