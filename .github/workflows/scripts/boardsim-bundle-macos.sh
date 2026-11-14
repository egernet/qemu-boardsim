#!/bin/bash
# Packs BoardSim's QEMU fork with every non-system library it loads, so it
# runs on a Mac without Homebrew:
#   bin/qemu-system-riscv32   lib/*.dylib   share/qemu/esp32c3-rom.bin
#
# usage: scripts/bundle-qemu-macos.sh <qemu build dir> <qemu source dir> <out dir>
set -euo pipefail
BUILD="$1"; SRC="$2"; OUT="$3"
BREW="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
rm -rf "$OUT"; mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/share/qemu"
cp "$BUILD/qemu-system-riscv32" "$OUT/bin/"
cp "$SRC/pc-bios/esp32c3-rom.bin" "$OUT/share/qemu/"

# Where a dependency of $1 (originally at $2) really lives.
resolve() {
  local dep="$1" origin="$2" name; name=$(basename "$dep")
  case "$dep" in
    @loader_path/*|@executable_path/*) python3 -c "import os,sys; print(os.path.realpath(os.path.join(os.path.dirname(sys.argv[1]), sys.argv[2])))" "$origin" "${dep#*/}" ;;
    @rpath/*) for d in "$BREW/lib" "$BREW"/opt/*/lib; do [ -f "$d/$name" ] && { echo "$d/$name"; return; }; done ;;
    *) echo "$dep" ;;
  esac
}

declare -a queue=("$OUT/bin/qemu-system-riscv32|$BUILD/qemu-system-riscv32")
while [ ${#queue[@]} -gt 0 ]; do
  entry="${queue[0]}"; queue=("${queue[@]:1}")
  file="${entry%%|*}"; origin="${entry#*|}"
  for dep in $(otool -L "$file" | tail -n +2 | awk '{print $1}' | grep -v -E '^(/usr/lib|/System)'); do
    name=$(basename "$dep")
    [ "$dep" = "@rpath/$(basename "$file")" ] && continue          # its own id
    if [ ! -f "$OUT/lib/$name" ]; then
      real=$(resolve "$dep" "$origin")
      [ -n "$real" ] && [ -f "$real" ] || { echo "cannot find $dep (needed by $origin)"; exit 1; }
      cp -L "$real" "$OUT/lib/$name"; chmod u+w "$OUT/lib/$name"
      install_name_tool -id "@rpath/$name" "$OUT/lib/$name" 2>/dev/null
      queue+=("$OUT/lib/$name|$real")
    fi
    install_name_tool -change "$dep" "@rpath/$name" "$file" 2>/dev/null
  done
done

# Drop the build machine's rpaths so nothing is found in Homebrew by accident.
for f in "$OUT/bin/qemu-system-riscv32" "$OUT"/lib/*.dylib; do
  for rp in $(otool -l "$f" | awk '/LC_RPATH/{getline; getline; print $2}'); do
    install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null || true
  done
done
install_name_tool -add_rpath "@executable_path/../lib" "$OUT/bin/qemu-system-riscv32"
for lib in "$OUT"/lib/*.dylib; do install_name_tool -add_rpath "@loader_path" "$lib"; done
# Changing load commands breaks the signature; arm64 needs at least an ad-hoc one.
codesign --force -s - "$OUT"/lib/*.dylib "$OUT/bin/qemu-system-riscv32" 2>/dev/null
echo "Bundled into $OUT: $(ls "$OUT/lib" | tr '\n' ' ')"
