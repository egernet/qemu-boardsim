#!/usr/bin/env bash
# Configures the riscv32 (ESP32-C3) emulator the way BoardSim needs it: no
# GUI, gcrypt and slirp (the emulated network). Windows builds statically,
# like Espressif's own release, so qemu-system-riscv32.exe needs no DLLs.
set -euo pipefail
EXTRA=()
case "${BOARDSIM_PLATFORM:-}" in
  windows-*)
    # libintl pulls in libiconv; Espressif's workaround for the static MinGW link.
    sed -z -i "s/qemu_ldflags = \[\]/qemu_ldflags = \['-liconv','-Wl,--allow-multiple-definition'\]/g" -- meson.build
    EXTRA=(--static --enable-iconv)
    ;;
  macos-*)
    sed -i '' "s/project('qemu', \['c'\],/project('qemu', \['c', 'objc'\],/" meson.build
    EXTRA=(--enable-fdt=internal)
    ;;
esac
PY=()
[ -n "${BOARDSIM_PYTHON:-}" ] && PY=(--python="$BOARDSIM_PYTHON")
./configure \
  --target-list=riscv32-softmmu \
  --without-default-features \
  --enable-tcg --enable-system \
  --enable-gcrypt --enable-slirp \
  --disable-docs --disable-sdl --disable-gtk --disable-cocoa --disable-capstone \
  --disable-werror \
  --with-pkgversion="boardsim-${VERSION:-dev}" \
  ${EXTRA[@]+"${EXTRA[@]}"} ${PY[@]+"${PY[@]}"} \
  || { cat build/meson-logs/meson-log.txt 2>/dev/null | tail -60; false; }
