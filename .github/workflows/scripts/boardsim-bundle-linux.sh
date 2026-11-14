#!/usr/bin/env bash
# Packs qemu-system-riscv32 with every shared library it needs except the C
# library itself, and points the binary at them with $ORIGIN, so it runs on
# any Linux with glibc as new as the build machine's - no apt install.
#
# usage: boardsim-bundle-linux.sh <build dir> <source dir> <out dir>
set -euo pipefail
BUILD="$1"; SRC="$2"; OUT="$3"
rm -rf "$OUT"; mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/share/qemu"
cp "$BUILD/qemu-system-riscv32" "$OUT/bin/"
cp "$SRC/pc-bios/esp32c3-rom.bin" "$OUT/share/qemu/"
# glibc and the loader stay the system's own.
SYSTEM='^(linux-vdso|ld-linux|libc\.so|libm\.so|libpthread|libdl\.so|librt\.so|libresolv|libutil\.so)'
ldd "$OUT/bin/qemu-system-riscv32" | awk '/=> \//{print $1" "$3}' | while read -r name path; do
  echo "$name" | grep -q -E "$SYSTEM" && continue
  cp -L "$path" "$OUT/lib/$name"
done
patchelf --set-rpath '$ORIGIN/../lib' "$OUT/bin/qemu-system-riscv32"
for lib in "$OUT"/lib/*; do patchelf --set-rpath '$ORIGIN' "$lib"; done
strip "$OUT/bin/qemu-system-riscv32" || true
echo "Bundled: $(ls "$OUT/lib" | tr '\n' ' ')"
"$OUT/bin/qemu-system-riscv32" --version
