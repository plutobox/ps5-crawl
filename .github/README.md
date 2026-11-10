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
| ✕ Cross | Take the stairs, pick up what's here, or wait a turn | Confirm (Enter); in a list of items, take the highlighted one | Confirm |
| ○ Circle | Cancel (Esc) | Cancel | Cancel |
| □ Square | Cast a spell (`z`) | Tick an entry, in a menu that takes several; type in a text field (name, seed) | |
| △ Triangle | Inventory (`i`) | | |
| L1 | Wait a turn (`s`) when tapped; hold it for the world layer | Previous page | |
| R1 | Fire (`f`) when tapped; hold it for the item layer | Next page | Fire at the target |
| L2 | Explore automatically (`o`) | | Previous target |
| R2 | Fight the nearest monster (`Tab`) when tapped; hold it for the planning layer | | Next target |
| L3 | Examine (`x`) | | |
| R3 | Rest (`5`) | | |
| Create | Abilities (`a`) | | |
| Options | Game menu (`~`): save, quit, help, settings | Cancel | Cancel |
| Touchpad | Level map (`X`) | | |

**Holding R1, R2 or L1** changes the other buttons. R1 is the hands (items),
R2 is the head (knowing and planning), L1 is the world around you. A tap on
its own still fires (R1), attacks (R2) or waits a turn (L1). Holding L1 also
locks the left stick to the four diagonals.

| | Hold R1: items | Hold R2: know and plan | Hold L1: the world |
|---|---|---|---|
| ✕ Cross | Drink | Travel | Close a door |
| □ Square | Read | Memorise a spell | Shout (order allies) |
| △ Triangle | Wield, wear, put on | Skills | Search for items seen |
| ○ Circle | Take off, remove | Previous messages | Open a door |
| D-pad up | Evoke | Character overview | Status effects |
| D-pad down | Drop | Dungeon overview | Known items |
| D-pad left | Swap weapon | Religion | Mutations |
| D-pad right | Choose what to fire | Spell list | Runes |

Everything else is in Options, **All commands**. In the game, Options, Help and manual opens a controller page with all of this.

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
- **A button for every command.** The rest are in Options, All commands.
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

[`.github/workflows/ps5.yml`](workflows/ps5.yml) builds both versions. The
title is built by [`crawl-ref/source/ps5/build_ps5.py`](../crawl-ref/source/ps5/README.md),
which pins everything it builds with; on Linux or WSL:

```sh
cd crawl-ref/source
python3 ps5/build_ps5.py deps      # once: SDKs, libraries, SDL2, host tools
python3 ps5/build_ps5.py build     # make PS5_NATIVE=y
python3 ps5/build_ps5.py package   # -> ps5/dist/PPSA92041{,.zip}
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
