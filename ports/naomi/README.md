# OutRunners on a SEGA NAOMI / NAOMI 2

*Resumen en español al final.*

The [Dreamcast port](../dreamcast/README.md) by Pezz82 (the PowerVR renderer, the AICA sound, the streamed media),
built for KallistiOS's **naomi** subarchitecture and booted from a NetDIMM. The NAOMI has the Dreamcast's SH-4,
PowerVR2 and AICA, with twice the main RAM (32 MB), twice the video memory (16 MB) and four times the sound memory
(8 MB): the same code runs, and the limits the Dreamcast set it can be raised step by step.

**State (2026-11-17): runs on the board** (NAOMI 2 in a Crazy Taxi upright, image `ordc_naomi_1`): the
cabinet's wheel, pedals, START and coin work; picture and sound right. Measured over 140 s of play with
`live.py ADDR watch`: 59-63 frames shown a second nearly always (a few seconds at 47-57 at a race's start), the
worst frame 19-33 ms, about 2.6 frames skipped a second (`frames_skipped`), 16.8 MB of heap and 3 MB of video
memory left. The owner sees some pop-in of objects (the port's frame budget, `RT_BUILD_US`, puts far sprites
off to the next frame). Next: raise the Dreamcast's limits with the NAOMI's memory and measure each step.

## What is different from the Dreamcast build

| | Dreamcast | NAOMI |
|---|---|---|
| The game | the PC version's menus (`ORUNNERS_CONSOLE`) | the arcade game as the cabinet has it: coins, START, the test menu |
| Controls | a pad (`../dreamcast/input.c`) | the cabinet's JVS through [naomi-kit](https://github.com/0skarzer0/naomi-kit) (`naomi_input.c`): wheel, pedals, the two-position shifter, START (a press puts a coin in, the release is START), SERVICE, TEST, coins |
| Data | the disc's `/cd/assets` | the NetDIMM's `/dimm` (naomi-kit's archive after the program) |
| Saves | the VMU | none yet (`DC_NO_VMU`; the SRAM later) |
| Texts on the screen | the BIOS font | none (the NAOMI has no BIOS font): they go to the console's log, which the netbooter's Raspberry reads |
| Live figures | the test overlay | naomi-kit's live block (`naomi_glue.c`): the port's own timings and the PowerVR's figures once a second |

The Dreamcast sources change only where both builds meet: `DC_ASSETS` (the data folder) in `dc_port.h`, and one
call at the top of `main()` under `DC_NAOMI`.

## Building

```bash
git clone --recurse-submodules https://github.com/0skarzer0/OutRunners-Dreamcast.git
cd OutRunners-Dreamcast/ports/naomi
source /opt/toolchains/dc/kos-naomi/environ.sh       # KallistiOS built with KOS_SUBARCH=naomi
make                                                  # -> build/outrunners-naomi.elf
python3 make_naomi.py /path/to/orunners.zip           # -> output/outrunners_naomi.bin.gz (about 2 minutes)
```

`make_naomi.py` takes the same steps as the disc builder (`../dreamcast/dist/make_disc.py`) from your own ROM set,
then `naomibintool` and naomi-kit's `nk_pack.py` instead of `mkdcdisc`. The image holds data from your ROM set: it
is for your own use; do not share it. A KallistiOS for NAOMI: see naomi-wheel-games'
[toolchain recipe](https://github.com/0skarzer0/outrunners-port/tree/naomi/ports/naomi).

`tools/core` (arcade-decomp) is at a later commit than the Dreamcast tree had pinned: the pinned one stops at
`symbols.txt`'s `unaligned` element lists (2026-11-17).

## On the cabinet

Copy `outrunners_naomi.bin.gz` to the netbooter (WiPi / Piforce: `/boot/roms/`) and send it to the NetDIMM. From
the Raspberry, with [naomi-pi-tools](https://github.com/0skarzer0/naomi-pi-tools)' `live.py`:

```bash
NK_DIMM=10.0.0.4 python3 live.py ADDR log        # the console (ADDR: sh-elf-nm build/outrunners-naomi.elf | grep dc_naomi_live)
NK_DIMM=10.0.0.4 python3 live.py ADDR watch 60   # frames shown a second, the worst frame, the PowerVR's figures
```

## Acknowledgements

- **Pezz82** ([MisterPezz82/OutRunners-Dreamcast](https://github.com/MisterPezz82/OutRunners-Dreamcast)): the
  Dreamcast port this is, its renderer, sound and media; all of it is his.
- **Sebastian Toyos (cheviman)**: the OutRunners C reconstruction and the PC version
  ([outrunners-port](https://github.com/cheviman/outrunners-port), [arcade-decomp](https://github.com/cheviman/arcade-decomp)).
- [KallistiOS](https://github.com/KallistiOS/KallistiOS) and its NAOMI subarchitecture;
  [DragonMinded](https://github.com/DragonMinded) (NetDIMM tools, the NAOMI homebrew groundwork).
- [naomi-kit](https://github.com/0skarzer0/naomi-kit) (from the NAOMI build of Daytona USA's native port by
  alphanu1 and dfchil) and the WiPi / Piforce netbooter.
- Claude (Anthropic) wrote the NAOMI part with the cabinet's owner.
- OutRunners is © SEGA. An unofficial fan project, not affiliated with or endorsed by SEGA.

## Resumen en español

Port de Dreamcast de Pezz82 compilado para NAOMI (KallistiOS `naomi`) y arrancado por NetDIMM: el juego arcade tal
cual (monedas, START, menú de test), los mandos del mueble con naomi-kit, los datos en `/dimm`, sin guardado aún y
los textos de pantalla al log. Primera etapa: medirlo en la placa tal cual; después aprovechar la memoria extra de
la NAOMI (todo en RAM sin caché de páginas, más memoria de vídeo para tiles y sprites, muestras sin comprimir).
**Funciona en la placa** (2026-11-17): 59-63 fps casi siempre, volante, pedales, START y monedas bien; algo de
«popping» de objetos. Siguiente: subir los límites de la Dreamcast con la memoria de la NAOMI.
