#!/usr/bin/env python3
"""Link a native PS5 title: the link step of ps5-cc.

Takes an ordinary compiler link line (objects, archives, -L, -l, -Wl, -o) and
runs prospero-lld with the native app layout, giving the intermediate PIE that
`build_ps5.py package` converts to eboot.bin. What it adds:

- the startup objects (native/app_crt.cpp, app_cpp_runtime.cpp, the app heap
  and the libc++ locale shims), compiled into $PS5_RT_DIR when out of date;
- the app heap's allocator wrappers and its fopen (relative paths are read
  from /app0);
- libc++, libc++abi, libunwind and compiler-rt;
- only the SDK stubs of the modules in title-modules.txt, so a function only
  a module a title can't load exports fails the link by name;
- the platform layer's versions of the functions in platform-functions.txt,
  and of every other undefined function it has: a link that reports
  undefined symbols is retried once with those bound.

Environment: PS5_PAYLOAD_SDK (required), PS5_RT_DIR (default: ps5-rt next to
the output), PS5_CLANG (default clang-18), LLVM_NM (default llvm-nm-18).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
NATIVE = HERE / 'native'

RUNTIME = (
    ('app_crt.cpp', ['-std=c++20', '-fno-exceptions', '-fno-rtti']),
    ('app_cpp_runtime.cpp', ['-std=c++20', '-fno-exceptions', '-fno-rtti']),
    ('app_heap.c', ['-std=c11']),
    ('cts_runtime_shims.c', ['-std=c11']),
)
# The app heap (app_heap.c) replaces the allocator; cts_runtime_shims.c's fopen
# resolves relative paths against /app0.
WRAPS = ('malloc', 'calloc', 'realloc', 'free', 'posix_memalign', 'malloc_usable_size', 'fopen')
# Linker options of a desktop link that mean nothing here, or that this link sets itself.
DROPPED_LINKER_OPTIONS = {
    '--start-group', '--end-group', '-Bstatic', '-Bdynamic', '--as-needed', '--no-as-needed',
    '--allow-shlib-undefined', '--no-undefined', '--enable-new-dtags', '--export-dynamic', '-E',
    '-s', '--strip-all',
}
DROPPED_LINKER_OPTIONS_WITH_VALUE = {'-rpath', '-soname', '-h', '-m', '--version-script'}
UNDEFINED = re.compile(r'undefined symbol: ([A-Za-z_][A-Za-z0-9_]*)$', re.M)


def die(message):
    sys.exit(f'ps5-link: {message}')


def read_list(path):
    """Names from a list file: the first word of each line, without comments."""
    names = []
    for line in path.read_text().splitlines():
        word = line.split('#', 1)[0].split()
        if word:
            names.append(word[0])
    return names


def parse(args):
    out, inputs, libdirs, libs, extra, compile_flags = 'a.out', [], [], [], [], []
    i = 0

    def value():
        nonlocal i
        i += 1
        if i >= len(args):
            die(f'{args[i - 1]} needs a value')
        return args[i]

    def linker_option(parts):
        j = 0
        while j < len(parts):
            part = parts[j]
            if part in DROPPED_LINKER_OPTIONS_WITH_VALUE:
                j += 1
            elif part in DROPPED_LINKER_OPTIONS or part.startswith(('-O', '-rpath=', '-soname=',
                                                                    '--version-script=')):
                pass
            else:
                extra.append(part)
            j += 1

    while i < len(args):
        arg = args[i]
        if arg == '-o':
            out = value()
        elif arg.startswith('-o'):
            out = arg[2:]
        elif arg == '-L':
            libdirs.append(value())
        elif arg.startswith('-L'):
            libdirs.append(arg[2:])
        elif arg == '-l':
            libs.append(value())
        elif arg.startswith('-l'):
            libs.append(arg[2:])
        elif arg.startswith('-Wl,'):
            linker_option(arg[4:].split(','))
        elif arg == '-Xlinker':
            linker_option([value()])
        elif arg in ('-include', '-isystem', '-iquote', '-idirafter', '-imacros', '-I', '-D', '-U',
                     '-MF', '-MT', '-MQ', '-Xclang', '-target', '--target', '-x'):
            compile_flags += [arg, value()]
        elif arg.startswith('-'):
            compile_flags.append(arg)  # compiler options: used only for sources on the line
        else:
            inputs.append(arg)
        i += 1
    return out, inputs, libdirs, libs, extra, compile_flags


def compile_runtime(sdk, rt_dir):
    """The startup objects every link gets, rebuilt when their source is newer."""
    rt_dir.mkdir(parents=True, exist_ok=True)
    objects = []
    for name, flags in RUNTIME:
        source = NATIVE / name
        obj = rt_dir / (name + '.o')
        if not obj.exists() or obj.stat().st_mtime < source.stat().st_mtime:
            run(['sh', str(HERE / 'prospero-clang18'), *flags, '-O2', '-fPIC', '-ffunction-sections',
                 '-fdata-sections', '-D__PROSPERO__', '-c', str(source), '-o', str(obj)], sdk)
        objects.append(str(obj))
    return objects


def run(cmd, sdk, capture=False):
    env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk))
    result = subprocess.run(cmd, env=env, text=True, capture_output=capture)
    if not capture and result.returncode:
        die(f'failed: {" ".join(cmd)}')
    return result


def find_library(name, libdirs):
    found = [Path(d) / f'lib{name}.a' for d in libdirs if (Path(d) / f'lib{name}.a').is_file()]
    if len(set(found)) > 1:
        # PacBrew carries an SDL2 of its own beside ours: never pick one by -L order.
        die(f'-l{name} is in more than one -L directory ({", ".join(map(str, found))}): '
            'give the archive by its path')
    if found:
        return found[0]
    for directory in libdirs:
        if (Path(directory) / f'lib{name}.so').is_file():
            die(f'-l{name}: only a module stub (lib{name}.so) was found; a title imports system '
                'modules only from title-modules.txt')
    die(f'-l{name}: lib{name}.a not found in {", ".join(libdirs)}')


def find_stubs(libdirs):
    stubs = []
    for name in read_list(HERE / 'title-modules.txt'):
        for directory in libdirs:
            path = Path(directory) / name
            if path.is_file():
                stubs.append(path)
                break
        else:
            die(f'the stub {name} (title-modules.txt) is in none of {", ".join(libdirs)}')
    return sorted(stubs, key=lambda path: path.name)


class Platform:
    """The ps5_ functions of the platform layer, and which ones call their original."""

    def __init__(self, archive):
        self.archive = archive
        self.member_of = {}
        self.undefined = {}
        self.bodies = {}
        nm = os.environ.get('LLVM_NM', 'llvm-nm-18')
        listing = subprocess.run([nm, '-A', str(archive)], capture_output=True, text=True,
                                 check=True).stdout
        for line in listing.splitlines():
            location, _, rest = line.rpartition(': ')
            fields = rest.split()
            if len(fields) < 2:
                continue
            member = location.rpartition(':')[2]
            kind, symbol = fields[-2], fields[-1]
            if kind == 'U':
                self.undefined.setdefault(member, set()).add(symbol)
            elif kind in 'TW' and symbol.startswith('ps5_'):
                self.member_of[symbol] = member

    def references(self, member, function):
        """The symbols the relocations inside one function of an archive member refer to."""
        if member not in self.bodies:
            objdump = os.environ.get('LLVM_OBJDUMP', 'llvm-objdump-18')
            with tempfile.TemporaryDirectory() as scratch:
                subprocess.run(['llvm-ar-18', 'x', str(Path(self.archive).resolve()), member],
                               cwd=scratch, check=True)
                listing = subprocess.run([objdump, '-dr', '--no-show-raw-insn', f'{scratch}/{member}'],
                                         capture_output=True, text=True, check=True).stdout
            bodies, current = {}, None
            for line in listing.splitlines():
                header = re.match(r'^[0-9a-f]+ <([^>]+)>:$', line)
                if header:
                    current = bodies.setdefault(header.group(1), set())
                elif current is not None:
                    reloc = re.search(r'R_X86_64_\w+\s+([A-Za-z_][A-Za-z0-9_.]*)', line)
                    if reloc:
                        current.add(reloc.group(1).split('@')[0])
            self.bodies[member] = bodies
        # what it calls, and what the functions of the same object it calls call
        bodies, seen, pending = self.bodies[member], set(), [function]
        while pending:
            for symbol in bodies.get(pending.pop(), ()):
                if symbol not in seen:
                    seen.add(symbol)
                    if symbol in bodies:
                        pending.append(symbol)
        return seen

    def why_not(self, name):
        member = self.member_of.get('ps5_' + name)
        if member is None:
            return f'the platform layer has no ps5_{name}'
        # Only ps5_<name>'s own code matters: a neighbour in the same object may
        # call the original (ps5_localeconv_l calls localeconv, ps5_localeconv does not).
        if name in self.undefined.get(member, ()) and name in self.references(member, 'ps5_' + name):
            return f'ps5_{name} calls {name}: bound, it would call itself'
        return None


def link(lld, base, defsyms, sdk):
    return run([lld, *base, *[f'--defsym={n}=ps5_{n}' for n in sorted(defsyms)]], sdk, capture=True)


def main(argv):
    sdk = os.environ.get('PS5_PAYLOAD_SDK') or die('PS5_PAYLOAD_SDK is not set')
    sdk = Path(sdk)
    out, inputs, libdirs, libs, extra, compile_flags = parse(argv)
    libdirs = libdirs + [str(sdk / 'target' / 'lib')]
    rt_dir = Path(os.environ.get('PS5_RT_DIR') or Path(out).resolve().parent / 'ps5-rt')

    with tempfile.TemporaryDirectory() as scratch:
        # Build checks (such as meson's) compile and link a source in one command.
        objects = []
        for index, path in enumerate(inputs):
            if path.endswith(('.c', '.cc', '.cpp', '.cxx')):
                obj = f'{scratch}/{index}.o'
                run(['sh', str(HERE / 'prospero-clang18'), *compile_flags, '-c', path, '-o', obj], sdk)
                path = obj
            objects.append(path)

        # Archives given by path join the -l ones in the group, so their order does not matter.
        archives = [path for path in objects if path.endswith('.a')]
        objects = [path for path in objects if not path.endswith('.a')]
        archives += [str(find_library(name, libdirs)) for name in libs]
        platform_archive = next((a for a in archives if Path(a).name == 'libps5platform.a'), None)
        platform = Platform(platform_archive) if platform_archive else None

        defsyms = set()
        if platform:
            for name in read_list(HERE / 'platform-functions.txt'):
                reason = platform.why_not(name)
                if reason:
                    die(f'{name} (platform-functions.txt): {reason}')
                defsyms.add(name)

        target = sdk / 'target' / 'lib'
        clang = os.environ.get('PS5_CLANG', 'clang-18')
        resource = subprocess.run([clang, '--print-resource-dir'], capture_output=True, text=True,
                                  check=True).stdout.strip()
        builtins = Path(resource) / 'lib' / 'linux' / 'libclang_rt.builtins-x86_64.a'
        for path in (target / 'libc++.a', target / 'libc++abi.a', target / 'libunwind.a', builtins):
            if not path.is_file():
                die(f'missing {path}')
        lld = str(sdk / 'bin' / 'prospero-lld')
        # The -L directories also serve the archives' own requests (libc++'s
        # objects ask for pthread: the SDK's libpthread.a).
        base = ['-T', str(NATIVE / 'ps5-pie.ld'), '-L', str(NATIVE),
                *[f'-L{directory}' for directory in libdirs], '--eh-frame-hdr',
                '--version-script', str(NATIVE / 'app-symbols.map'), '-e', '_start', '-o', out,
                '--error-limit=0', *[f'--wrap={name}' for name in WRAPS], *extra,
                *compile_runtime(sdk, rt_dir), *objects,
                '--start-group', *archives, str(target / 'libunwind.a'), str(target / 'libc++abi.a'),
                str(target / 'libc++.a'), str(builtins), '--end-group',
                '--as-needed', *map(str, find_stubs(libdirs))]

        result = link(lld, base, defsyms, sdk)
        undefined = sorted(set(UNDEFINED.findall(result.stderr)))
        if result.returncode and undefined and platform:
            bindable = [name for name in undefined if not platform.why_not(name)]
            if bindable:
                print(f'ps5-link: from the platform layer: {" ".join(bindable)}', file=sys.stderr)
                defsyms.update(bindable)
                result = link(lld, base, defsyms, sdk)
        if result.returncode:
            sys.stderr.write(result.stderr)
            undefined = sorted(set(UNDEFINED.findall(result.stderr)))
            if undefined:
                hint = ('no module in title-modules.txt exports them, and the platform layer does '
                        'not have them: provide them in the title')
                die(f'undefined: {" ".join(undefined)}\n  ({hint})')
            die('the link failed')
        sys.stderr.write(result.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
