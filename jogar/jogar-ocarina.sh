#!/usr/bin/env bash
# Opens Ocarina Recomp on Linux. Build it first with ./compilar.sh --jogo <your game>.
#
# From a terminal it shows the output; when started from a desktop shortcut it writes ultima-partida.log and
# ultima-partida-erros.log next to the project, like the Windows launcher.
#
# On a laptop with two GPUs (Intel/AMD + NVIDIA), run it on the NVIDIA one with PRIME offload, for example
#   prime-run game-performance ./jogar/jogar-ocarina.sh
# or, as a Steam shortcut to this script, the launch options: prime-run game-performance %command%
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The same switches as jogar-ocarina.ps1. TOTK_GPL=0: bindless set 2 + an allocated vertex set in an independent
# layout loses the device. TOTK_ASYNC_PREEMPT=0: the old scheduling exactly. DKB_SKIP_CS: the game's upscaler, giant
# shaders that take many minutes to compile.
export TOTK_GPL=0
export TOTK_ASYNC_PREEMPT=0
export DKB_SKIP_CS="dec1feb3c7fa967a,3f6f2b1d671badcd"
# The environment overrides totk.ini, so the defaults below apply only to keys the player has not saved from the menu:
# otherwise every restart would undo the menu's choice (fullscreen, GPU).
ini="$root/build/oot111/totk.ini"
no_ini_key() {   # no_ini_key KEY...: none of these keys is set in totk.ini (case-insensitive, TOTK_/DKB_ prefix optional)
    local k
    for k in "$@"; do
        [ -f "$ini" ] && grep -Eiq "^[[:space:]]*((totk|dkb)_)?${k}[[:space:]]*=" "$ini" && return 1
    done
    return 0
}
nv_list=""
command -v nvidia-smi >/dev/null 2>&1 && nv_list="$(nvidia-smi -L 2>&1 || true)"
# After a driver update without a restart the old kernel module stays loaded and the new Vulkan driver refuses it:
# Vulkan then lists only the other GPU, and the game runs on it or stops with "no usable Vulkan device".
if printf '%s\n' "$nv_list" | grep -qi 'version mismatch'; then
    echo "jogar-ocarina.sh: the NVIDIA driver was updated but the old one is still loaded (nvidia-smi: Driver/library" \
         "version mismatch). Restart the PC to use the NVIDIA GPU." >&2
fi
if [ -z "${TOTK_GPU_NAME:-}" ] && no_ini_key gpu_name gpu_index && printf '%s\n' "$nv_list" | grep -q '^GPU'; then
    export TOTK_GPU_NAME=NVIDIA
fi
export TOTK_PROFILE_DIR="$root/perfil"

# A fullscreen window on a tiling Wayland compositor (niri, sway) takes the only output and the keyboard, and the
# player cannot reach the chat or the terminal to send a command. Start windowed unless asked otherwise: F11 toggles
# fullscreen, and TOTK_FULLSCREEN=1 ./jogar/jogar-ocarina.sh starts fullscreen. A Steam Deck keeps its profile.
if [ -z "${TOTK_FULLSCREEN:-}" ] && [ -z "${SteamDeck:-}" ] && no_ini_key fullscreen; then
    export TOTK_FULLSCREEN=0
fi

if [ ! -x "$root/build/oot111/ocarina" ]; then
    echo "build/oot111/ocarina not found: run ./compilar.sh --jogo <your game> first" >&2
    exit 1
fi
cd "$root/build/oot111"
if [ -t 1 ]; then
    exec ./ocarina "$@"
else
    # The previous run's logs go to logs/ (the newest 10 runs), as in jogar-ocarina.ps1.
    if [ -s "$root/ultima-partida-erros.log" ]; then
        mkdir -p "$root/logs"
        when="$(date -r "$root/ultima-partida-erros.log" +%Y-%m-%d_%H-%M-%S)"
        mv -f "$root/ultima-partida-erros.log" "$root/logs/partida-$when-erros.log"
        [ -f "$root/ultima-partida.log" ] && mv -f "$root/ultima-partida.log" "$root/logs/partida-$when.log"
        ls -1 "$root/logs"/partida-*-erros.log | sort -r | tail -n +11 | while read -r old; do
            rm -f "$old" "${old%-erros.log}.log"
        done
    fi
    exec ./ocarina "$@" >"$root/ultima-partida.log" 2>"$root/ultima-partida-erros.log"
fi
