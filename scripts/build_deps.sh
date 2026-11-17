#!/usr/bin/env bash
# Compila libmgba (estática y mínima) para PC o para Switch.
#   ./scripts/build_deps.sh pc
#   ./scripts/build_deps.sh switch     (requiere devkitPro con devkitA64 y switch-sdl2)
set -euo pipefail

TARGET="${1:-pc}"
MGBA_TAG="0.10.5"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/deps/src/mgba"
OUT="$ROOT/deps/$TARGET"

# El instalador actual de devkitPro deja devkitA64 dentro de DEVKITPRO, pero
# no siempre crea la variable aparte. Se completa solo para el objetivo Switch.
if [ "$TARGET" = "switch" ] && [ -n "${DEVKITPRO:-}" ] && [ -z "${DEVKITA64:-}" ]; then
	export DEVKITA64="$DEVKITPRO/devkitA64"
fi

# Núcleos de las consolas adicionales, todos con licencia libre compatible con
# la GPL (se pueden enlazar juntos y distribuir con su código fuente):
#   NES     FCEUmm      GPLv2 o posterior
#   SNES    Supafaust   GPLv2 o posterior (snes_faust de Mednafen)
#   Genesis clownmdemu  AGPLv3
# Se fijan a revisiones concretas para que una compilación futura no cambie sin aviso.
FCEUMM_REF="8997c2bf9503f6d50585b11a1fd7207af53a2426"
SUPAFAUST_REF="642d1d1b6684aa7e306a02a89885f3f5456a5157"
CLOWNMDEMU_REF="004044c2cee2b3e3c7452145a40f7b5ea648d8c4"

if [ ! -d "$SRC" ]; then
	git clone --depth 1 --branch "$MGBA_TAG" https://github.com/mgba-emu/mgba.git "$SRC"
fi

COMMON=(
	-DLIBMGBA_ONLY=ON -DBUILD_STATIC=ON -DBUILD_SHARED=OFF
	# SwapShelf usa mGBA tanto para GBA como para GB y GBC. Si se desactiva
	# M_CORE_GB, las pestañas GB/GBC aparecen en la biblioteca pero mCoreFind()
	# no puede crear el núcleo al abrir una ROM.
	-DM_CORE_GB=ON -DUSE_DEBUGGERS=OFF -DENABLE_SCRIPTING=OFF
	-DUSE_EPOXY=OFF -DUSE_FFMPEG=OFF -DUSE_ZLIB=OFF -DUSE_PNG=OFF
	-DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF -DUSE_SQLITE3=OFF -DUSE_ELF=OFF
	-DUSE_LUA=OFF -DUSE_JSON_C=OFF -DUSE_DISCORD_RPC=OFF -DUSE_LZMA=OFF
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$OUT"
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5
)

# Copia solo lo necesario (librería + cabeceras); evita instalar el
# emulador completo de mGBA, que no se compila.
install_lib() {
	local B="$1"
	mkdir -p "$OUT/lib" "$OUT/include/mgba"
	cp "$B/libmgba.a" "$OUT/lib/"
	cp -r "$SRC/include/mgba" "$SRC/include/mgba-util" "$OUT/include/"
	cp "$B/include/mgba/flags.h" "$OUT/include/mgba/"
	# La librería se compila sin MINIMAL_CORE aunque flags.h lo declare (Switch):
	# se quita para que las estructuras coincidan con la librería.
	sed -i '/#define MINIMAL_CORE/d' "$OUT/include/mgba/flags.h"
}

fetch_core() {
	local name="$1" url="$2" ref="$3" dir
	dir="$ROOT/deps/src/$name"
	if [ ! -d "$dir/.git" ]; then git clone --recurse-submodules "$url" "$dir"; fi
	git -C "$dir" fetch --depth 1 origin "$ref"
	git -C "$dir" checkout --detach "$ref"
	git -C "$dir" submodule update --init --recursive
}

# Herramientas del objetivo
if [ "$TARGET" = "switch" ]; then
	TOOL_CC="${DEVKITA64:-}/bin/aarch64-none-elf-gcc"
	TOOL_CXX="${DEVKITA64:-}/bin/aarch64-none-elf-g++"
	TOOL_LD="${DEVKITA64:-}/bin/aarch64-none-elf-ld"
	TOOL_OBJCOPY="${DEVKITA64:-}/bin/aarch64-none-elf-objcopy"
	TOOL_AR="${DEVKITA64:-}/bin/aarch64-none-elf-ar"
	NX_FLAGS="-O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections -fdata-sections -D__SWITCH__"
else
	TOOL_CC="${CC:-cc}"; TOOL_CXX="${CXX:-c++}"; TOOL_LD="${LD:-ld}"
	TOOL_OBJCOPY="${OBJCOPY:-objcopy}"; TOOL_AR="${AR:-ar}"
	NX_FLAGS="-O2"
fi

# Deja un núcleo como un único objeto que solo exporta sus funciones
# sws_<id>_retro_*: sus copias internas de libretro-common y demás símbolos
# globales quedan ocultos, así no chocan con los otros núcleos.
# (Solo para núcleos en C; en C++ se evita para no romper los grupos COMDAT
# compartidos con la librería estándar estática de Switch.)
isolate_core() {
	local id="$1" lib="$2" tmp
	tmp="$(mktemp -d)"
	"$TOOL_LD" -r -o "$tmp/core_$id.o" --whole-archive "$lib"
	"$TOOL_OBJCOPY" -w --keep-global-symbol="sws_${id}_retro_*" "$tmp/core_$id.o"
	rm -f "$OUT/lib/libswscore_$id.a"
	"$TOOL_AR" rcs "$OUT/lib/libswscore_$id.a" "$tmp/core_$id.o"
	rm -rf "$tmp"
}

# Compila un núcleo libretro con su propio Makefile como archivo estático.
# El encabezado forzado renombra retro_* con un prefijo único.
build_core() {
	local id="$1" dir="$2" makefile="$3" prefix="$4" target_name="$5" isolate="${6:-no}"
	local cc cxx platform result
	local -a extra_make=()
	cc="$TOOL_CC -include '$ROOT/src/libretro_prefix.h' -DSWS_RETRO_PREFIX=$prefix"
	cxx="$TOOL_CXX -include '$ROOT/src/libretro_prefix.h' -DSWS_RETRO_PREFIX=$prefix"
	if [ "$TARGET" = "switch" ]; then platform="libnx"; else platform="unix"; fi
	if [ "$id" = "nes" ]; then
		# FCEUmm omite libretro-common al compilar un archivo estático porque
		# normalmente lo aporta RetroArch. SwapShelf es su propio frontend, así
		# que se incluyen esas utilidades dentro del archivo. HD packs quedan
		# fuera para mantener el tamaño y las dependencias de Switch acotados.
		local L=src/drivers/libretro/libretro-common
		extra_make+=(HAVE_HDPACK=0)
		extra_make+=("RETROARCH_OBJECTS=$L/compat/compat_posix_string.o $L/compat/compat_snprintf.o $L/compat/compat_strcasestr.o $L/compat/compat_strl.o $L/compat/fopen_utf8.o $L/encodings/encoding_utf.o $L/file/file_path.o $L/file/file_path_io.o $L/streams/file_stream.o $L/streams/file_stream_transforms.o $L/string/stdstring.o $L/time/rtime.o $L/vfs/vfs_implementation.o")
	fi
	make -C "$dir" -f "$makefile" clean >/dev/null 2>&1 || true
	if [ "$TARGET" = "switch" ]; then
		# switch_rules selecciona el ar de devkitA64; no se lo debe sobrescribir.
		make -C "$dir" -f "$makefile" -j"$(nproc 2>/dev/null || echo 4)" platform="$platform" STATIC_LINKING=1 STATIC_LINKING_LINK=1 CC="$cc" CXX="$cxx" "${extra_make[@]}"
	else
		make -C "$dir" -f "$makefile" -j"$(nproc 2>/dev/null || echo 4)" platform="$platform" STATIC_LINKING=1 STATIC_LINKING_LINK=1 CC="$cc" CXX="$cxx" AR="$TOOL_AR" "${extra_make[@]}"
	fi
	result="$dir/$target_name"
	[ -f "$result" ] || result="$(ls "$dir"/*.a "$dir"/*_libretro*.so 2>/dev/null | head -1)"
	[ -f "$result" ] || { echo "No se generó el núcleo $id en $dir" >&2; exit 1; }
	mkdir -p "$OUT/lib" "$OUT/include/sws"
	if [ "$isolate" = "si" ]; then isolate_core "$id" "$result"; else cp "$result" "$OUT/lib/libswscore_$id.a"; fi
}

# clownmdemu se compila directo (un único unity.c en C portable); su Makefile
# no trae plataforma libnx.
build_clownmdemu() {
	local dir="$ROOT/deps/src/clownmdemu" tmp
	tmp="$(mktemp -d)"
	"$TOOL_CC" $NX_FLAGS -DNDEBUG -include "$ROOT/src/libretro_prefix.h" -DSWS_RETRO_PREFIX=sws_genesis_ \
		-I"$dir/libretro-common/include" -c "$dir/unity.c" -o "$tmp/unity.o"
	"$TOOL_AR" rcs "$tmp/libclownmdemu.a" "$tmp/unity.o"
	mkdir -p "$OUT/lib"
	isolate_core genesis "$tmp/libclownmdemu.a"
	rm -rf "$tmp"
}

build_libretro_cores() {
	# Las versiones anteriores usaban Snes9x 2005 y PicoDrive: si quedan sus
	# archivos, se borran para compilar los núcleos libres actuales.
	if [ ! -f "$OUT/nucleos-v2" ]; then
		rm -f "$OUT/lib/libswscore_nes.a" "$OUT/lib/libswscore_snes.a" "$OUT/lib/libswscore_genesis.a"
	fi
	fetch_core fceumm https://github.com/libretro/libretro-fceumm.git "$FCEUMM_REF"
	fetch_core supafaust https://github.com/libretro/supafaust.git "$SUPAFAUST_REF"
	fetch_core clownmdemu https://github.com/Clownacy/clownmdemu-libretro.git "$CLOWNMDEMU_REF"
	if [ "$TARGET" = "switch" ]; then
		[ -f "$OUT/lib/libswscore_nes.a" ] || build_core nes "$ROOT/deps/src/fceumm" Makefile.libretro sws_nes_ fceumm_libretro_libnx.a si
		[ -f "$OUT/lib/libswscore_snes.a" ] || build_core snes "$ROOT/deps/src/supafaust" Makefile sws_snes_ mednafen_supafaust_libretro_libnx.a
	else
		[ -f "$OUT/lib/libswscore_nes.a" ] || build_core nes "$ROOT/deps/src/fceumm" Makefile.libretro sws_nes_ fceumm_libretro.so si
		[ -f "$OUT/lib/libswscore_snes.a" ] || build_core snes "$ROOT/deps/src/supafaust" Makefile sws_snes_ mednafen_supafaust_libretro.so
	fi
	[ -f "$OUT/lib/libswscore_genesis.a" ] || build_clownmdemu
	# El API libretro es el mismo en los tres; se instala una copia para el frontend.
	cp "$ROOT/deps/src/fceumm/src/drivers/libretro/libretro-common/include/libretro.h" "$OUT/include/sws/libretro.h"
	echo "FCEUmm $FCEUMM_REF / Supafaust $SUPAFAUST_REF / clownmdemu $CLOWNMDEMU_REF" > "$OUT/nucleos-v2"
}

# Útil para retomar una compilación de núcleos sin reconstruir mGBA.
if [ "${SWS_ONLY_CORES:-0}" = "1" ]; then
	build_libretro_cores
	echo "Núcleos libretro listos en $OUT"
	exit 0
fi

case "$TARGET" in
	pc)
		# MSYS2/GCC necesita exponer las extensiones POSIX para locale_t, strdup
		# y pthread_setname_np que usa libmgba.
		cmake -S "$SRC" -B "$SRC/build-pc" "${COMMON[@]}" -DCMAKE_C_FLAGS="-D_GNU_SOURCE"
		cmake --build "$SRC/build-pc" --target mgba -j"$(nproc 2>/dev/null || echo 4)"
		install_lib "$SRC/build-pc"
		build_libretro_cores
		;;
	switch)
		: "${DEVKITPRO:?Instala devkitPro y define DEVKITPRO (p. ej. /opt/devkitpro)}"
		cmake -S "$SRC" -B "$SRC/build-switch" -G "Unix Makefiles" "${COMMON[@]}" \
			-DCMAKE_TOOLCHAIN_FILE="$SRC/src/platform/switch/CMakeToolchain.txt"
		cmake --build "$SRC/build-switch" --target mgba -j"$(nproc 2>/dev/null || echo 4)"
		install_lib "$SRC/build-switch"
		build_libretro_cores
		;;
	*)
		echo "Uso: $0 pc|switch"; exit 1;;
esac
echo "libmgba lista en $OUT"
