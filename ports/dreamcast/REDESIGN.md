# OutRunners for Dreamcast: the road to a locked 60 fps

Goal for the final release: every frame drawn at 60 fps in one player, no drawing faults (no late tiles,
no missing or blinking sprites), and 2 PLAYERS as close to that as the hardware allows.

## Where the time goes now (console, version 0.3 / test image AK)

Measured with the test overlay at the San Francisco transition (stage 1, 0'06 to 0'10):

| Work, per frame                                   | Time      |
|---------------------------------------------------|-----------|
| Game logic and the sound program (H, 1st figure)  | 9.2 ms    |
| of which the sound program (Q)                    | 3.9 ms    |
| Background planes: tiles and strips (H, 3rd)      | 6.7 ms    |
| Sprites, without builds (H, 4th)                  | 0.6 ms    |
| Waiting for the PowerVR (H, 2nd)                  | 0.4 ms    |
| Total                                             | 17 ms (a frame is 16.7) |

Tiles left late (L): about 12 a frame in the transition (the edge garbage). Dropped frames (D): 2 to 4 a
second. Tile pages unpacked (Z): about one every two frames (not the limit any more).

Host census over a FULL race (12,500 frames): the two tile planes in use need 64 colour banks a frame
(median), 95 at most; the ground plane covers up to 1,485 cells (row-scrolled); up to 1,120 cells change
in one frame; up to 98 palette banks change in one frame.

## Why: the current design fights the hardware

The arcade board draws from 4-bit and 8-bit graphics through palettes, in hardware, every line. The port
turns that into finished 16-bit textures on the SH4: every tile cell is unpacked, looked up through its
palette and copied (512 bytes) into a plane texture whenever its tile, its palette or its scroll position
changes; every sprite likewise. A palette change or a change of scenery means hundreds of cells redone in
one frame, which the SH4 cannot do inside its share of 16.7 ms. Everything the console shows wrong comes
from that one design: late tiles at the edges, colour bands, blinking far scenery, reduced resolution.

## The plan

Each step is measured on the console before the next starts; each is a separate test image.

### Step 1: measure precisely (small)
- Overlay split already in test image AL: plane tile work separate from strip drawing.
- Add: game logic alone, sound program alone, the submit thread's time, vertex count, VRAM free.
- Host: census of distinct (tile, colour bank) pairs a frame per plane, the figure Step 3 depends on.

### Step 2: the sound program's 3.9 ms (medium)
The Z80 program itself is small; most of the 3.9 ms is the emulation around it (cycle accounting called
5,000 times a frame, the FM chip's timers). Options, cheapest first:
- run the program in larger time slices, with the timers computed instead of stepped;
- drop the FM chip to timers only (OutRunners never plays a note on it: measured on the PC build);
- longer term, move the sound program to the AICA's own ARM7 processor, which drives the sound chip
  directly (only if the ARM7 proves fast enough: it is far slower than the SH4).
Target: under 1.5 ms a frame.

### Step 3: background planes drawn straight from a tile store (large: the core of the redesign)
Settled by a model on the PC side over a full 31-course race (12,500 frames):
- Every tile's graphics kept once, as 4-bit pens, in a tile store in video memory (tiles never change: they
  come from the ROM). Uploading a tile is a 128-byte copy; the model needs 4,732 uploads in the whole race,
  at most 248 in one frame (at a stage's start).
- Each visible cell drawn as its own small quad from the store, coloured by the PowerVR from its palette
  memory. The colour banks a frame uses (44 a frame, at most 68) go into 56 palette slots, written just
  before the frame is drawn: a palette change or fade costs nothing.
- The few banks that do not fit are drawn from 16-bit copies of (tile, colours) in a second store: 1,176
  conversions in the whole race (at most 64 in a frame), against 153,726 tile conversions today (up to 1,120
  in one frame).
- Cells drawn: about 705 a frame (at most 796), more where the ground's rows are chosen line by line.
- (Composing whole plane textures on the PowerVR is not possible in this version of the Dreamcast library:
  rendering to a texture covers a screen-sized area only, without transparency.)
Target: plane work under 1.5 ms a frame; no late or stale tiles, ever.

### Step 4: sprites the same way (medium)
- Sprite graphics prepared in the PowerVR's own formats when the disc is built from the ROM set (the
  builder already converts them), so the console only copies them to video memory (by DMA), never
  converts a pixel.
- Paletted sprite textures wherever the 64-bank limit allows (already partly in place), 16-bit otherwise.
- Previews and half-resolution scenery then become unnecessary.
Target: no sprite ever missing or blinking; full resolution in 2 PLAYERS.

### Step 5: game logic (medium)
Profile the recompiled game code on the console (5 ms a frame) and optimise the hottest routines (memory
access helpers, the sprite list builder, the road). Target: 4 ms.

### Step 6: frame pacing (small)
With the above, the frame is about 8 ms of work: render a frame while the next is being computed, a
fixed 60 Hz cadence, and the overlay's D (dropped frames) at 0 in the busiest stretches, then 2 PLAYERS.

## Order and releases
- 0.4: Steps 1 and 2 (measurement, sound program cost), plus Step 3's tile atlas.
- 0.5: Step 3 complete (planes composed by the PowerVR) and Step 4.
- 0.6 onwards: Step 5 and 6, then 2 PLAYERS tuning.

## Risks
- Render to texture on the PowerVR has its own costs (a render pass per plane texture); to be measured in
  Step 3's first prototype before committing.
- Video memory (8 MB): two frame buffers, plane textures, the tile atlas and sprite textures must fit;
  today only 65 to 360 KB are free in a race, so the sprite cache must shrink as the atlas takes over.
- The 64-bank palette limit: the census shows planes alone need up to 95 banks, so paletted textures can
  only be part of the answer.
