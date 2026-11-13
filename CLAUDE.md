@AGENTS.md

## OoT VR

This fork (`feat/vr-openxr`, remote `vr` = SinCuri0/Curios-recomp-VR) finds the game facts for the VR mod. The mod
itself is moving to Lichii in `D:\Games\Emulators\lichii repo` (branch `feat/oot-vr`, notes in `docs/nx2/oot-vr.md`).
Write every fact the VR mode uses in `docs/vr-port-notes.md`, with its status: verified, read, or not tested.

Ghidra 12.1.4 runs headless in WSL Ubuntu (`/opt/ghidra_12.1.4_PUBLIC`), not in `toolchain\ghidra\` as
`docs/building.md` says. Project: `/home/alexander/ghidra-oot/oot.gpr` with `main.elf` at `0x7000000000`. Run a script:
`wsl -d Ubuntu -- bash /mnt/d/Games/Emulators/recomp/tools/ghidra/analyze.sh --script <Name>.java <args>`. Keep new
scripts in `tools/ghidra/scripts/` (tracked). Write script output to `build/ghidra/` (ignored): it comes from the game.

## Sync and commits

- At the start of a session: `git status`, then `git fetch --all --prune`, then report how far `HEAD` is from
  `origin/main` and `vr/feat/vr-openxr`.
- Commit each working step. Do not end a session with uncommitted work; make a WIP commit if needed.
- Ask before you push. Push target: `vr`.
- Merge `origin/main` with `git merge`, never rebase pushed branches.

## Builds

Run every build and every other long command (more than about a minute) in a Terminal panel tab with `run_in_terminal`, so the user sees the live output. Do not run them hidden in the background. Link the tab in the reply. For a build, write its output to a log file and show only progress with `& 'D:\Games\Emulators\recomp\tools\build_progress.ps1' -Log <log>` (one progress line, errors in red), not the raw output.
