# Dungeon Crawl Stone Soup for PS5

A port of [Dungeon Crawl Stone Soup](https://crawl.develz.org/) to jailbroken
PS5 consoles. It runs as a native title (`PPSA92041`) with the tiles
graphics, and is played with the DualSense.

This is the `ps5` branch of a Crawl fork. The upstream Crawl readme is
[`README.md`](../README.md) at the root. The PS5 code lives in
[`crawl-ref/source/ps5/`](../crawl-ref/source/ps5) and behind
`USE_PS5_NATIVE` in the game's source.

## Install

- **From the homebrew catalog:** install Dungeon Crawl Stone Soup from a
  store that reads the PS5 homebrew catalog, such as ProsperoStore.
- **By hand:** download `PPSA92041.zip` from [Releases](../../releases),
  unzip it, and copy the `PPSA92041` folder to `/data/homebrew/` over FTP.
  ShadowMountPlus then installs it on the home screen.

Saves are kept in the title's own storage (`/download0/crawl`).

## Controls

Each button sends a key Crawl already understands, so every menu, prompt
and targeting mode works. What a button does depends on what the game is
waiting for.

| Button | In the dungeon | In menus and prompts | When targeting |
|---|---|---|---|
| D-pad / left stick | Move in 8 directions (hold to repeat) | Move the cursor | Move the cursor |
| ✕ Cross | Take the stairs, or pick up what's here | Confirm (Enter) | Confirm |
| ○ Circle | Cancel (Esc) | Cancel | Cancel |
| □ Square | Cast a spell (`z`) | Type in the text field (name, seed) | |
| △ Triangle | Inventory (`i`) | | |
| L1 | Wait a turn (`s`) | Previous page | |
| R1 | Fire (`f`) | Next page | Fire at the target |
| L2 | Explore automatically (`o`) | | |
| R2 | Fight the nearest monster (`Tab`) | | |
| L3 | Examine (`x`) | | |
| R3 | Rest (`5`) | | |
| Create | Abilities (`a`) | | |
| Options | Game menu (`~`): save, quit, help, settings | Cancel | Cancel |
| Touchpad | Level map (`X`) | | |

**Typing text** uses the PS5's own on-screen keyboard:
- Questions that ask you to type, such as inscribing an item, searching
  or adding a note, open it by themselves, titled with the question.
- On the main menu and the new-game screens, press □ Square to type your
  name or a seed into the field.
- Close the keyboard with ○ Circle to cancel the question.

When you start a new game, you can also leave the name empty and confirm:
Crawl picks a random name for you.

## What's missing

- **Typing a repeat count.** The prompt that reads a number and then a
  command still expects keys, so it can only be cancelled.
- **Commands without a button.** Only the commands in the table have a
  button. The rest (such as quaff, read, wear and evoke) are reached through
  the inventory, so item actions work but there's no shortcut for each.
- **A hint bar.** The screen doesn't show what each button does.
- **Your PSN name.** The game doesn't read the console's user name.
- **A closing screen.** A PS5 title may not exit by itself. After you save
  or quit, the screen stops updating; press the PS button and close the game
  from the home screen.
- **Sound.** The title is built without Crawl's sound support.
- **Online play.** WebTiles and the online servers aren't available; this is
  the offline game.
- **The ELF payload build** (`PS5=y`, for loaders such as elfldr) is built
  by CI and kept as a workflow artifact, but it isn't released.

## Build and release

[`.github/workflows/ps5.yml`](workflows/ps5.yml) builds both versions with
the pinned toolchain: ps5-native-app-boilerplate, ps5-opengl with its SDK
and SDL2, PacBrew's PS5 libraries and mihawk-99's platform layer. To build
the title by hand after setting those up in `$HOME`:

```sh
cd crawl-ref/source
make -j"$(nproc)" PS5_NATIVE=y crawl-native.objs
bash ps5/package-native.sh --build   # -> build-ps5/app/dist/PPSA92041{,.zip}
```

**Releasing.** The homebrew catalog tells consoles an update exists by
comparing `contentVersion` in [`sce_sys/param.json`](../sce_sys/param.json)
(`NN.NNN.NNN`), so every release raises it and is tagged with it. Run the
**PS5 Build and Release** workflow by hand on `ps5` with `release` set to
`patch`, `minor` or `major`:

```sh
gh workflow run ps5.yml --repo plutobox/ps5-crawl --ref ps5 -f release=patch
```

It raises the version, builds, commits the bump, tags it and publishes the
release with `PPSA92041.zip` as its only asset. The catalog's update job
then picks the release up. `crawl-ref/source/ps5/content-version.py` holds
the rules.

**Debugging.** The title writes its log to `/data/crawl-ps5.log`, which you
can read over FTP.

## License

Crawl is distributed under the [GNU GPL, version 2 or later](../LICENSE).
