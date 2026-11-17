#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""OutRunners for NAOMI: the netboot image from YOUR OWN OutRunners ROM set (MAME "orunners", the World set).
The same data as the Dreamcast disc (../dreamcast/dist/make_disc.py: steps 1 to 5), appended to the program as
naomi-kit's archive (read at /dimm) instead of a CDI. Nothing of the ROM comes with the repository.

Linux/WSL (python3, a C compiler, KallistiOS for NAOMI: KOS_BASE set, the program built with `make`):
    make_naomi.py ROM_ZIP [OUTPUT_FOLDER]       ->  OUTPUT/outrunners_naomi.bin.gz (for the netbooter)"""
import gzip, os, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parent.parent
FILES = ('core.assets', 'tiles4.z', 'sprites.z', 'pcm.bin', 'aica_fixed.bin', 'aica_index.bin')


def step(text):
    print(f'\n== {text}', flush=True)


def run(args, **kw):
    print('   ' + ' '.join(str(a) for a in args), flush=True)
    if subprocess.run([str(a) for a in args], **kw).returncode != 0:
        raise SystemExit(f'\nFAILED: {Path(str(args[0])).name}')


def py(script, *args, env=None, cwd=None):
    run([sys.executable, script, *args], env=env, cwd=cwd)


def built(tool):
    out = subprocess.run(['bash', str(SRC / 'tools/dreamcast/build_probe.sh'), tool], capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stdout, out.stderr)
        raise SystemExit(f'FAILED: building {tool}')
    return SRC / out.stdout.strip().splitlines()[-1]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    rom = Path(sys.argv[1]).resolve()
    out = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else HERE / 'output'
    elf = HERE / 'build' / 'outrunners-naomi.elf'
    bintool = Path(os.environ.get('KOS_BASE', '/opt/toolchains/dc/kos')) / 'utils/naomibintool/naomibintool'
    for need, what in ((rom, 'the ROM set'), (elf, 'the program (make)'), (bintool, 'naomibintool (KOS_BASE)')):
        if not need.exists():
            raise SystemExit(f'not found: {need} ({what})')
    start = time.time()
    work = out / 'work'
    shutil.rmtree(work, ignore_errors=True)
    (work / 'dimm').mkdir(parents=True)

    step('1/5 checking and extracting your ROM set')
    (SRC / 'rom').mkdir(exist_ok=True)
    shutil.copy2(rom, SRC / 'rom' / 'orunners.zip')
    py(SRC / 'extract_assets.py', '--force', 'rom/orunners.zip', cwd=SRC)

    step('2/5 packing the game\'s data')
    run([built('asset-probe'), SRC / 'assets', work / 'outrunners.assets'], stdout=subprocess.DEVNULL)
    py(SRC / 'tools/dreamcast/split_pack.py', work / 'outrunners.assets', work / 'split')
    py(SRC / 'tools/dreamcast/pack_tiles4.py', work / 'split/tiles.bin', work / 'split/tiles4.bin')
    py(SRC / 'tools/dreamcast/pack_compressed.py', work / 'split/tiles4.bin', work / 'split/tiles4.z', '4096')
    py(SRC / 'tools/dreamcast/pack_compressed.py', work / 'split/sprites.bin', work / 'split/sprites.z', '8192')

    step('3/5 the samples for the sound chip')
    encoder = work / 'aica_adpcm_encode'
    run(['cc', '-O2', '-o', encoder, SRC / 'tools/dreamcast/aica_adpcm_encode.c', '-lm'])
    py(SRC / 'tools/dreamcast/make_aica_pcm.py', work / 'split/pcm.bin', work / 'split',
       env=dict(os.environ, AICA_ENCODER=str(encoder), AICA_DEPTH='4'))

    step('4/5 the loading screen')
    sys.path.insert(0, str(SRC / 'ports/dreamcast/dist'))
    import make_disc    # (its own step, with the paths of this tree)
    make_disc.SRC = SRC
    try:
        if not make_disc.loading_screen(built('splash-frames'), work / 'outrunners.assets', work):
            print('   (no loading screen picture: the game starts all the same)')
    except SystemExit:
        print('   (no loading screen picture: the game starts all the same)')

    step('5/5 the netboot image')
    for f in FILES + ('splash.bin',):
        if (work / 'split' / f).exists():
            shutil.copy2(work / 'split' / f, work / 'dimm' / f)
    run([bintool, 'build', work / 'prog.bin', '-t', 'OUTRUNNERS', '-d', 'HOMEBREW', '-e', elf], stdout=subprocess.DEVNULL)
    py(HERE / 'naomi-kit/tools/nk_pack.py', work / 'prog.bin', work / 'dimm', work / 'outrunners_naomi.bin')
    with open(work / 'outrunners_naomi.bin', 'rb') as src, gzip.open(out / 'outrunners_naomi.bin.gz', 'wb', 9) as dst:
        shutil.copyfileobj(src, dst)
    for p in (work, SRC / 'rom', SRC / 'assets'):
        shutil.rmtree(p, ignore_errors=True)
    print(f'\nDone in {time.time() - start:.0f} s:\n   {out / "outrunners_naomi.bin.gz"}')
    print('This image holds data from your ROM set: it is for your own use, not for sharing.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
