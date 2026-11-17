#!/usr/bin/env bash
# =====================================================================
#  Arma los .zip de una versión de SwapShelf, después de ./compilar_switch.sh
#
#      ./scripts/empaquetar.sh
#
#  Resultado en salida/:
#    SwapShelf-<versión>-Switch.zip          carpeta switch/swapshelf lista para la microSD
#    SwapShelf-<versión>-codigo-fuente.zip   código fuente (sin claves, ROMs ni compilados)
#  Usa python3 o zip, el que esté instalado.
# =====================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
VER="$(sed -n 's/^set(APP_VERSION "\([0-9.]*\)").*/\1/p' CMakeLists.txt | sed 's/\.0$//')"
OUT="$ROOT/salida"
SW="$OUT/switch/swapshelf"
[ -f "$SW/swapshelf.nro" ] || { echo "Falta $SW/swapshelf.nro: corre primero ./compilar_switch.sh"; exit 1; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
# --- Switch: switch/swapshelf con carpetas de Roms vacías (con una nota) ---
mkdir -p "$STAGE/sw/switch"
cp -r "$SW" "$STAGE/sw/switch/"
for c in snes genesis gba gbc gb nes; do
	mkdir -p "$STAGE/sw/switch/swapshelf/Roms/$c"
	echo "Pon aqui tus copias de juegos de $c. SwapShelf no incluye juegos." > "$STAGE/sw/switch/swapshelf/Roms/$c/LEEME.txt"
done
# --- código fuente: lo que se publica (nunca la clave, ROMs, partidas ni compilados) ---
mkdir -p "$STAGE/src/swapshelf-$VER"
for f in src scripts tools assets shaders dist CMakeLists.txt compilar_switch.sh README.md LICENSE swapshelf.ini .gitignore; do
	[ -e "$f" ] && cp -r "$f" "$STAGE/src/swapshelf-$VER/"
done
rm -f "$STAGE/src/swapshelf-$VER/clave_thegamesdb.txt"

zipdir() {  # zipdir <carpeta> <archivo.zip>
	rm -f "$2"
	if command -v python3 >/dev/null; then
		python3 - "$1" "$2" <<'PY'
import os, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
    for base, dirs, files in os.walk(src):
        for d in dirs:
            p = os.path.join(base, d)
            if not os.listdir(p): z.write(p, os.path.relpath(p, src) + "/")
        for f in files:
            p = os.path.join(base, f)
            z.write(p, os.path.relpath(p, src))
PY
	elif command -v zip >/dev/null; then
		(cd "$1" && zip -qr "$2" .)
	else
		echo "No hay python3 ni zip. En MSYS2: pacman -S zip"; exit 1
	fi
}
zipdir "$STAGE/sw" "$OUT/SwapShelf-$VER-Switch.zip"
zipdir "$STAGE/src" "$OUT/SwapShelf-$VER-codigo-fuente.zip"
echo "Listo:"
ls -l "$OUT"/SwapShelf-"$VER"-*.zip
