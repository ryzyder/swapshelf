#!/usr/bin/env bash
# Compila SwapShelf.
#   ./scripts/build.sh pc       -> build/pc/swapshelf (o SwapShelf.exe en MSYS2)
#   ./scripts/build.sh switch   -> build/switch/swapshelf.nro
set -euo pipefail

TARGET="${1:-pc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Dependencias: mGBA y los núcleos libres (nucleos-v2 = FCEUmm, Supafaust y clownmdemu)
if [ ! -f "$ROOT/deps/$TARGET/lib/libmgba.a" ]; then
	"$ROOT/scripts/build_deps.sh" "$TARGET"
elif [ ! -f "$ROOT/deps/$TARGET/nucleos-v2" ]; then
	SWS_ONLY_CORES=1 "$ROOT/scripts/build_deps.sh" "$TARGET"
fi

# Clave de TheGamesDB (opcional): SWS_TGDB_KEY=tu_clave o el archivo clave_thegamesdb.txt
if [ -z "${SWS_TGDB_KEY:-}" ] && [ -f "$ROOT/clave_thegamesdb.txt" ]; then
	SWS_TGDB_KEY="$(tr -d ' \r\n' < "$ROOT/clave_thegamesdb.txt")"
fi

case "$TARGET" in
	pc)
		cmake -S "$ROOT" -B "$ROOT/build/pc" -DSWS_TARGET=pc -DCMAKE_BUILD_TYPE=Release -DSWS_TGDB_KEY="${SWS_TGDB_KEY:-}"
		cmake --build "$ROOT/build/pc" -j"$(nproc 2>/dev/null || echo 4)"
		echo "Listo: carpeta build/pc (tus juegos van en Roms/<consola>)"
		;;
	switch)
		: "${DEVKITPRO:?Instala devkitPro y define DEVKITPRO}"
		cmake -S "$ROOT" -B "$ROOT/build/switch" -DSWS_TARGET=switch \
			-DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" -DCMAKE_BUILD_TYPE=Release -DSWS_TGDB_KEY="${SWS_TGDB_KEY:-}"
		cmake --build "$ROOT/build/switch" -j"$(nproc 2>/dev/null || echo 4)"
		echo "Listo: build/switch/swapshelf.nro -> cópialo a sdmc:/switch/swapshelf/"
		;;
	*)
		echo "Uso: $0 pc|switch"; exit 1;;
esac
