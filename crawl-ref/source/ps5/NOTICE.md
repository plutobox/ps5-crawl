# Third-party code in `ps5/`

The port is distributed under the game's licence (GPL v2 or later; see
`crawl-ref/LICENSE`). Combined with the GPL-3.0-or-later pieces below, the
title is distributed under GPL-3.0-or-later. It builds on, or carries code from:

| Piece | Where | Licence |
| --- | --- | --- |
| Native app tooling: startup code, linker layout, converter, FSELF signer, clean-room runtime emitter and its manifests (ps5-native-app-boilerplate 4f531c4, as carried by Apotris-PS5; portions derived from SharpProspero) | `tooling/`, `tooling/native/` | GPL-3.0-or-later, BlackBearReloaded; SharpProspero GPL-3.0, SvenGDK |
| App heap and libc++ runtime shims (PS5 OpenGL SDK's `native-app/`) | `tooling/native/app_heap.c`, `cts_runtime_shims.c`, `ps5-pie.ld`, `app-symbols.map` | GPL-3.0-or-later, BlackBearReloaded |
| `ps5-cc`, the SDL2 build, video and render drivers (Apotris-PS5's `ps5port/`, derived from the PS5 OpenGL SDK's SDL2 integration) | `tooling/ps5-cc`, `sdl2/` | GPL-3.0-or-later; SDL itself zlib |
| PS5 OpenGL SDK, payload SDK, PacBrew libraries (libsodium's SHA-256 among them, ISC), platform layer, SDL2 sources | `.deps/` (downloaded, not tracked) | their own licences |
| Sound effects (synthesized by DCSS Remastered Audio's `make_sfx.py`, and three Freesound recordings) | `sound/sfx/` | CC0 |
| Music, Kevin MacLeod (incompetech.com) | downloaded by the title from its menu (or by `package --with-music` into `.deps/music/`, packaged in `assets/sound/music/`) | CC BY 4.0; credits in `sound/CREDITS.txt` |
