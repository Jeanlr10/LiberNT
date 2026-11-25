#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_DIR="${LIBERNT_BUILD_DIR:-$REPO_ROOT/output-Clang-amd64-debug}"
IMG="$BUILD_DIR/LiberNT.img"
OVMF_CODE="/usr/share/edk2/ovmf/OVMF_CODE.fd"
OVMF_VARS="/usr/share/edk2/ovmf/OVMF_VARS.fd"

OVERLAY="/tmp/libernt_gui_overlay.qcow2"
VARS_COPY="/tmp/libernt_gui_ovmf_vars.fd"
SERIAL_LOG="/tmp/libernt_gui_serial.log"
MOCK_LOG="/tmp/libernt_gui_mock_openai.log"

USE_MOCK=1
PERSIST=0
NO_NET=0
LOCAL_MODEL=""
DISPLAY_BACKEND="gtk"

while [ $# -gt 0 ]; do
    case "$1" in
        --no-mock) USE_MOCK=0 ;;
        --no-net) NO_NET=1; USE_MOCK=0 ;;
        --persist) PERSIST=1 ;;
        --sdl) DISPLAY_BACKEND="sdl" ;;
        --gtk) DISPLAY_BACKEND="gtk" ;;
        --local-model)
            shift
            LOCAL_MODEL="${1:-}"
            ;;
        --local-model=*)
            LOCAL_MODEL="${1#*=}"
            ;;
    esac
    shift
done

if [ ! -f "$IMG" ]; then
    echo "Error: $IMG not found." >&2
    exit 1
fi

if [ "$PERSIST" -eq 0 ] || [ ! -f "$OVERLAY" ]; then
    rm -f "$OVERLAY"
    qemu-img create -f qcow2 -b "$IMG" -F raw "$OVERLAY" >/dev/null
fi
if [ "$PERSIST" -eq 0 ] || [ ! -f "$VARS_COPY" ]; then
    cp "$OVMF_VARS" "$VARS_COPY"
fi
rm -f "$SERIAL_LOG"

MOCK_PID=""
cleanup() {
    if [ -n "$MOCK_PID" ]; then
        kill "$MOCK_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

if [ "$USE_MOCK" -eq 1 ]; then
    if ! ss -tln 2>/dev/null | grep -q ":11434 "; then
        python3 "$SCRIPT_DIR/mock_openai_server.py" --host 127.0.0.1 --port 11434 >"$MOCK_LOG" 2>&1 &
        MOCK_PID=$!
        sleep 0.5
        echo "Started LiberNT mock OpenAI server on http://127.0.0.1:11434/v1 (guest: http://10.0.2.2:11434/v1)"
    else
        echo "Host port 11434 already active (e.g. Ollama) — guest reaches it at http://10.0.2.2:11434/v1"
    fi
fi

MODELS_IMG="/tmp/libernt_gui_models.img"
EXTRA_QEMU_ARGS=()

if [ -n "$LOCAL_MODEL" ]; then
    if [ ! -f "$LOCAL_MODEL" ]; then
        echo "Error: local model $LOCAL_MODEL not found." >&2
        exit 1
    fi
    NTFSIMG="$BUILD_DIR/host-tools/bin/ntfsimg"
    MKDISKIMG="$BUILD_DIR/host-tools/bin/mkdiskimg"
    MODELS_PART="/tmp/libernt_gui_models.ntfs"
    MODELS_LIST="/tmp/libernt_gui_models.lst"
    MODELS_CMD="/tmp/libernt_gui_enable_local.cmd"
    MODEL_BASE="$(basename "$LOCAL_MODEL")"
    MODEL_BYTES="$(stat -c%s "$LOCAL_MODEL")"
    SECTORS=$(( (MODEL_BYTES / 512) + 65536 ))
    if [ "$SECTORS" -lt 32768 ]; then
        SECTORS=32768
    fi
    SECTORS=$(( (SECTORS + 7) & ~7 ))

    rm -f "$MODELS_IMG" "$MODELS_PART" "$MODELS_LIST" "$MODELS_CMD"
    printf '@echo off\r\nai config set --provider local --model-path D:\\%s --max-tokens 128\r\nai config\r\npause\r\n' "$MODEL_BASE" > "$MODELS_CMD"
    printf '%s=%s\nenable_local_ai.cmd=%s\n' "$MODEL_BASE" "$LOCAL_MODEL" "$MODELS_CMD" > "$MODELS_LIST"
    "$NTFSIMG" --format "$MODELS_PART" "$SECTORS" 2048 8 MODELS >/dev/null
    "$NTFSIMG" --addfiles "$MODELS_PART" "$MODELS_LIST" >/dev/null
    "$MKDISKIMG" -o "$MODELS_IMG" -partition "$MODELS_PART" -start 2048 -type 07 >/dev/null
    rm -f "$MODELS_PART" "$MODELS_LIST" "$MODELS_CMD"

    EXTRA_QEMU_ARGS+=(
        -drive "id=disk1,if=none,format=raw,file=$MODELS_IMG"
        -device "ide-hd,drive=disk1,bus=ahci.1"
    )
    echo "Mounted local GGUF model on D:\\$MODEL_BASE (run D:\\enable_local_ai.cmd inside LiberNT)"
fi

if [ "$NO_NET" -eq 1 ]; then
    EXTRA_QEMU_ARGS+=(-nic none)
else
    EXTRA_QEMU_ARGS+=(-netdev user,id=net0 -device e1000e,netdev=net0)
fi

echo "Launching LiberNT AI OS interactive GUI VM (serial log: $SERIAL_LOG)..."
qemu-system-x86_64 \
    -machine q35,accel=kvm \
    -cpu host \
    -smp 4 \
    -m 4096 \
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
    -drive "if=pflash,format=raw,file=$VARS_COPY" \
    -drive "id=disk0,if=none,format=qcow2,file=$OVERLAY" \
    -device ich9-ahci,id=ahci \
    -device ide-hd,drive=disk0,bus=ahci.0 \
    "${EXTRA_QEMU_ARGS[@]}" \
    -usb -device usb-tablet \
    -vga std \
    -display "$DISPLAY_BACKEND" \
    -serial "file:$SERIAL_LOG"
