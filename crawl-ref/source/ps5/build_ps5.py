#!/usr/bin/env python3
"""Build Dungeon Crawl Stone Soup as a native PS5 title.

  deps      fetch and build everything the title is built with, pinned, into
            ps5/.deps: the payload SDK, the OpenGL SDK, PacBrew's libraries,
            the platform layer, SDL2 (ps5/sdl2), the host tools and the
            title runtime (sce_module/libc.prx)
  build     make PS5_NATIVE=y: the game, linked into the intermediate PIE
  package   eboot.bin and the title folder ps5/dist/<TITLEID>, and its zip
  deploy    upload ps5/dist/<TITLEID> to a console over FTP (never launches)

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
import subprocess
import sys
import tarfile
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
    make()


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

    archive = DIST / f'{title_id}.zip'
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(app.rglob('*')):
            if path.is_file():
                zf.write(path, path.relative_to(DIST))
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


def cmd_deploy(args):
    title_id = read_param()['titleId']
    app = DIST / title_id
    if not (app / 'eboot.bin').is_file():
        sys.exit('nothing to deploy: run "package" first')
    ftp = ftplib.FTP()
    ftp.connect(args.ip, args.port, timeout=30)
    ftp.login()
    running = running_title(ftp)
    if running:
        sys.exit(f'{running} is running on the console: close it first (never replace files under a '
                 'running game, and never interrupt someone else\'s)')
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
    for path in files:
        remote = f'{target}/{path.relative_to(app).as_posix()}'
        directory, name = remote.rsplit('/', 1)
        mkdirs(directory)
        temporary = f'{directory}/.{name}.upload'
        remove(temporary)
        with path.open('rb') as stream:
            ftp.storbinary(f'STOR {temporary}', stream, blocksize=256 * 1024)
        remove(remote)
        ftp.rename(temporary, remote)
        print(remote, flush=True)
    ftp.quit()
    print(f'deployed {len(files)} files to {target}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    for name, fn in (('deps', cmd_deps), ('build', cmd_build), ('package', cmd_package)):
        sub.add_parser(name).set_defaults(fn=fn)
    deploy = sub.add_parser('deploy')
    deploy.add_argument('--ip', required=True)
    deploy.add_argument('--port', type=int, default=2121)
    deploy.add_argument('--eboot-only', action='store_true', help='upload only eboot.bin')
    deploy.set_defaults(fn=cmd_deploy)
    args = parser.parse_args()
    args.fn(args)


if __name__ == '__main__':
    main()
