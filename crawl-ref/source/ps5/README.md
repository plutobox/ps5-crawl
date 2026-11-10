# The native PS5 title: how it is built

Dungeon Crawl Stone Soup as a native PS5 title, `PPSA92041`, installed as a
folder in `/data/homebrew/PPSA92041` (see [the port's readme](../../../.github/README.md)
for playing it). The layout follows [Apotris-PS5](https://github.com/darkxex/Apotris-PS5/tree/main/ps5port).

## Build

On Linux or WSL, with `clang-18`, `lld-18`, `llvm-18`, `libclang-rt-18-dev`,
`cmake`, `ninja-build`, `python3`, `perl`, `libpng-dev` and `git`, in a
checkout with LF line endings (`.gitattributes` keeps this folder LF):

```sh
cd crawl-ref/source
python3 ps5/build_ps5.py deps      # once
python3 ps5/build_ps5.py build     # make PS5_NATIVE=y -> crawl (the intermediate PIE)
python3 ps5/build_ps5.py package   # -> ps5/dist/PPSA92041/ and ps5/dist/PPSA92041.zip
python3 ps5/build_ps5.py deploy --ip <console> [--eboot-only]
```

`deploy` uploads the title folder over FTP (port 2121); it refuses while any
game runs on the console and never launches anything.

## How it works

- **Dependencies** (`deps`), all pinned in `build_ps5.py` by SHA-256 or commit,
  into `ps5/.deps`: the payload SDK (v0.42), the PS5 OpenGL SDK (1.0.2),
  PacBrew's libraries (v0.40.2: SDL2_mixer, SDL2_image, freetype, libpng, zlib,
  Lua, SQLite), mihawk-99's platform layer (C library functions a title lacks
  or is refused), SDL2 built from `sdl2/`, the host tools built from
  `tooling/native/`, and `sce_module/libc.prx`, reproduced from its manifests
  and checked against the recorded digest. A piece is redone when its pin (or,
  for SDL2 and the host tools, its sources here) changes.
- **Compiling and linking.** The Makefile's `PS5_NATIVE` build uses
  `tooling/ps5-cc` as its compiler: compiles go to the PS5 Clang wrapper
  (`tooling/prospero-clang18`), the link to `tooling/ps5-link.py`, which adds
  the startup code, the app heap and the C++ runtime, and links only against the
  stubs of the system modules in `tooling/title-modules.txt`. A function that
  only some other module exports (`strcasestr` is in WebKit's) therefore fails
  the link by name instead of crashing the title at address 0; ps5-link.py
  binds it to the platform layer when that has it, and binds the functions a
  title has but may not use (`tooling/platform-functions.txt`). It refuses a
  binding whose `ps5_` version calls the original.
- **Packaging** (`package`): `ps5-native-tool` converts the PIE to the PS5
  layout and signs it as `eboot.bin`; `sce_sys/` comes from the repository root
  (`param.json`, `icon0.png`), the game data from `make install-data`.
- **`app/`**, the port's own code: `ps5_entry.c` wraps the game's `main()` (log
  to `/data/crawl-ps5.log`, the save folder `/download0/crawl`) and every
  `exit()` (a title can't exit: it asks the system to close it);
  `ime.c` opens the system's on-screen keyboard; `native_shims.c` has three C
  library functions. The controller layout and the hint texts are in the game
  (`windowmanager-sdl.cc`, `USE_PS5_NATIVE`).
- **SDL2** (`sdl2/`, from Apotris-PS5): ps5-payload-dev/SDL with a video driver
  that makes any window the full-screen output and shares the single GL
  context, an `SDL_Render` driver on OpenGL, SDL's PS5 audio and joystick
  drivers, and `SDL_GetBasePath()` from `SDL_PS5_BASE_PATH`.

Third-party code here and its licences: [NOTICE.md](NOTICE.md).
