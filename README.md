# SwapShelf (SWS)

Una estantería de cartuchos para tus propias copias de juegos retro, en PC (Windows / Linux) y Nintendo Switch (homebrew).

**SwapShelf no incluye ni descarga juegos.** Solo muestra y abre las copias de seguridad que tú pongas en la carpeta `Roms`. De internet solo se descargan nombres oficiales y portadas.

## Qué hace

- **Biblioteca por consola:** SNES, Genesis, GBA, GBC, GB y NES, cada una en su pestaña y en su carpeta (`Roms/snes`, `Roms/genesis`, `Roms/gba`, `Roms/gbc`, `Roms/gb`, `Roms/nes`).
- **Identificación por CRC32 y header:** reconoce cada juego por su huella, aunque el archivo tenga otro nombre.
- **Nombres oficiales (No-Intro):** descarga una vez la base No-Intro de cada consola desde [libretro-database](https://github.com/libretro/libretro-database) y busca el nombre por CRC32. Si el CRC no está pero el código del juego coincide, lo marca como hack o versión modificada del original.
- **Portadas (TheGamesDB):** busca la portada de la caja (JPG o PNG) por nombre, solo entre los juegos de esa consola y con los lanzamientos oficiales primero. Cada juego se consulta una sola vez y queda guardado. Desde el menú de cada juego (**Y** en Switch, **M** en PC; o **P** directo en PC) se elige la portada a mano entre las coincidencias, con su año y si es oficial, variante o hack.
- **Tarjetas tipo cartucho:** banda del color de la consola y etiqueta con un diseño distinto para GBA/GB, SNES y Genesis, con la portada entera y el título.
- **Animaciones de cartucho:** el cartucho entra en la ranura al jugar y sale al volver a la biblioteca, con sonido.
- **Filtro Zadark HD:** suavizado de bordes (xBR), color vivo y textura, con niveles en la pausa.
- **Controles:** perfil global por consola o solo para un juego, con los botones reales de cada consola; teclado y mando por separado.
- **Partidas:** una por juego en `Partidas/`, con respaldo automático si la ROM cambia.

Se juegan las seis consolas:

| Consola | Núcleo | Licencia |
|---|---|---|
| GBA, GBC, GB | [mGBA](https://mgba.io) (libmgba) | MPL 2.0 |
| NES | [FCEUmm](https://github.com/libretro/libretro-fceumm) | GPLv2 o posterior |
| SNES | [Supafaust](https://github.com/libretro/supafaust) (snes_faust de Mednafen) | GPLv2 o posterior |
| Genesis / Mega Drive | [clownmdemu](https://github.com/Clownacy/clownmdemu-libretro) | AGPLv3 |

Los núcleos de NES, SNES y Genesis van enlazados dentro del mismo ejecutable a través de una capa libretro propia, con su partida normal (SRAM) guardada automáticamente, estados rápidos, filtro Zadark HD y controles por consola.

- **Biblioteca grande:** la pestaña "Inicio" muestra los últimos 10 juegos jugados y debajo un acceso a cada consola. El menú de cada juego permite también borrar la ROM (las partidas se conservan). Cada consola se ve en cuadrícula de 8 por fila, con orden (A-Z, Recientes, Más jugados, Favoritos) y salto por letra. La búsqueda (− en Switch, Ctrl+F en PC) busca en el nombre, el nombre oficial y la consola, con teclado en pantalla. El historial queda en `Datos/jugados.txt`, sin internet.

| Acción | PC | Switch |
|---|---|---|
| Jugar | Enter | A |
| Consola | Q / E | L / R |
| Menú del juego | M | Y |
| Favorito | F | X |
| Buscar | Ctrl+F o / | − |
| Orden | O | + |
| Letra anterior / siguiente | RePág / AvPág | ZL / ZR |

**Consolas visibles:** las consolas sin juegos se ocultan solas; en Opciones → Consolas visibles se puede ocultar cualquiera (`ocultar=gb,gbc` en el ini).

**Tiempo jugado:** se guarda por juego en `Datos/jugados.txt` (sin contar la pausa) y se ve en la ficha del juego; el orden "Más jugados" usa el tiempo.

**Tema:** menús oscuros o claros y color de acento (cian, el de la S del logo, por defecto; también rojo, verde, violeta o naranja). En Opciones o con `tema=` y `color=` en `swapshelf.ini`.

**Idiomas:** español, inglés y portugués (Opciones → Idioma, o `idioma=es|en|pt` en `swapshelf.ini`). Los textos están en `tools/traducciones.py`; `python3 tools/gen_lang.py` regenera `src/lang.inc`. En el código, todo texto visible va dentro de `T("...")` en español.

**Opciones:** en el inicio, el último recuadro de la fila de consolas (o F2 en PC) abre las opciones generales: cartuchos por fila, aumento, imagen del cartucho, seguir donde lo dejé, tamaño y costados del juego, volúmenes, intercambiar A y B, turbo, idioma, carpeta de juegos, actualizar biblioteca y Acerca de. Cada cambio se guarda solo en `swapshelf.ini`.

En `swapshelf.ini`, `por_fila=` (5 a 9, por defecto 7) elige cuántos cartuchos entran por fila en cada consola, y `aumento=` (100 a 170 %, por defecto 130) cuánto se agranda el elegido.

### Partidas y estados

- **Continuar:** al salir de un juego se guarda el punto exacto (`<juego>.auto`, con su foto) y al volver a abrirlo sigues desde ahí. Se desactiva con `continuar=no`. En la pausa, **Reiniciar juego** empieza de cero.
- **Estados rápidos:** 4 ranuras por juego (`.estado`, `.estado2`…), cada una con foto y fecha. Al cargar también aparece la ranura **Al salir**.
- **Genesis PAL:** los juegos solo europeos (región "E" en la cabecera o "(Europe)" en el nombre) se emulan a 50 Hz.
- **Cupo de TheGamesDB:** cada respuesta informa cuántas consultas quedan en el mes; se guarda en `Datos/tgdb_cupo.txt`. La búsqueda automática se detiene cuando quedan 25 (reserva para elegir portadas a mano) y busca primero favoritos y juegos jugados. Las imágenes vienen del CDN y no gastan cupo.

### Nintendo Switch: abrir con R

En Switch conviene abrir SwapShelf **manteniendo R al iniciar un juego** (modo aplicación). Desde el Álbum (modo applet) tiene menos memoria y CPU; SwapShelf lo detecta y lo avisa en la biblioteca.

## Carpetas

```
SwapShelf/
├── SwapShelf.exe
├── SDL2.dll
├── swapshelf.ini       configuración
├── Roms/<consola>/     tus copias de juegos (no se incluyen)
├── Caratulas/          carátulas: <CRC>.bmp (pantalla de título), <CRC>_caja.bmp (caja)
├── Datos/              base No-Intro y metadatos.txt (qué se encontró de cada juego)
├── Controles/          perfiles de controles
└── Partidas/           partidas y respaldos
```

## Compilar

PC (Linux, o MSYS2 en Windows):

```
./scripts/build.sh pc
```

Nintendo Switch (consola MSYS2 de devkitPro):

```
./compilar_switch.sh
```

### Clave de TheGamesDB

Las portadas usan la API de [TheGamesDB](https://thegamesdb.net), que necesita una clave. La clave **no va en este repositorio**:

- Para una versión que vas a distribuir, pásala al compilar: `-DSWS_TGDB_KEY=tu_clave`.
- Cada usuario también puede poner la suya en `swapshelf.ini` (`thegamesdb_clave=`), y esa tiene prioridad.

Sin clave, SwapShelf igual reconoce los nombres y usa la pantalla de título como carátula.

## Licencia

SwapShelf es software libre bajo la **Licencia Pública General de GNU, versión 3** (GPLv3); el texto completo está en [`LICENSE`](LICENSE). Copyright © 2026 Zadark.

El ejecutable incluye núcleos GPLv2+ y uno AGPLv3 (clownmdemu); la sección 13 de la GPLv3 permite combinarlos, y la parte de clownmdemu sigue bajo sus propios términos. Por eso cada versión publicada debe ir acompañada de su código fuente completo. Es un proyecto homebrew gratuito y sin fines comerciales.

## Créditos y licencias de terceros

- Diseño y dirección: **Zadark**. Programado con la ayuda de Claude (Anthropic).
- [mGBA](https://mgba.io) (libmgba), de endrift y colaboradores, licencia MPL 2.0.
- [FCEUmm](https://github.com/libretro/libretro-fceumm), licencia GPLv2 o posterior.
- [Supafaust](https://github.com/libretro/supafaust) (snes_faust de [Mednafen](https://mednafen.github.io)), licencia GPLv2 o posterior.
- [clownmdemu](https://github.com/Clownacy/clownmdemu-libretro), de Clownacy, licencia AGPLv3.
- [SDL2](https://libsdl.org), licencia zlib.
- Tipografía del logo: [Inter](https://rsms.me/inter/) Black, de Rasmus Andersson, licencia SIL Open Font License 1.1. El resto de la interfaz usa DejaVu Sans Bold (licencia libre Bitstream Vera / DejaVu).
- [dr_mp3](https://github.com/mackron/dr_libs), dominio público / MIT-0.
- Bases No-Intro distribuidas por [libretro-database](https://github.com/libretro/libretro-database).
- Datos e imágenes de portadas de [TheGamesDB](https://thegamesdb.net).

Los nombres de consolas se usan solo para describir con qué juegos funciona SwapShelf. SwapShelf no está afiliado ni respaldado por Nintendo, Sega ni ninguna otra empresa.

## Publicar una versión

1. `./compilar_switch.sh` (en la consola MSYS2 de devkitPro).
2. `./scripts/empaquetar.sh` arma en `salida/` el `.zip` para la microSD (`switch/swapshelf/` con el `.nro`, los LEEME en español, inglés y portugués, la licencia y las carpetas de `Roms` vacías) y el `.zip` del código fuente, sin la clave de TheGamesDB, ROMs ni archivos compilados.
