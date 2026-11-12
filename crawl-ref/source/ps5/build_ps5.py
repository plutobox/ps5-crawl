#!/usr/bin/env python3
"""Build Dungeon Crawl Stone Soup as a native PS5 title.

  deps      fetch and build everything the title is built with, pinned, into
            ps5/.deps: the payload SDK, the OpenGL SDK, PacBrew's libraries,
            the platform layer, SDL2 (ps5/sdl2), the host tools, the
            title runtime (sce_module/libc.prx)
  build     make PS5_NATIVE=y: the game, linked into the intermediate PIE
            (--debug-input: a debug build, driven over FTP: it types
            /data/crawl-input.txt and shows its screen in
            /data/crawl-screen.bmp, see windowmanager-sdl.cc)
  package   eboot.bin and the title folder ps5/dist/<TITLEID>, and its zip
            (the title downloads the music from its menu; --with-music:
            carry it, fetched pinned into ps5/.deps/music)
  deploy    upload ps5/dist/<TITLEID> to a console over FTP (never launches;
            --stop: close this title first if it is running). Only files
            changed since the last deploy go (--all: every file): a map file
            uploaded again makes the title rebuild its map cache, minutes
  launch    start the title on the console, through elfldr
  keys      type keys into a --debug-input build ("\\n" Enter, ^X Cross,
            ^[ Circle...: windowmanager-sdl.cc) and fetch its screen as
            ps5/build/crawl-screen.png

Run it from Linux or WSL, in a checkout with LF line endings. `deps` is
needed once (each piece is redone only when its pin changes).
"""
import argparse
import ftplib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import time
import urllib.parse
import urllib.request
import zipfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent                     # crawl-ref/source
ROOT = SOURCE.parent.parent              # the repository
DEPS = Path(os.environ.get('PS5_DEPS', HERE / '.deps'))
BUILD = HERE / 'build'
DIST = HERE / 'dist'
TOOLING = HERE / 'tooling'
NATIVE = TOOLING / 'native'

SDK = DEPS / 'ps5-payload-sdk'
GL = DEPS / 'ps5-opengl-sdk'
PACBREW = DEPS / 'pacbrew'
PLATFORM = DEPS / 'ps5-platform'
SDL = DEPS / 'sdl2'
HOST = DEPS / 'host'
RUNTIME = DEPS / 'runtime'
MUSIC = DEPS / 'music'
SOUND = HERE / 'sound'

# Pins: what the title is built with. Change one, and `deps` redoes that piece.
PAYLOAD_SDK = ('https://github.com/ps5-payload-dev/sdk/releases/download/v0.42/ps5-payload-sdk.zip',
               '8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da')
OPENGL_SDK = ('https://github.com/darkxex/ps5-opengl/releases/download/v1.0.2/ps5-opengl-sdk-1.0.2.tar.gz',
              '30d8e51dd062739b629346102be5932728ba714bc50b1bdcdf08306c8843d9a4')
PACBREW_RELEASE = ('https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.40.2/ps5-payload-dev.tar.gz',
                   'a85f65de418a8e6a898c6c3e3c870d50fff7618a200e4dd59ea9692af6ecec4d')
ZLIB = ('https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz',
        'bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16')
PLATFORM_REPO = ('https://github.com/mihawk-99/PS5_PayloadSDK', 'e105cf6f25801d9e9981812a89a3b74021d2d2a2')
SDL_REPO = ('https://github.com/ps5-payload-dev/SDL', '8c56053f13ca13a0c050de613706ff69eb615836')
# The title runtime libc_builder.cpp reproduces (ps5-native-app-boilerplate 4f531c4).
LIBC_RAW_SHA256 = '8ee6e124993e1af26420cb455890fd002f5d6c7e78883c860ce45734e7d002bb'
LIBC_PRX_SHA256 = 'e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036'
# The sound pack's music (sound/init.txt): Kevin MacLeod's tracks from
# incompetech.com, CC BY 4.0 (sound/CREDITS.txt). Track file: (title, sha256,
# size). The title downloads them from its menu; package --with-music carries
# them instead.
MUSIC_BASE = 'https://incompetech.com/music/royalty-free/mp3-royaltyfree/'
MUSIC_TRACKS = {
    'menu': ('The Path of the Goblin King', 'f8a30fac2528bb0a630cd2d7500daf10b64860ec7e91823390cc6e1cb545175a', 8258790),
    'dungeon': ('Long Note Two', '53fd96b8fdcae5d2a9bb35a342cfb67344994952f76ff60ff94f56644262f93b', 18641758),
    'temple': ('Meditation Impromptu 01', 'ac1332a12fe020cbeee206c7f33a82e29109d5ce63636e3302454e3bdfc36187', 7042622),
    'lair': ('Chee Zee Cave', '209b4b8ed80f7e01e5bcace96f887bbe6973ea8053809fd57dc466fc9acca85a', 9273499),
    'swamp': ('Mesmerize', 'fd6a372b816bb9117cbd81167f258e71add16fec072b711aa7bdb89aa9acc277', 13572535),
    'shoals': ('Windswept', '0d7445201046bf536e9982c56a8b068d3c4bda18d01817edc6610b4a6a46e706', 8335980),
    'snake': ('Desert City', 'aeefb582c77059aaf5b10d1e9125d7b19adca07cd841f3654e74aed7da9a3a71', 3679550),
    'spider': ('Phantom from Space', '351ef091fddb6e96a7c7b0f707ae508d4177c4fa4b63fd0dedecd2b9288ec268', 5006916),
    'slime': ('Controlled Chaos', '2272606eca85243b723eeba82bb2b2c078f0c753c931103f768c233bb34987f0', 14092660),
    'orc': ('Crusade', '863758365f6a03cd0cf85237d7d49448d5cb3c1af2e0858dc7c786bc9c0b981e', 7951802),
    'elf': ('Angevin', '425f5febc62aee69761f16eae09158c35fd922a72c0fe8165af51c3a1f8818bf', 11224420),
    'vaults': ('Master of the Feast', '7d8c1e5ebfd4c842718fa7c6bd81e8804cd0e743fe1496936f526dd514e65409', 7306499),
    'crypt': ('Ghost Processional', '9502c6f9a0c365f2717ea82acd7e18c17f784da742c63a1e6887c5761a7ef421', 3440840),
    'tomb': ('Curse of the Scarab', 'e1c825715a8e0e58eb7bf308438df68bcd2b9caf5df84d397e0466bcba89e193', 10473824),
    'depths': ('The Descent', 'f097d51700b970efffc62499473ea3a31e09aad4172e787978bc9c2a644d53a2', 7672813),
    'zot': ('Lightless Dawn', '59fd69e4432c95887d626e8055113da8fb0da9a8ccac85c6b7da6a6031c4f325', 15197957),
    'hell': ('Anguish', 'b1750903a90c2cfe88445196e78181a9093c0a3bc08f8bed39993d969fe0d268', 7653395),
    'coc': ('Frost Waltz', '560f932189551c8a009a671da25d8b51cf2067df85fcac35731ea6ac0c7d6cd3', 5526812),
    'geh': ('Volatile Reaction', 'ced1148df201a6c69062a96b7ef49c49924572ca5023c2ac5a2d6c5828ecb218', 5293585),
    'tar': ('Dark Fog', 'a357b85ec02c52c17e4c677ca19037d742985045993db8f2226c7eab646ae86e', 9564075),
    'dis': ('Killers', '5a47315f15cae3148df295aef80c7ec64bf124f692b5464a835353bd4ec82d23', 9769089),
    'abyss': ('Unseen Horrors', 'c80c5136705b17b557e55bbb8f2fcdc49c4107bda419e5bbb1742ef313e6efb2', 8036245),
    'pan': ('Nightmare Machine', '0b4a6a81f333d81dd8b04ff2cb182a88685aec2878d4cbfc767f1dd1562e19fb', 7248839),
}

# Loader and container constants of the native app format, validated on
# firmware 6.02 and 12.70 (ps5-native-app-boilerplate tools/build.sh).
MODULE_SDK = '0x02000009'
COMPANION_SDK = '0x08050001'
FSELF_MAGIC = '0x1D3D154F'

CLANG = os.environ.get('PS5_CLANG', 'clang-18')
CLANGXX = os.environ.get('HOST_CXX', 'clang++-18')


def run(*cmd, **kw):
    cmd = [str(c) for c in cmd]
    print('+', ' '.join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def toolchain_env():
    return dict(os.environ, PS5_PAYLOAD_SDK=str(SDK), PS5_CLANG=shutil.which(CLANG) or CLANG,
                USE_CCACHE='0')


# -- deps ---------------------------------------------------------------------

def stamp_ok(directory, pin):
    return (directory / '.pin').is_file() and (directory / '.pin').read_text().strip() == pin


def stamp(directory, pin):
    (directory / '.pin').write_text(pin + '\n')


def download(url, digest):
    """A pinned download, cached in .deps/downloads and checked every time."""
    cache = DEPS / 'downloads'
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / url.rsplit('/', 1)[1]
    if not path.is_file() or sha256(path) != digest:
        print(f'downloading {url}', flush=True)
        partial = path.with_suffix(path.suffix + '.part')
        with urllib.request.urlopen(url) as response, open(partial, 'wb') as out:
            shutil.copyfileobj(response, out, 1 << 20)
        got = sha256(partial)
        if got != digest:
            partial.unlink()
            sys.exit(f'{url}: sha256 {got}, expected {digest}')
        partial.replace(path)
    return path


def fetch_commit(url, commit, target):
    """A pinned commit, fetched by its SHA (it need not be on a branch)."""
    if not (target / '.git').is_dir():
        target.mkdir(parents=True, exist_ok=True)
        run('git', 'init', '-q', target)
    have = subprocess.run(['git', '-C', target, 'rev-parse', 'HEAD'], capture_output=True, text=True)
    if have.stdout.strip() != commit:
        run('git', '-C', target, 'fetch', '-q', '--depth', '1', url, commit)
        run('git', '-C', target, 'checkout', '-q', '--force', commit)


def fresh(directory):
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)


def deps_payload_sdk():
    url, digest = PAYLOAD_SDK
    if stamp_ok(SDK, digest):
        return
    archive = download(url, digest)
    shutil.rmtree(SDK, ignore_errors=True)
    fresh(DEPS / 'sdk-extract')
    run('unzip', '-q', archive, '-d', DEPS / 'sdk-extract')  # keeps the tools' executable bits
    (DEPS / 'sdk-extract' / 'ps5-payload-sdk').rename(SDK)
    shutil.rmtree(DEPS / 'sdk-extract')
    stamp(SDK, digest)


def deps_opengl_sdk():
    url, digest = OPENGL_SDK
    if stamp_ok(GL, digest):
        return
    archive = download(url, digest)
    fresh(DEPS / 'gl-extract')
    with tarfile.open(archive) as tar:
        tar.extractall(DEPS / 'gl-extract', filter='data')
    shutil.rmtree(GL, ignore_errors=True)
    (DEPS / 'gl-extract' / 'ps5-opengl-sdk-1.0.2' / 'sdk').rename(GL)
    shutil.rmtree(DEPS / 'gl-extract')
    stamp(GL, digest)


def deps_pacbrew():
    url, digest = PACBREW_RELEASE
    if stamp_ok(PACBREW, digest):
        return
    archive = download(url, digest)
    fresh(PACBREW)
    # only the ported libraries (user/homebrew), not the SDK it also carries
    run('tar', '-xzf', archive, '-C', PACBREW, '--strip-components=3',
        'opt/ps5-payload-sdk/target/user/homebrew')
    if not (PACBREW / 'user' / 'homebrew' / 'lib' / 'libSDL2_mixer.a').is_file():
        sys.exit('PacBrew release has an unexpected layout')
    stamp(PACBREW, digest)


def deps_platform():
    url, commit = PLATFORM_REPO
    if stamp_ok(PLATFORM, commit):
        return
    source = DEPS / 'src' / 'PS5_PayloadSDK'
    fetch_commit(url, commit, source)
    run('make', '-C', source / 'platform', f'-j{os.cpu_count()}')
    fresh(PLATFORM)
    (PLATFORM / 'lib').mkdir()
    shutil.copy2(source / 'platform' / 'build' / 'ps5' / 'libps5platform.a', PLATFORM / 'lib')
    stamp(PLATFORM, commit)


def patch_sdl(snapshot):
    """Static build and joystick fixes (sdl2/*.patch), the base path, and the PS5 render driver."""
    run('git', 'init', '-q', snapshot)
    for name in ('static-ps5.patch', 'ps5-joystick.patch'):
        run('git', 'apply', HERE / 'sdl2' / name, cwd=snapshot)

    def replace_once(path, anchor, replacement):
        text = path.read_text()
        if text.count(anchor) != 1:
            sys.exit(f'{path.name} changed: the anchor {anchor!r} was not found once')
        path.write_text(text.replace(anchor, replacement))

    # A title can't chdir and argv[0] has no folder: SDL_GetBasePath() is /app0/.
    filesystem = snapshot / 'src' / 'filesystem' / 'ps5' / 'SDL_sysfilesystem.c'
    replace_once(filesystem, '    const char** argv = getargv();\n    char dirname[PATH_MAX];\n',
                 '    const char** argv = getargv();\n    char dirname[PATH_MAX];\n'
                 '    const char *base = getenv("SDL_PS5_BASE_PATH");\n\n'
                 '    if (base && *base) {\n        return strdup(base);\n    }\n')
    replace_once(filesystem, '#include <string.h>\n', '#include <string.h>\n#include <stdlib.h>\n')
    replace_once(snapshot / 'src' / 'render' / 'SDL_render.c', '#if SDL_VIDEO_RENDER_SW\n    &SW_RenderDriver\n',
                 '#if SDL_VIDEO_RENDER_PS5GL\n    &PS5GL_RenderDriver,\n#endif\n'
                 '#if SDL_VIDEO_RENDER_SW\n    &SW_RenderDriver\n')
    replace_once(snapshot / 'src' / 'render' / 'SDL_sysrender.h', 'extern SDL_RenderDriver SW_RenderDriver;',
                 'extern SDL_RenderDriver SW_RenderDriver;\nextern SDL_RenderDriver PS5GL_RenderDriver;')


def deps_sdl():
    """SDL2 (static) on the OpenGL SDK, built from ps5/sdl2; its pin includes ps5/sdl2's files."""
    url, commit = SDL_REPO
    inputs = hashlib.sha256()
    for path in sorted((HERE / 'sdl2').iterdir()):
        inputs.update(path.name.encode() + b'\0' + path.read_bytes())
    pin = f'{commit} {inputs.hexdigest()} {OPENGL_SDK[1]}'
    if stamp_ok(SDL, pin):
        return
    source = DEPS / 'src' / 'SDL'
    fetch_commit(url, commit, source)
    work = DEPS / 'build' / 'sdl2'
    fresh(work)
    snapshot = work / 'SDL'
    snapshot.mkdir()
    archive = work / 'sdl.tar'
    with archive.open('wb') as stream:
        run('git', '-C', source, 'archive', commit, stdout=stream)
    with tarfile.open(archive) as tar:
        tar.extractall(snapshot, filter='data')
    patch_sdl(snapshot)
    wrapper = TOOLING / 'prospero-clang18'
    wrapper.chmod(0o755)  # cmake runs it directly
    fresh(SDL)
    env = toolchain_env()
    run('cmake', '-S', HERE / 'sdl2', '-B', work / 'cmake', '-G', 'Ninja', f'-DSDL_SOURCE={snapshot}',
        f'-DPS5_OPENGL_PREFIX={GL}', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_SYSTEM_NAME=Generic',
        '-DCMAKE_SYSTEM_PROCESSOR=x86_64', '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
        f'-DCMAKE_C_COMPILER={wrapper}', f'-DCMAKE_CXX_COMPILER={wrapper}',
        f'-DCMAKE_INSTALL_PREFIX={SDL}', '-DCMAKE_INSTALL_LIBDIR=lib',
        '-DCMAKE_C_FLAGS=-D__PROSPERO__ -fPIC -ffunction-sections -fdata-sections', env=env)
    run('cmake', '--build', work / 'cmake', '--parallel', env=env)
    run('cmake', '--install', work / 'cmake', env=env)
    stamp(SDL, pin)


def deps_host_tools():
    """ps5-native-tool (converter and FSELF signer) and libc-builder, from ps5/tooling/native."""
    sources = ['native_app_builder.cpp', 'self_container.cpp', 'elf_object.cpp', 'sce_module_writer.cpp']
    inputs = hashlib.sha256()
    for path in sorted(NATIVE.glob('*.[ch]pp')):
        inputs.update(path.name.encode() + b'\0' + path.read_bytes())
    pin = f'{inputs.hexdigest()} {ZLIB[1]}'
    if stamp_ok(HOST, pin):
        return
    url, digest = ZLIB
    archive = download(url, digest)
    work = DEPS / 'build' / 'zlib'
    fresh(work)
    with tarfile.open(archive) as tar:
        tar.extractall(work, filter='data')
    zsrc = work / 'zlib-1.3.2'
    zenv = dict(os.environ, CC=CLANG, AR='llvm-ar-18', RANLIB='llvm-ranlib-18')
    run('./configure', '--static', '--prefix=/usr', cwd=zsrc, env=zenv, stdout=subprocess.DEVNULL)
    run('make', f'-j{os.cpu_count()}', cwd=zsrc, env=zenv, stdout=subprocess.DEVNULL)
    fresh(HOST)
    flags = ['-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', f'-I{zsrc}']
    run(CLANGXX, *flags, *[NATIVE / s for s in sources], zsrc / 'libz.a', '-o', HOST / 'ps5-native-tool')
    run(CLANGXX, *flags, NATIVE / 'libc_builder.cpp', '-o', HOST / 'libc-builder')
    stamp(HOST, pin)


def deps_runtime():
    """sce_module/libc.prx, reproduced from its manifests and checked against the recorded digests."""
    if stamp_ok(RUNTIME, LIBC_PRX_SHA256) and (RUNTIME / 'libc.prx').is_file() \
            and sha256(RUNTIME / 'libc.prx') == LIBC_PRX_SHA256:
        return
    fresh(RUNTIME)
    raw = RUNTIME / 'libc.raw.elf'
    run(HOST / 'libc-builder', NATIVE / 'runtime' / 'api-surface.txt', NATIVE / 'runtime' / 'imports.txt', raw)
    if sha256(raw) != LIBC_RAW_SHA256:
        sys.exit(f'libc.prx: the raw runtime is {sha256(raw)}, expected {LIBC_RAW_SHA256}')
    run(HOST / 'ps5-native-tool', 'self', '--sign', '--in', raw, '--out', RUNTIME / 'libc.prx')
    if sha256(RUNTIME / 'libc.prx') != LIBC_PRX_SHA256:
        sys.exit(f'libc.prx is {sha256(RUNTIME / "libc.prx")}, expected {LIBC_PRX_SHA256}')
    raw.unlink()
    stamp(RUNTIME, LIBC_PRX_SHA256)


def music_pin():
    return hashlib.sha256(json.dumps(MUSIC_TRACKS, sort_keys=True).encode()).hexdigest()


def music_url(title):
    return MUSIC_BASE + urllib.parse.quote(title) + '.mp3'


def deps_music():
    pin = music_pin()
    if stamp_ok(MUSIC, pin):
        return
    fresh(MUSIC)
    for name, (title, digest, _) in MUSIC_TRACKS.items():
        shutil.copy2(download(music_url(title), digest), MUSIC / f'{name}.mp3')
    stamp(MUSIC, pin)


def cmd_deps(args):
    DEPS.mkdir(parents=True, exist_ok=True)
    for step in (deps_payload_sdk, deps_opengl_sdk, deps_pacbrew, deps_platform, deps_sdl,
                 deps_host_tools, deps_runtime):
        print(f'== {step.__name__[5:]}', flush=True)
        step()
    print(f'dependencies ready in {DEPS}')


# -- build and package ----------------------------------------------------------

def make(*targets, **variables):
    run('make', f'-j{os.cpu_count()}', 'PS5_NATIVE=y', f'PS5_DEPS={DEPS}',
        *[f'{k}={v}' for k, v in variables.items()], *targets, cwd=SOURCE)


def cmd_build(args):
    make(**({'EXTERNAL_FLAGS': '-DPS5_DEBUG_INPUT'} if args.debug_input else {}))


def read_param():
    path = ROOT / 'sce_sys' / 'param.json'
    param = json.loads(path.read_text())
    title_id = param.get('titleId', '')
    checks = [
        (re.fullmatch(r'PPSA\d{5}', title_id), 'titleId must be PPSA and five digits'),
        (param.get('conceptId') == title_id[4:], 'conceptId must be the digits of titleId'),
        (re.fullmatch(rf'[A-Z]{{2}}\d{{4}}-{title_id}_00-[A-Z0-9]{{16}}', param.get('contentId', '')),
         'contentId must be XX9999-<titleId>_00-<16 characters>'),
        (re.fullmatch(r'\d{2}\.\d{3}\.\d{3}', param.get('contentVersion', '')),
         'contentVersion must be NN.NNN.NNN'),
        (isinstance(param.get('downloadDataSize'), int) and param['downloadDataSize'] > 0,
         'downloadDataSize must be positive: /download0 holds the saves'),
    ]
    for ok, message in checks:
        if not ok:
            sys.exit(f'sce_sys/param.json: {message}')
    return param


def cmd_package(args):
    param = read_param()
    title_id = param['titleId']
    pie = SOURCE / 'crawl'
    if not pie.is_file():
        sys.exit('no crawl to package: run "build" first')
    tool = HOST / 'ps5-native-tool'
    BUILD.mkdir(exist_ok=True)
    eboot_elf = BUILD / 'eboot.elf'
    # The converter looks for every imported module's stub in one folder.
    stubs = BUILD / 'stubs'
    fresh(stubs)
    for stub in [*(SDK / 'target' / 'lib').glob('*.so'), *(GL / 'lib').glob('*.so')]:
        shutil.copy2(stub, stubs / stub.name)
    run(tool, 'link', '--in', pie, '--out', eboot_elf, '--stub-dir', stubs,
        '--module-sdk', MODULE_SDK, '--companion-sdk', COMPANION_SDK, '--file-name', 'eboot.elf')

    app = DIST / title_id
    fresh(app)
    (app / 'sce_sys').mkdir()
    (app / 'sce_module').mkdir()
    run(tool, 'self', '--sign', '--in', eboot_elf, '--out', app / 'eboot.bin', '--magic', FSELF_MAGIC)
    run(tool, 'self', '--inspect', '--file', app / 'eboot.bin')
    shutil.copy2(RUNTIME / 'libc.prx', app / 'sce_module' / 'libc.prx')
    if sha256(app / 'sce_module' / 'libc.prx') != LIBC_PRX_SHA256:
        sys.exit('libc.prx does not have the recorded digest: run "deps"')
    for name in ('param.json', 'icon0.png', 'pic0.dds', 'pic1.dds', 'snd0.at9'):
        if (ROOT / 'sce_sys' / name).is_file():
            shutil.copy2(ROOT / 'sce_sys' / name, app / 'sce_sys' / name)

    # Game data: read-only in the title, at /app0/assets.
    stage = BUILD / 'data-stage'
    shutil.rmtree(stage, ignore_errors=True)
    make('install-data', DESTDIR=stage)
    shutil.copytree(stage / 'app0' / 'assets', app / 'assets')
    # The title's sound pack, at /app0/assets/sound, and the list of the
    # music the startup menu downloads what's missing from (startup.cc).
    sound = app / 'assets' / 'sound'
    shutil.copytree(SOUND, sound)
    if args.with_music:
        deps_music()
        shutil.copytree(MUSIC, sound / 'music', ignore=shutil.ignore_patterns('.pin'))
    (sound / 'music-download.txt').write_text(''.join(
        f'music/{name}.mp3 {digest} {size} {music_url(title)}\n'
        for name, (title, digest, size) in MUSIC_TRACKS.items()))

    archive = DIST / f'{title_id}.zip'
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(app.rglob('*')):
            if path.is_file():
                # audio is compressed already
                stored = path.suffix in ('.mp3', '.ogg')
                zf.write(path, path.relative_to(DIST),
                         compress_type=zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED)
    print(f'title folder: {app}\nzip: {archive}')


# -- deploy -------------------------------------------------------------------

def running_title(ftp):
    """The title ShadowMountPlus last saw start without exiting, or None."""
    lines = []
    try:
        ftp.retrlines('RETR /data/shadowmount/debug.log', lines.append)
    except ftplib.error_perm:
        return None
    last = ''
    for line in lines:
        if '[GAME] started:' in line or '[GAME] process exited' in line:
            last = line.rstrip('\r')
    if '[GAME] started:' in last:
        return last.split('[GAME] started:', 1)[1].split()[0]
    return None


def run_payload(ip, port, name, title_id, *libs):
    """Builds tooling/<name>.c for title_id and runs it on the console through
    elfldr; its output, which must end in the line <NAME>_OK."""
    if not re.fullmatch(r'[A-Z]{4}\d{5}', title_id):
        sys.exit(f'bad title ID {title_id}')
    payload = BUILD / f'{name}.elf'
    BUILD.mkdir(exist_ok=True)
    run(SDK / 'bin' / 'prospero-clang', TOOLING / f'{name}.c', f'-DTITLE_ID="{title_id}"',
        *libs, '-o', payload, env=toolchain_env())
    with socket.create_connection((ip, port), timeout=30) as connection:
        connection.sendall(payload.read_bytes())
        connection.shutdown(socket.SHUT_WR)
        output = bytearray()
        while chunk := connection.recv(4096):
            output.extend(chunk)
    message = output.decode(errors='replace')
    print(message, end='', flush=True)
    marker = name.split('-')[0].upper() + '_OK'
    if marker not in message:
        sys.exit(f'{name} failed for {title_id}')


def stop_title(ip, port, title_id):
    """Closes title_id on the console (tooling/stop-title.c)."""
    run_payload(ip, port, 'stop-title', title_id, '-lSceSystemService')


def cmd_launch(args):
    """Starts the title on the console (tooling/launch-title.c)."""
    run_payload(args.ip, args.elf_port, 'launch-title', read_param()['titleId'],
                '-lSceSystemService', '-lSceUserService')


def cmd_deploy(args):
    title_id = read_param()['titleId']
    app = DIST / title_id
    if not (app / 'eboot.bin').is_file():
        sys.exit('nothing to deploy: run "package" first')
    ftp = ftplib.FTP()
    ftp.connect(args.ip, args.port, timeout=30)
    ftp.login()
    running = running_title(ftp)
    if running == title_id and args.stop:
        print(f'stopping {title_id}', flush=True)
        stop_title(args.ip, args.elf_port, title_id)
        for _ in range(15):
            time.sleep(2)
            running = running_title(ftp)
            if running != title_id:
                break
    if running:
        sys.exit(f'{running} is running on the console: close it first (never replace files under a '
                 'running game, and never interrupt someone else\'s; --stop closes this title)')
    target = f'/data/homebrew/{title_id}'
    made = set()

    def mkdirs(path):
        current = ''
        for part in path.strip('/').split('/'):
            current += '/' + part
            if current not in made:
                try:
                    ftp.mkd(current)
                except ftplib.error_perm:
                    pass  # it exists
                made.add(current)

    def remove(path):
        try:
            ftp.sendcmd(f'DELE {path}')  # the console's server answers 226, which ftplib.delete rejects
        except ftplib.error_perm as error:
            if not str(error).startswith('550'):
                raise

    files = sorted(p for p in app.rglob('*') if p.is_file())
    if args.eboot_only:
        files = [app / 'eboot.bin']
    # What the last deploys to this console sent, by SHA-256: a file is sent
    # again only if it changed, or the console's copy isn't that size. Each
    # upload gives the file a new time, and the title compiles a map file
    # again when its time changes (maps.cc), for all of them minutes.
    record_path = BUILD / f'deployed-{args.ip}.json'
    try:
        record = {} if args.all else json.loads(record_path.read_text())
    except (OSError, ValueError):
        record = {}
    BUILD.mkdir(exist_ok=True)
    sent = 0
    for path in files:
        relative = path.relative_to(app).as_posix()
        remote = f'{target}/{relative}'
        digest = sha256(path)
        if record.get(relative) == digest:
            try:
                ftp.voidcmd('TYPE I')
                if ftp.size(remote) == path.stat().st_size:
                    continue
            except ftplib.error_perm:
                pass  # not there: send it
        directory, name = remote.rsplit('/', 1)
        temporary = f'{directory}/.{name}.upload'
        # A dropped connection (WSL's network blinks) is tried again, from a
        # new one; each file takes its name only once it is all there.
        for attempt in range(4):
            try:
                mkdirs(directory)
                remove(temporary)
                with path.open('rb') as stream:
                    ftp.storbinary(f'STOR {temporary}', stream, blocksize=256 * 1024)
                remove(remote)
                ftp.rename(temporary, remote)
                break
            except (OSError, EOFError, ftplib.error_temp) as error:
                if attempt == 3:
                    raise
                print(f'{remote}: {error}; trying again', flush=True)
                time.sleep(5)
                try:
                    ftp.close()
                except OSError:
                    pass
                ftp = ftplib.FTP()
                ftp.connect(args.ip, args.port, timeout=30)
                ftp.login()
        print(remote, flush=True)
        sent += 1
        record[relative] = digest
        record_path.write_text(json.dumps(record, indent=0, sort_keys=True))
    ftp.quit()
    print(f'deployed {sent} changed files of {len(files)} to {target}')


def bmp_to_png(bmp, png, width=1280):
    """The debug build's 32-bit screen BMP as a PNG, scaled to width."""
    import struct
    import zlib
    data = bmp.read_bytes()
    offset = struct.unpack_from('<I', data, 10)[0]
    w, h = struct.unpack_from('<ii', data, 18)
    if struct.unpack_from('<H', data, 28)[0] != 32:
        sys.exit(f'{bmp}: not a 32-bit BMP')
    masks = (struct.unpack_from('<III', data, 54) if struct.unpack_from('<I', data, 30)[0] == 3
             else (0xFF0000, 0xFF00, 0xFF))
    shifts = [(m & -m).bit_length() - 1 for m in masks]
    top_down, h = h < 0, abs(h)
    out_w, out_h = width, width * h // w
    raw = bytearray()
    for oy in range(out_h):
        y = oy * h // out_h
        row = offset + (y if top_down else h - 1 - y) * w * 4
        raw.append(0)
        for ox in range(out_w):
            v = struct.unpack_from('<I', data, row + (ox * w // out_w) * 4)[0]
            raw += bytes(((v & m) >> s) & 0xFF for m, s in zip(masks, shifts))

    def chunk(kind, body):
        return (struct.pack('>I', len(body)) + kind + body
                + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF))

    png.write_bytes(b'\x89PNG\r\n\x1a\n'
                    + chunk(b'IHDR', struct.pack('>IIBBBBB', out_w, out_h, 8, 2, 0, 0, 0))
                    + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))


def cmd_keys(args):
    """Types keys into a --debug-input build and fetches its screen."""
    import io
    ftp = ftplib.FTP()
    ftp.connect(args.ip, args.port, timeout=30)
    ftp.login()
    if args.keys:
        keys = args.keys.replace('\\n', '\n').encode()
        # Whole, then renamed: the title reads the file as soon as it is there.
        ftp.storbinary('STOR /data/crawl-input.tmp', io.BytesIO(keys))
        ftp.rename('/data/crawl-input.tmp', '/data/crawl-input.txt')
        time.sleep(args.wait)
    BUILD.mkdir(exist_ok=True)
    bmp = BUILD / 'crawl-screen.bmp'
    with bmp.open('wb') as out:
        ftp.retrbinary('RETR /data/crawl-screen.bmp', out.write)
    ftp.quit()
    png = BUILD / 'crawl-screen.png'
    bmp_to_png(bmp, png)
    print(png)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('deps').set_defaults(fn=cmd_deps)
    build = sub.add_parser('build')
    build.add_argument('--debug-input', action='store_true',
                       help='a debug build, driven over FTP (never release it)')
    build.set_defaults(fn=cmd_build)
    package = sub.add_parser('package')
    package.add_argument('--with-music', action='store_true',
                         help='carry the music (otherwise the title downloads it from its menu)')
    package.set_defaults(fn=cmd_package)
    deploy = sub.add_parser('deploy')
    deploy.add_argument('--ip', required=True)
    deploy.add_argument('--port', type=int, default=2121)
    deploy.add_argument('--eboot-only', action='store_true', help='upload only eboot.bin')
    deploy.add_argument('--all', action='store_true', help='send every file, changed or not')
    deploy.add_argument('--stop', action='store_true',
                        help='close this title first if it is running (never another one)')
    deploy.add_argument('--elf-port', type=int, default=9021, help="elfldr's port, for --stop")
    deploy.set_defaults(fn=cmd_deploy)
    launch = sub.add_parser('launch')
    launch.add_argument('--ip', required=True)
    launch.add_argument('--elf-port', type=int, default=9021, help="elfldr's port")
    launch.set_defaults(fn=cmd_launch)
    keys = sub.add_parser('keys')
    keys.add_argument('keys', nargs='?', default='', help='what to type; none fetches the screen only')
    keys.add_argument('--ip', required=True)
    keys.add_argument('--port', type=int, default=2121)
    keys.add_argument('--wait', type=float, default=3, help='seconds before fetching the screen')
    keys.set_defaults(fn=cmd_keys)
    args = parser.parse_args()
    args.fn(args)


if __name__ == '__main__':
    main()
