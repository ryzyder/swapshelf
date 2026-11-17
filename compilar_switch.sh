#!/usr/bin/env bash
# =====================================================================
#  Compila SwapShelf para Nintendo Switch (homebrew .nro)
#
#  Uso (en la consola MSYS2 de devkitPro, desde la carpeta del proyecto):
#      ./compilar_switch.sh
#
#  Resultado: salida/switch/swapshelf/  -> copia esa carpeta "swapshelf"
#  completa a  sdmc:/switch/  en la microSD de la consola.
# =====================================================================
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(pwd)"

echo "== 1/4 Comprobando devkitPro =="
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
if [ ! -d "$DEVKITPRO/libnx" ]; then
	echo "No se encontró devkitPro en $DEVKITPRO."
	echo "Instálalo con 'Switch Development' marcado y abre esta consola desde Inicio -> devkitPro -> MSYS2."
	exit 1
fi
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"

echo "== 2/4 Paquetes necesarios =="
NEED=(switch-dev switch-sdl2 switch-curl switch-cmake git cmake make)
if command -v pacman >/dev/null; then
	pacman -S --needed --noconfirm "${NEED[@]}" || {
		echo "Aviso: no se pudieron instalar los paquetes automáticamente."
		echo "Ejecuta:  pacman -S --needed ${NEED[*]}"
	}
elif command -v dkp-pacman >/dev/null; then
	sudo dkp-pacman -S --needed --noconfirm switch-dev switch-sdl2 switch-curl switch-cmake
fi

echo "== 3/4 Compilando los núcleos para Switch (solo la primera vez) =="
# nucleos-v2 = FCEUmm (NES), Supafaust (SNES) y clownmdemu (Genesis), todos libres.
if [ ! -f "$ROOT/deps/switch/lib/libmgba.a" ]; then
	"$ROOT/scripts/build_deps.sh" switch
elif [ ! -f "$ROOT/deps/switch/nucleos-v2" ]; then
	SWS_ONLY_CORES=1 "$ROOT/scripts/build_deps.sh" switch
fi

echo "== 4/4 Compilando el juego =="
# Clave de TheGamesDB (opcional): SWS_TGDB_KEY=tu_clave ./compilar_switch.sh
# o un archivo "clave_thegamesdb.txt" junto a este script (no lo subas a GitHub).
if [ -z "${SWS_TGDB_KEY:-}" ] && [ -f "$ROOT/clave_thegamesdb.txt" ]; then
	SWS_TGDB_KEY="$(tr -d ' \r\n' < "$ROOT/clave_thegamesdb.txt")"
fi
cmake -S "$ROOT" -B "$ROOT/build/switch" -G "Unix Makefiles" -DSWS_TARGET=switch \
	-DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" -DCMAKE_BUILD_TYPE=Release \
	-DSWS_TGDB_KEY="${SWS_TGDB_KEY:-}"
cmake --build "$ROOT/build/switch" -j"$(nproc 2>/dev/null || echo 4)"

OUT="$ROOT/salida/switch/swapshelf"
rm -rf "$OUT"
mkdir -p "$OUT/Roms/snes" "$OUT/Roms/genesis" "$OUT/Roms/gba" "$OUT/Roms/gbc" "$OUT/Roms/gb" "$OUT/Roms/nes"
cp "$ROOT/build/switch/swapshelf.nro" "$OUT/"
cp "$ROOT/swapshelf.ini" "$OUT/"
cp "$ROOT/dist/LEEME.txt" "$ROOT/dist/README.txt" "$ROOT/dist/LEIAME.txt" "$OUT/"
cp "$ROOT/LICENSE" "$OUT/LICENSE.txt"

echo
echo "=============================================================="
echo " Listo: $OUT"
echo " Copia la carpeta 'swapshelf' completa a  sdmc:/switch/"
echo " y pon tus propias copias de juegos en  sdmc:/switch/swapshelf/Roms/<consola>/"
echo " Ábrelo manteniendo R al iniciar un juego (modo aplicación, todo el rendimiento)."
echo "=============================================================="
