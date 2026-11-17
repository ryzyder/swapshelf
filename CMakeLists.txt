cmake_minimum_required(VERSION 3.16)
project(SwapShelf C)

set(CMAKE_C_STANDARD 11)
set(APP_NAME    "SwapShelf")
set(APP_AUTHOR  "Zadark")
set(APP_VERSION "0.6.0")

# Clave pública de TheGamesDB para las versiones que se distribuyen.
# NO va en el repositorio: se pasa al compilar, ej. -DSWS_TGDB_KEY=xxxx
# (cada usuario puede poner la suya en swapshelf.ini con thegamesdb_clave=)
set(SWS_TGDB_KEY "" CACHE STRING "Clave pública de TheGamesDB incluida en el ejecutable")

# Dependencias compiladas por scripts/build_deps.sh
set(DEPS_DIR "${CMAKE_SOURCE_DIR}/deps/${SWS_TARGET}" CACHE PATH "Carpeta de dependencias")
list(PREPEND CMAKE_PREFIX_PATH "${DEPS_DIR}")

add_executable(swapshelf src/main.c src/netutil.c src/menu_music.c src/xbr.c src/postfx.c src/gbaui.c src/jpeg.c src/png.c)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
	target_compile_options(swapshelf PRIVATE -Wno-format-truncation)
endif()
target_include_directories(swapshelf PRIVATE "${DEPS_DIR}/include")
if(SWS_TGDB_KEY)
	target_compile_definitions(swapshelf PRIVATE SWS_TGDB_KEY="${SWS_TGDB_KEY}")
endif()
find_library(MGBA_LIB NAMES mgba PATHS "${DEPS_DIR}/lib" "${DEPS_DIR}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(SWS_NES_LIB NAMES swscore_nes PATHS "${DEPS_DIR}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(SWS_SNES_LIB NAMES swscore_snes PATHS "${DEPS_DIR}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(SWS_GENESIS_LIB NAMES swscore_genesis PATHS "${DEPS_DIR}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
target_include_directories(swapshelf PRIVATE "${DEPS_DIR}/include/sws")
target_link_libraries(swapshelf PRIVATE ${SWS_NES_LIB} ${SWS_SNES_LIB} ${SWS_GENESIS_LIB})

if(NINTENDO_SWITCH)
	# ---------- Nintendo Switch (devkitPro / devkitA64) ----------
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(SDL2 REQUIRED sdl2)
	pkg_check_modules(CURL REQUIRED libcurl)
	target_include_directories(swapshelf PRIVATE ${SDL2_INCLUDE_DIRS} ${CURL_INCLUDE_DIRS})
	# mGBA en Switch incluye siempre su renderizador OpenGL ES 3 (aunque usamos SDL),
	# así que se enlaza Mesa igual que hace mGBA en su propio port: GLESv2 + EGL + glapi.
	target_link_libraries(swapshelf PRIVATE ${MGBA_LIB} ${SDL2_LIBRARIES} ${CURL_LIBRARIES}
		GLESv2 EGL glapi drm_nouveau stdc++ nx m)

	if(EXISTS "${CMAKE_SOURCE_DIR}/assets/icon.jpg")
		set(NRO_ICON ICON "${CMAKE_SOURCE_DIR}/assets/icon.jpg")
	endif()
	nx_generate_nacp(swapshelf.nacp NAME "${APP_NAME}" AUTHOR "${APP_AUTHOR}" VERSION "${APP_VERSION}")
	nx_create_nro(swapshelf NACP swapshelf.nacp ${NRO_ICON})
else()
	# ---------- PC (Windows / Linux / macOS) ----------
	find_package(SDL2 REQUIRED CONFIG)
	target_link_libraries(swapshelf PRIVATE ${MGBA_LIB})
	if(TARGET SDL2::SDL2-static AND SWS_STATIC_SDL)
		target_link_libraries(swapshelf PRIVATE SDL2::SDL2main SDL2::SDL2-static stdc++)
	else()
		if(TARGET SDL2::SDL2main)
			target_link_libraries(swapshelf PRIVATE SDL2::SDL2main)
		endif()
		target_link_libraries(swapshelf PRIVATE SDL2::SDL2 stdc++)
	endif()

	if(WIN32)
		target_link_libraries(swapshelf PRIVATE shlwapi ws2_32 winhttp)
		target_link_options(swapshelf PRIVATE -static-libgcc)
		set_target_properties(swapshelf PROPERTIES WIN32_EXECUTABLE ON OUTPUT_NAME "SwapShelf")
		# SDL2.dll al lado del .exe para que funcione con doble clic
		add_custom_command(TARGET swapshelf POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL2::SDL2>" "$<TARGET_FILE_DIR:swapshelf>")
		if(EXISTS "${CMAKE_SOURCE_DIR}/assets/icon.ico")
			file(WRITE "${CMAKE_BINARY_DIR}/icon.rc" "1 ICON \"${CMAKE_SOURCE_DIR}/assets/icon.ico\"\n")
			target_sources(swapshelf PRIVATE "${CMAKE_BINARY_DIR}/icon.rc")
		endif()
	else()
		find_package(Threads REQUIRED)
		target_link_libraries(swapshelf PRIVATE Threads::Threads m)
		set_target_properties(swapshelf PROPERTIES OUTPUT_NAME "swapshelf")
	endif()

	# Copia config e ícono junto al ejecutable
	add_custom_command(TARGET swapshelf POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/swapshelf.ini" "$<TARGET_FILE_DIR:swapshelf>/swapshelf.ini")
	if(EXISTS "${CMAKE_SOURCE_DIR}/assets/icon.bmp")
		add_custom_command(TARGET swapshelf POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/assets/icon.bmp" "$<TARGET_FILE_DIR:swapshelf>/icon.bmp")
	endif()
endif()
