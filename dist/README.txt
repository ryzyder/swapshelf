SWAPSHELF (SWS) 0.6
===================
Homebrew cartridge launcher for Nintendo Switch and PC: SNES, Genesis / Mega Drive,
GBA, GBC, GB and NES. Free and non-commercial.

SwapShelf does NOT include or download games. Only use backups of cartridges you own.

INSTALL ON SWITCH
1. Copy the "swapshelf" folder to  sdmc:/switch/  (you get sdmc:/switch/swapshelf/).
2. Put your games in  sdmc:/switch/swapshelf/Roms/<console>/
   (snes, genesis, gba, gbc, gb, nes).
3. Open SwapShelf from the Homebrew menu holding R while starting a game
   (application mode: full performance). From the Album it runs with less memory
   and the library warns you.
   The first time it checks every game (it can take a while with many); after that
   it opens quickly.

BUTTONS
                      Switch            PC
   Play               A                 Enter
   Console            L / R             Q / E
   Game menu          Y                 M     (cover, favorite, controls,
                                               information, update, delete)
   Favorite           X                 F
   Search             -                 Ctrl+F  or  /
   Sort               +                 O
   Letter             ZL / ZR           PgUp / PgDn
   Options            (last tile of the consoles row)   F2
   Pause in game      + and -           Esc

IN GAME (pause)
   Save / load state in 4 slots with a picture, restart, Zadark HD filter, controls
   per console or per game. On exit the exact point is saved and next time the game
   continues where you left off (can be turned off in Options).

OPTIONS (saved automatically to swapshelf.ini)
   Dark or light theme, menu color, visible consoles, cartridges per row, zoom on the
   selected one, cartridge image, resume where I left off, game size and sides,
   volumes, swap A and B, turbo, language (Español, English, Português) and About.

FOLDERS CREATED AUTOMATICALLY
   Partidas             saves, states (.estado, .auto) and their pictures.
   Caratulas            covers; you can add your own image <CRC>_caja.bmp.
   Datos                official names (No-Intro), jugados.txt (times played, time
                        and favorites) and the TheGamesDB quota.
   Controles, biblioteca.txt and registro.txt.

COVERS
   Covers come from TheGamesDB, with a query quota per key. If it runs out, SwapShelf
   waits and continues later on its own; favorites and played games go first.

WHAT'S NEW IN 0.6
   - Library for large collections: Home with the last 10 played, a grid per console
     with sorting and jump by letter, search with an on-screen keyboard and a menu
     for each game (cover, favorite, controls, information, delete).
   - New cartridges with each console's color band; the selected one grows and glows
     with the accent color.
   - Resume where you left off, 4 state slots with picture and date, restart game.
   - Time played per game.
   - In-app options, light and dark themes, five accent colors, visible consoles and
     three languages.
   - European Genesis games run at 50 Hz automatically.
   - Covers: preview when choosing by hand, only from the right console, care for the
     TheGamesDB quota and background loading without stuttering while scrolling.
   - Faster startup (the library loads during the intro), auto-repeat when holding the
     d-pad and no green frame around the game.

LICENSE
   GNU GPL version 3 (LICENSE.txt). Source code comes with every release.
   Cores: mGBA (MPL 2.0), FCEUmm (GPLv2+), Supafaust (GPLv2+), clownmdemu (AGPLv3).
   Data from No-Intro and TheGamesDB.net. Made with SDL2 and libnx / devkitPro.
   SwapShelf is not affiliated with or endorsed by Nintendo, Sega or any other company.
