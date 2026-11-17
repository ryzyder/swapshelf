# Zadark HD

A "remastered look" filter for 2D games (made for Game Boy Advance, works on DS and other consoles).
Created by **Zadark**.

What it does, in order:

1. **Edges** – xBR-lv3 (Hyllian) upscaling x4, blended with the original pixel so curves stay smooth
   without looking over-rounded (levels 1-4 = 35/40/45/50 %).
2. **Vivid** – remastered color: vibrance that boosts dull colors more than strong ones, a soft
   S-curve contrast and a slight warm tint (levels 1-4: subtle, natural, vivid, intense).
3. **Sharpness** – AMD CAS adaptive sharpening (0-100).
4. **Glow** – a soft bloom on the brightest areas only (0-100).
5. **Texture** – optional fine diagonal RGB screen texture at screen resolution, with light
   compensation so colors don't get darker (1 = none ... 4 = strong).

Defaults: Edges 2, Vivid 3, Sharpness 60, Glow 22, Texture 1.

## Install

**mGBA** – copy `mgba/zadark-hd.shader` into mGBA's `shaders` folder, then open the shader settings
in mGBA, choose *Load new shader* and select the `zadark-hd.shader` folder. The levels appear as
shader parameters.

**RetroArch** (any core: mGBA, gpSP, DeSmuME, melonDS, SNES, ...) – copy the contents of
`retroarch/` into RetroArch's `shaders` folder (for example `shaders/shaders_slang/zadark-hd/`).
In a game: *Quick Menu > Shaders > Load* → `zadark-hd.slangp`. Adjust levels in *Shader Parameters*.
Needs a video driver with slang support (Vulkan, glcore, D3D11/12 or Metal).

## License and credits

MIT License (see `LICENSE.txt`). You can use it, modify it and include it in your emulator or app,
including commercial projects, as long as you keep the copyright notice:
**"Zadark HD by Zadark"**, plus the xBR (Hyllian) and CAS (AMD) notices.

`tools/gen_shaders.py` generates both versions from the same source, so mGBA and RetroArch give
identical results.
