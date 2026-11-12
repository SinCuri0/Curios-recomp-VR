# VR port notes (Ocarina of Time 1.1.1)

This file lists every game fact the VR mode uses: addresses, offsets, patch words, register meanings and units. It is for
moving the VR mode to the [Fritchii](https://github.com/Frizti/Fritchii/tree/oot-gather-fix) emulator. The code that uses
these facts is `runtime/vr_camera.c`, `runtime/vr_xr.c`, `runtime/vr_stereo.c` and `recomp/patches-oot111.txt`.

Game build: OoT 1.1.1 `main`, build id `e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348`.

Each fact has a status:

- **verified**: seen in a test, with the result written here.
- **read**: read from the game code (Ghidra or the generated C), not tested by itself.
- **not tested**: written, but never run.

## Addresses and units

- All addresses are offsets in the `main` module: `main+0x33a410`. This recomp loads `main` at `0x7000000000`, so
  `0x700033a410` in a log is `main+0x33a410`. Fritchii's patch offsets (`patch_manager.cpp`) use the same module offsets.
- Vtable addresses are also `main` offsets: `0x7004b25e18` in a log is `main+0x4b25e18`.
- Game units are **metres** (verified: the camera controller's eye and look-at point sit 2.5 m apart in normal play, and
  the listener view sits 1.36 m above Link's feet).
- World axes: `+y` is up. A view matrix is 3 rows of 4 floats, world to camera, and the camera looks along `-z`
  (verified: the set-view hook turns the picture as expected with this layout).

## How a hook works here, and how to port it

This recomp lifts game code to C, so one site can become several instructions. Each hook keeps the original instruction
and adds two after it:

```
<original>          ; for example mov x19, x0
mov x16, x30        ; aa1e03f0
bl  page+<slot>     ; the host function
```

`page` is `main+0x4ea0000` (the patch page right after `main`). The `bl` word is
`0x94000000 | (((0x4ea0000 + slot - site) >> 2) & 0x3ffffff)`, with `site` the site address (checked against all five
words below). The host function first restores the return address: `x30 = x16`.

**Port:** an emulator cannot add instructions in place. In Fritchii, a patched `svc #imm` can call emulator C++ (the
same way `svc_nx2_oot_poll.cpp` uses `svc 0xF3`). Replace the original instruction with one `svc`, and do the original
instruction's work in the handler. Every original instruction here is simple: `ldr w10, [x0, #0x10]` or a `mov` from
`x0`. The `mov x16, x30` step is not needed with `svc`. Not tested in Fritchii.

## The hooks

| Site | Original word | Instruction | Host slot | Function | What the host function gets |
|---|---|---|---|---|---|
| `main+0x33a428` | `b940100a` | `ldr w10, [x0, #0x10]` | `0xfb0` | set-view `main+0x33a410` | `w1` view index, `x2` and `x4` the 3x4 view, `x3` the projection object, `x16` the caller's return address |
| `main+0x4c48f4` | `aa0003f3` | `mov x19, x0` | `0xfa0` | camera controller update `main+0x4c48c0` | `x0` the camera controller |
| `main+0x32a4de4` | `aa0003f5` | `mov x21, x0` | `0xf90` | camera target update `main+0x32a4c40` | `x0` the actor the camera follows (Link) |
| `main+0x14e0220` | `aa0003f3` | `mov x19, x0` | `0xf80` | skinning matrices `main+0x14e0200` | `x0` a skeleton |
| `main+0xe7a248` | `aa0003f3` | `mov x19, x0` | `0xf70` | cutscene camera track `main+0xe7a220` | not used for detection (see below) |

Patch lines (`recomp/patches-oot111.txt`, set `vrcam`): the words after the original are the replacement.

```
site vrcam main 33a428 b940100a b940100a aa1e03f0 952d9ae2
site vrcam main 4c48f4 aa0003f3 aa0003f3 aa1e03f0 952771ab
site vrcam main 32a4de4 aa0003f5 aa0003f5 aa1e03f0 946ff06b
site vrcam main 14e0220 aa0003f3 aa0003f3 aa1e03f0 94e70358
site vrcam main e7a248 aa0003f3 aa0003f3 aa1e03f0 95009b4a
```

## The renderer's set-view: `main+0x33a410` (verified)

This is the one hook that turns the picture. Earlier tries did not work:

- `GameLookAtCamera_doUpdateMatrix` `main+0x7139c0`: no effect on the picture.
- Writing the controller's eye `+0xd8` and look-at `+0x100` at `main+0x4c4af0`: no effect.
- The render-view setup `main+0x33f5c0`: the picture turned, but shadows and effects stayed with the old view.

The writer was found with a hardware write breakpoint (DR0). A guard-page watch missed it.

Arguments on entry (read and verified):

- `x0`: the renderer. `*(x0 + 0x18)` is its view array: `0x480` bytes per view index. The u32 at `x0 + 0x10` is the
  number of views: the function skips the copy when `w1` is not lower (read).
- `w1`: the view index. View 0 is the main camera.
- `x2`: the 3x4 view matrix (12 floats, rows of 4). It is copied to `slot + 0x00 .. 0x2f`.
- `x4`: the same view again (often the same pointer as `x2`). The inverse and the culling planes come from it, so the
  hook must change `x2` **and** `x4`.
- `x3`: the projection object. For the main projection its vtable (`*(x3)`) is `main+0x4bf8c70`. Its 4x4 matrix is at
  `x3 + 0xc` and again at `x3 + 0x4c`, rows of 4 floats: `P00` at `+0xc`, `P11` at `+0xc + 20`.

What the hook does: it copies the view into a buffer of its own, turns it by the head pose, and points `x2` and `x4`
at the copy. For view 0 it also points `x3` at a copy of the projection object with a wider `P00` / `P11` for the
headset's field of view.

Who calls set-view for view 0 (the return address in `x16`, verified):

| Caller | What it is |
|---|---|
| `main+0x33a2d8` | The displayed camera in normal play. It is in `main+0x33a240`, which reads the camera at `*(x0 + 0x5f0)` and its view at `*(camera + 0x78) + 8`. `main+0x33a240` is called from `main+0x79be8`. It also sets views 1 (`main+0x33a320`) and 2 (`main+0x33a368`). |
| `main+0x33dd88` | A "listener" view. In normal play it sits 1.36 m above Link's feet. In a talk or cutscene it moves to the shot. This is the cutscene signal. It runs on every screen, also during videos. |
| `main+0x3320b4` | A fixed probe view at `(5, 130, -5)`. |

## Head pose to view (math, verified in the window test)

From OpenXR (`runtime/vr_xr.c`, `vr_head_pose`):

- The head's forward `fwd` is built from yaw and pitch only (roll is dropped):
  `fwd = (-sin(yaw) cos(pitch), sin(pitch), -cos(yaw) cos(pitch))`.
- The head position `p` is in metres from the first pose seen ("head zero").

Applied to a view (`runtime/vr_camera.c`, `vr_camera_view`):

1. Take the game view's rows `R` (right), `U` (up), `B` (back) and its eye position `pos = -(R t.x + U t.y + B t.z)`.
2. With `level` on (default), drop the game camera's pitch: `B` becomes horizontal, `U = (0,1,0)`, `R = (B.z, 0, -B.x)`.
3. Move the eye by the head position in the camera's frame: with `n = p / |p|`,
   `pos += (n.x R + n.y U + n.z B) * min(|p|, 0.5 m) * move`. `move` is `vr_move`, default 1.0 (1 game metre per real
   metre).
4. Turn: the head rows `hr = (-fwd.z, 0, fwd.x)`, `hu = hr x fwd`, `hb = -fwd` are written in the camera frame
   (`row = h.x R + h.y U + h.z B`) and become the new view rows. The translation column is `-(row . pos)`.

## First person (verified in the window test)

### Link's actor

From the camera-target hook (`x0` at `main+0x32a4de4`). Seen in tests (read, the code does not check them): vtable
`*(actor)` is `main+0x4b25e18`, and the u32 at `actor + 0x2c` is 4.

| Offset | Type | Meaning |
|---|---|---|
| `+0x31c` | float3 | Link's feet in the world (metres) |
| `+0x328` | float 3x3 | Link's rotation |
| `+0x8` | pointer | the "Player" object: vtable `main+0x4bf78e0`, name `"Player"` at `+0x20` |

### Link's model and skeleton

- Parts array: `*(*(actor + 0x8) + 0x70)`.
- Body model: `*(parts + 0xb0)`. Its name is `Player_Child` (child Link). Adult Link is **not tested**.
- Skeleton object: `*(model + 0x398)`.
  - `+0x00`: pointer to the skeleton resource.
    - `+0x38` (u16): bone count.
    - `+0x10` (pointer): bones, `0x58` bytes each. The parent index is the u16 at `bone + 0x22`.
  - `+0x20`: pointer to the world matrices: `0x40` bytes per bone, 4 columns of 4 floats, from a local origin (not the
    world origin). The translation is floats 12, 13, 14.
- Head bone: **103**. Its parent is 102 (`Neck`). 122 bones are below the head: face, hair, and the `Ph_` physics bones of
  the cap and hair. The code checks that bone 103's parent is 102 before it uses this.

### Head position and eye

`head = feet + (world[103].translation - world[0].translation)`, where `feet` is `actor + 0x31c`.
Eye = `head + (0, vr_eye_up, 0)`. `vr_eye_up` is metres, default `0.1`.

The view direction comes from the camera controller (`x0` at `main+0x4c48f4`): eye float3 at `+0xd8`, look-at float3 at
`+0x100`. Back vector `B = eye - look_at`, made level for the right vector `R = (B.z, 0, -B.x)`, `U = B x R`. The head
pose is then applied to this view as above.

Second source for Link's feet (no actor needed): `*(*(*(controller + 0x10) + 0x420) + 0x3e8) + 0x348`, float3. This is
the camera's tracking data.

### Hiding the head

At the skinning-matrix hook (`main+0x14e0220`, `x0` = a skeleton), when `x0` is Link's skeleton: for the head bone and
all 122 bones below it, the world matrix becomes `scale 0.001` with the translation of the head bone. The head shrinks
to a point inside the neck.

This must run here, after the physics. Shrinking the head earlier (after the world-matrix step `main+0x14e1400`) sent the
`Ph_` physics bones flying out in spikes.

Known side effect, **not explained**: after a talk that started in first person, Link looked very dark from behind.

## Walking where you look (verified)

No game hook. In the input poll, the left stick is turned by the head's yaw:

- `a = fwd.x`, `b = -fwd.z`, normalized.
- `x' = x b + y a`, `y' = y b - x a`, clamped to `±32767`.

Test: with the head turned 90 degrees and the stick pushed forward, Link walked to the camera's left.

## Cutscenes (verified for talks)

The signal is the listener view (set-view view 0 from `main+0x33dd88`). Take its eye position from the view matrix:
`e = -(R t.x + U t.y + B t.z)`.

- `side`: horizontal distance from `e` to the nearest of Link's last 4 feet positions (the view follows them a frame or
  two late).
- `up`: `e.y - feet.y` for that nearest one.
- Away = `side > 0.6 m`, or `|up - usual_up| > 0.3 m`. `usual_up` is learned while not away and not in a cutscene:
  `usual_up += (up - usual_up) * 0.05`. In normal play it is about 1.36 m.
- 6 away presents in a row = cutscene. First person and the head hiding stay off until 45 presents after the last away
  present.

Tests: a talk with Saria paused first person. Walking, and climbing onto a ladder (the view moved 0.33 m to the side),
did not.

Not tested: the end of a long talk, story cutscenes, adult Link.

The cutscene camera track `main+0xe7a220` (reads `CameraPos`, `CameraAt`, `CameraUp`) runs only when you enter an area,
not in talks. It is not a cutscene signal.

## Videos (verified)

The title background, the new-game intro and some story scenes are pre-rendered videos: 26 files in `Event/Movie/*.webm`
(for example `Dm_Title_0005.webm`, 99.1 s, and `Dm_Kokiri_0000.webm`, 154.2 s). They play through the GPU video decoder
"Ndve" (`Movie/Ndve/Ndve-Nvn2-vid_seq5.dat.zs`). They do **not** use `movie::Decoder`.

Signal: a video plays exactly while an `Event/Movie/*.webm` file is open (`romfs_videos_open()` in `runtime/romfs.c`).
Test: the intro file was open for 9259 presents at 60 presents per second = 154 s, its own length.

Decision: show videos on a flat screen that stays fixed in the room. Do not lock the video to the head.

## Depth 3D (graphics; facts for reference)

- The game's sun-visibility probe (a 1x1 point pass after the scene) gets the camera in constant buffer `c3`: view rows
  3-5, `P00` at row 6 `.x`, `P11` at row 7 `.y`, depth linearization at row 10: `near / (d * c10.y + c10.x)`, `near` at
  row 11 `.x`. Its texture 0 is the scene's D32F depth.
- The probe does not draw in every frame. With VR on, the depth is copied from the largest non-square D32F pass each
  frame instead.
- The scene is 1280x720 even when the output is larger. With VR on, `resolution = auto` means 100 here, because the game's
  lowest dynamic resolution is too blurry in a headset.

## HUD and menus (verified in Lichii)

UI manager: `M = *(*(main+0x4c7e098))`.

- Screens: count `M+0x8`, array `M+0x10`. A screen's layout is at `screen+0x28`. Its name (`char*`) is at `screen+0x178`.
- ID to screen: the map `*(M+0x48)` is an open-address table, capacity `+0x24`, entries `{u32 id, u32 index}` at
  `+0x18`. Start at `id % capacity`, step +1 with wrap, stop at id 0.
- Open: `+0x1a6 == 2` and signed `+0x1a5 >= 0` (`main+0xa687f0`). Byte `+0x1a8` is not a visibility signal: most
  screens keep it at 1.
- Pane calls: `find_pane(layout, name)` `main+0x43e1e0`; `SetTranslate(s0 x, s1 y, x0 pane)` `main+0x20a3e0`
  (fields `+0x6c`, `+0x70`); `SetScale(s0, s1, s2, x0 pane)` `main+0xb29b20` (`+0x60`, `+0x64`). The origin is the screen
  centre and y is up: the top corner groups sit at y 1078.

| ID | Name | Notes |
|---|---|---|
| `5c7f1c3c` | PlayerStatus_00 | hearts and items; corner groups `N_ScaleL_00` (-1920, 1078), `N_ScaleR_00` (1920, 1078) |
| `3e737418` | (counters) | shop and event counters |
| `c3332ad4` | MapMenu_00 | the minimap |
| `2d9798a8` | DoCommand_00 | the action prompt ("Open") |
| `b116cfbc` | ActionGuide_00 | |
| `5528d3d9` | CtrlGuide_00 | button guide bar |
| `2e6867e2` | Talk_00 | talk box |
| `6b8f8fdd` | MessageDemo_00 | |
| `c0a4dec4` | PlusMenu_00 | pause menu |
| `f5b7168b` | MenuBG_00 | open for the pause menu and the big map |
| `20d55c5a` | MenuOverlay_00 | |
| `8f87396d` | MessageLog_01 | opens with `MessageDialog_00` in every talk: not a menu signal |

Tests: + opened `PlusMenu_00` and `MenuBG_00`; − opened `MenuBG_00` only. Both closed again on the second press.

The HUD code hooks `main+0x340a4b4` (`bl main+0x338b4c0`, the same site as the Edge-to-Edge Ultrawide HUD mod) and moves
panes only when they are at rest, as that mod does. Code-cave use: Dark's code `0x3c46200`, Hyrule Cheat Menu
`0x3c46280-0x3c46398`, ultrawide `0x3c46600-0x3c467c8`, Lichii's own `0x3c46e00-0x3c46f10`; the VR HUD uses
`0x3c46900-0x3c46c20`.

The recomp cutscene signal (the listener view, see "Cutscenes") does not work in Lichii: in normal running with first
person on, that view was 3-4 m from Link's feet (the third-person camera place), so it reported 3 false cutscenes in
one run-and-roll test.

Lichii reads `global.resolution_setup` from `frizti.ini`; that file wins over `qt-config.ini`.

## Not found or not done

- Headset tests: one, with a PlayStation VR2 through SteamVR, before most of this work.
