# Fritchii fixes for Ocarina of Time: what carries over

Source: [Frizti/Fritchii](https://github.com/Frizti/Fritchii), branch `oot-gather-fix` at `18c6280c0` (2026-11-08), an
Eden/Lichii fork (GPL-3.0-or-later, as this repository). 157 commits name Ocarina; this sorts the ones that change what
the game shows. Perf experiments, the Qt frontend, the updater, DLSS, debug tooling and merges are left out.

Why most of Fritchii cannot be merged: it emulates the Switch 2 GPU (command puller, macros, GPU memory, a Maxwell
shader IR), while this recomp runs the game's NVN calls on its own Vulkan runtime and translates shaders with Ryujinx's
translator. Only two parts are shared code: the Ampere-to-Maxwell lowering (`runtime/third_party/eglantine`) and the
game's own code patches (`recomp/patches-oot111.txt`).

## Ported

| Fritchii | What | Here |
|---|---|---|
| `06bb2da97` | Shadow TXD and ATOMG registers | `lower_to_maxwell.h`, `register_pressure.h` |
| `dd8342353` | Retry lowering with an aligned high uniform base | `lower_to_maxwell.h` |
| `794233f7d` | Gather from a second descriptor buffer as TLD4.B (the branch's namesake) | `lower_to_maxwell.h` |
| `1ad14bebb` | F2I abs/neg on every source form | `lower_to_maxwell.h` |
| (none, ours) | FSEL with a uniform b (0xc08); shadowing for FMNMX imm, P2R, LDC and the cbuf/uniform SEL, FSEL, FMNMX forms. Took the failed programs from 12 to 1 | `lower_to_maxwell.h`, `register_pressure.h` |
| `99ac3d595` | Max Resolution: the game's "dynamic resolution disabled" path | `patches-oot111.txt` set `maxres` (`OOT_MAX_RESOLUTION`), as the flag load before the branch |
| `1a46ba972` | Occlusion Off: no z-prepass or samples-passed queries | `patches-oot111.txt` set `occlusion` (`OOT_OCCLUSION_OFF`) |

Already in the lowering here before the port: `9b327e4b4` (SHF shadowing), `f356d39ba` (remaining compute forms),
`9824171b7` (RED/SURED), `2b2b74190` (ULDC.U8/U16), `3c8dc569f` (ISBEWR, door NaN), `375c4a472`, `9bf296674`,
`2185d5608`, `8390649c0` (gathers). This recomp's own per-tap fallback for offsets it cannot prove is kept; Fritchii
lacks it.

## Covered here by a different design

Fritchii binds storage buffers behind loaded and run-time-indexed pointers in its global-to-storage pass. This recomp
does the same job with guarded candidates: translator patches 0015, 0024 and 0025 turn an access it cannot resolve into
range-guarded reads over a list of reachable buffers, which the runtime builds at each dispatch and draw
(`dyn_collect` in `runtime/vk_exec_draw.c`).

| Fritchii | What | Status here |
|---|---|---|
| `8262db06f` | Culling pass: pointer from a cbuf array at a computed offset; pointer loaded through a cbuf pointer. "Every instance was culled, which hid trees, leaves, fog and sunlight" | Guarded candidates. **Suspect:** the list is heuristic (first 0x100 bytes of each compute SSBO, `OOT_CS_SCAN`; two levels of 0x40 bytes), so a table past that is missed. Test below |
| `749e026c1` | Vegetation: ShuffleIndex (R2UR) walk, table pointers from packed/phi/select/array cbuf pointers | Same as above; patch 0024 covers a fixed table field |
| `8dcd2947a` | Near foliage: select/phi of a cbuf pointer and a pointer loaded from it | Same as above |
| `691c3a066` | Fold x*0, x*1 so the sky ambient binds its buffer | Unchecked: depends on Ryujinx's own folding before GlobalToStorage |
| `d603d98bb`, `d6280ef68` | Bindless handle words: storage-buffer addresses, host-aligned base | Runtime bindless set (patch 0021); unchecked |

## Does not apply (no GPU command emulation here)

Puller, semaphore and fence work (`c905ad9b6`, `547ce11b4`, `f362e5a5f`, `10c3155f8`, `9a7266c62`), macro DMA reads and
snapshots (`8ac3666f7`, `526b9f240`, `41f100674`, `93b903987`, `5df8c96da`, `1bb45da83`, `1c9a048e9`, `22aad455d`),
the GPU split and its experiments (EXP2-EXP19, `3606d26fb`, `5f0c7973b`), GPU memory manager runs (`16b828559`,
`c38631643`, `7c0b5e791`), buffer-cache read-back policy (`dde435084`, `fbc7b1127`, `d65fd98e1`, `0c822822c`,
`df510fbb0`, `b47e55dbc`, `3165a2b6f`), kernel and CPU (`b6706b0c2`, `1ca792919`).

## Open in Fritchii too

- Haze and over-exposure: `4aa1e429a` is an opt-in workaround, not a fix ("the root cause of the haze is still open"):
  fog integration slice distance x0.25 in one compute shader, exposure constant x0.65 in the log-bloom final.
- Dynamic FPS (`765659af4`): a routine written into free space after main's code. Not portable as a site (the lifter
  only converts existing code); it needs a frame-rate patch set of its own.

## Still to port or check

| Fritchii | What | Where it would go |
|---|---|---|
| `5b89a3daf`, `87203d593` | Occlusion counter paused, not reset, across submissions (white flashes) | `vk_exec_xfer.c` query path, if flashes show |
| `57ed438cd` | Split misaligned 64/128-bit SSBO accesses | translator SPIR-V backend patch |
| `77cbc3e7c` | Count nested returns before moving demotes (black scene on AMD) | translator patch |
| `34ae8336e` | NaN-defined float min/max/clamp | patch 0031 (compare) |
| `b3b2d8438`, `05899fd36` | Land deferred clears before image writes; no stale guest memory in new images | runtime texture path |
| `049db02d9`, `1142a380a` | Bypass the game's sharpening anti-aliasing network | code patch or shader edit |

## Missing vegetation: where it breaks (measured 2026-11-08, Kokiri Forest)

Measured with `OOT_DRAW_ARGS=3600` (records copied from GPU memory after the frame's compute) and a same-frame
`TOTK_DUMP=3600`, both patch sets off:

1. The vegetation is 41 indirect mesh draws a frame (programs 4034-4095). All pass `prepare_draw`. Their records,
   read on the GPU, have count 0, so they draw nothing. Forcing the count (`OOT_MESH_DIRECT`) was not a valid test
   (see below); the records are what decides it.
2. The records are written by the culling compute passes. Every one of those runs as an indirect dispatch whose
   arguments are `{0, 1, 1}`, on the GPU and in guest memory, from the first frame on: the culling never runs.
3. No GPU pass writes those dispatch arguments (`TOTK_DUMP_BUF` on one over three frames: never changes). The game's CPU
   writes them, from counts the culling wrote on the GPU and the CPU reads back.
4. With mirrors off (`TOTK_MEMORY=import`) the arguments became 1719, 25, 68, 222... and the vegetation counts 46, 412,
   79...: the chain works when GPU writes reach guest memory. Import mode runs at about 5 fps, so it is not the fix.
5. Cause: a guarded binding with no declared size counted only its first 8 MiB as written (`WRITTEN_CAP`,
   `runtime/vk_exec_draw.c`), and only written ranges go back from the mirror to guest memory. The culling's counts lie
   up to 15.2 MiB in (one instance table of 0xf33080 bytes, its record 0x1f80 after it), so the CPU read 0.
   Fix: the cap is 16 MiB (`written_cap()`, `OOT_WRITTEN_CAP` overrides). Measured: vegetation draws, 28-30 fps,
   GPU 16 ms a frame (256 MiB also drew it, at 20 fps and 27 ms).

Fritchii's `3606d26fb` (dispatches sized by words a compute shader writes into the command stream) is a different
mechanism, for the lighting tiles; it was not needed here.

Ruled out by measurement: the compute pointer scan (`OOT_CS_SCAN=0x10000`: counts still 0), binding slack
(`OOT_SSBO_SLACK=0x2000`: counts still 0; the records sit 0-0x1fe0 bytes past a bound buffer's end), occlusion queries
(the patch set removes the vegetation for good), the 11 shaders the lowering fixes enabled (`OOT_LOWER_REJECT`).

Testing pitfall: in PowerShell 7 `$env:X = ''` leaves X set to an empty string, and the runtime's `getenv` switches take
an empty value as on (or as 0). Start each test from a fresh shell.

## Tests that need no rebuild

Environment variables the runtime reads (set before `jogar\jogar-ocarina.ps1`):

- `OOT_OCCLUSION_OFF=0` / `OOT_MAX_RESOLUTION=0`: the new patch sets off, for A/B.
- `OOT_CS_SCAN=0x10000`: read 64 KiB of each compute storage buffer for pointers instead of 256 bytes. If trees come
  back, the culling pass's table was outside the list.
- `OOT_DYN_COV=<frame>`: logs, for compute dispatches of that frame, the pointers in bound buffers the list misses.
