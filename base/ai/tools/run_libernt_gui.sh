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
DISPLAY_BACKEND="gtk"

for arg in "$@"; do
    case "$arg" in
        --no-mock) USE_MOCK=0 ;;
        --persist) PERSIST=1 ;;
        --sdl) DISPLAY_BACKEND="sdl" ;;
        --gtk) DISPLAY_BACKEND="gtk" ;;
    esac
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
    -netdev user,id=net0 \
    -device e1000e,netdev=net0 \
    -usb -device usb-tablet \
    -vga std \
    -display "$DISPLAY_BACKEND" \
    -serial "file:$SERIAL_LOG"
