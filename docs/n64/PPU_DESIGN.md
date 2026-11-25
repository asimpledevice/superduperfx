# The SNES PPU on N64 hardware

Design notes for moving the PPU's work from the CPU to the RSP and RDP.
Since SuperDuperFX recompiles rather than emulates, HLE is preferred: the
goal is SNES pictures drawn the way an N64 program draws 2D, with the
software PPU (`RENDER=0`) kept as the build-time reference
(`RENDER=2` compares against it; see the shared HLE policy in AGENTS.md).

## SNES facts that shape the design (owner, 2026-11-15)

- **The colour formats match.** SNES colour is 15-bit BGR 5-5-5, which
  fits the N64's 16-bit RGBA 5-5-5-1 textures, palettes (TLUTs) and
  framebuffer exactly; the alpha bit can carry "transparent". SNES 4bpp
  tiles with a 3-bit palette select are exactly N64 CI4 textures with
  their 16 palettes of 16 colours, and 8bpp tiles are CI8. CGRAM converts
  to a TLUT directly.
- **The pixel rate is small by N64 standards.** The SNES shows about 57,000
  pixels a frame (256x224). The RDP's fill rate is far above that (a full
  256x224 layer pass is about 1 ms in 1-cycle mode, less in copy mode), so
  the cost of drawing SNES pictures on the N64 is in organizing the work:
  texture loads through the 4 KB TMEM, command counts, and RDRAM traffic
  (blending doubles it), not in raw pixels.

## Who does what (owner, 2026-11-15)

The deciding question for each piece: does the 65816 see the result at a
precise moment? If so it stays on the CPU, in step with the 65816; if it
only shapes the picture, it can be deferred to the RSP and RDP, one frame
behind (the pipelining we already have).

- **Blue: the virtual PPU** (the bus side of both chips). The register file
  (`$2100-$213F`), the VRAM, CGRAM and OAM ports (DMA included) with the
  authoritative copies and dirty ranges, and everything the game reads
  back: H/V counter latches, status flags including sprite overflow
  (`$213E`, answered by the exact sprite line table), VRAM/OAM/CGRAM
  read-back, and the Mode 7 multiplier (`$2134-$2136`, which games use as
  a multiply). It records each frame as a raster journal (register writes
  by line). It draws nothing.
- **RSP overlays (Yellow): most of PPU1, and two slivers of PPU2.** Tile
  decode into a CI4/CI8 tile cache (dirty tiles only); walking tilemaps
  (scroll, flips, big tiles, offset-per-tile) into texrect commands per
  band of lines; sprite evaluation for drawing (the same rules as Blue's
  flag check); Mode 7 (per-line matrix and sampling into line textures);
  palettes (CGRAM to TLUT, brightness); window logic into scissor
  rectangle lists; lines needing exact or subtractive colour maths,
  composited on the RSP; and the per-band plan (which path draws each
  band). Red sequences the overlays and feeds the RDP (Phase G).
- **RDP: most of PPU2.** Palettes as TLUTs, priority as draw order,
  windows as scissors, add and half-add colour maths in the blender,
  brightness in the combiner, mosaic as a point-sampled upscale, output
  into the framebuffer.
- **Green: the closer.** Closes the frame (journal, dirty ranges or a
  memory snapshot), kicks the RSP, presents the finished framebuffer, and
  plays audio. No per-tile or per-pixel work.

Sprite evaluation runs twice on purpose: on Blue for the game-visible
flags, on the RSP for drawing. Expected overlays (IMEM, as with the S-DSP
and its prep): a tile-cache converter, a scene builder (tilemaps, sprites,
windows to RDP commands), a line renderer (Mode 7, exact colour maths).

## Work plan (2026-11-15, revised after bsnes and Kaze)

Today: Blue records the raster journal; Green's PPU stage
(`draw_frame_rdp`) replays it with HDMA (the real unit, `dma.c`) to get
each line's registers, and builds lines for the RDP (`n64_ppu_build`): the
CPU makes palettes, windows and sprite planes, `rspbg` expands BG rows into
8-bit planes, the RDP composites the planes straight into the display
framebuffer (320x240); unsupported lines fall back to the software PPU,
drawn as textures (JM2: all of them, big tiles). "Present" (SMW 9 ms) is
waiting for `rspbg`'s jobs, issuing the RDP commands (a full-screen fill
each frame included) and the RDP drawing, plus the CPU writing the debug
status line into the framebuffer. Costs (ares, gameplay): SMW PPU stage 16 ms +
present 9 ms; JM2 230 ms + 27 ms. The software PPU stays the reference
throughout (`RENDER=0`; `RENDER=2` compares).

0. **Measure, reference, and check the hardware facts the plan rests on.**
   - Per-stage timing: Green's replay (journal + HDMA), the line builder
     (palettes, windows, sprites), RSP busy time, RDP busy time, and the
     parts of present (waiting for the RSP, issuing commands, RDP drawing,
     the status line).
   - Pixel compare against the software PPU (`RENDER=2`), with a
     tolerance mechanism for documented HLE differences; a ROM-free test
     ROM per new overlay (as `rspbg/test`).
   - Hardware checks (ares, then the EverDrive): combiner clamping of
     negative and overflowing results, and 5-bit to 8-bit expansion (does
     add/subtract land exactly; does an IA16 palette give exact 8-bit
     inputs); whether copy-mode texrects can flip; blender additive mode
     rounding; the VI showing a 256-wide framebuffer scaled to the screen
     (and how libdragon's display code sets it up).
1. **Frame description.** The replay emits bands of lines with identical
   PPU state instead of building lines; with VRAM/CGRAM/OAM snapshots (or
   dirty ranges) this is the contract the RSP consumes. The replay stays
   on Green at first (moving it needs WRAM/ROM snapshots for HDMA).
2. **Cheaper present.** The RDP already draws into the displayed
   framebuffer; what step 0 finds in present gets trimmed here (for
   example a 256-wide framebuffer the VI scales, if step 0 confirms it,
   instead of filling and drawing into 320x240; the status line off the
   per-frame path). Independent of the rest; an early, measurable win.
3. **Tile cache overlay.** Dirty VRAM tiles to CI4/CI8 textures (flipped
   copies if copy mode cannot flip); CGRAM to TLUTs with brightness.
4. **Scene builder overlay.** Mode 1 BG layers as texrects per band, in
   priority order (priority as depth stays an option), scissored by
   windows; then sprites (evaluation with exact limits, texrects). Red
   feeds the RDP (Phase G). Replaces `rspbg`'s plane expansion and the
   CPU's sprite planes. First consumers: SMW and Pitfall.
5. **Green as the closer.** Frame handoff and VI only; the CPU line builder
   retires for covered features. Bands not yet covered keep the software
   PPU (drawn as a texture), so migration is incremental.
6. **Features**, each against the software PPU:
   - big tiles and offset-per-tile (cheap with the tile cache);
   - colour maths: the subscreen drawn into its own buffer, then a combine
     pass on the RDP (add and subtract in the combiner, IA16 palettes for
     exact values if needed); the RSP line renderer only where the RDP
     cannot match;
   - mosaic (point-sampled upscale), hi-res, Mode 7 (RSP line sampling;
     later, perspective planes as real geometry);
   - fades and brightness as a combiner pass where cheaper than palettes.

## From bsnes (2026-11-15)

bsnes has two PPUs: `sfc/ppu`, cycle-stepped like the two chips (the fetch
slots per mode, sprite fetch in H-blank, mid-line effects work), and
`sfc/ppu-fast`, a scanline renderer: each visible line copies all
registers and CGRAM into a line record (`Line::cache`), and queued lines
are rendered in batches, in parallel (`Line::flush`). Points that shape
our design:

- **Flush on write does not fit us.** ppu-fast avoids copying VRAM by
  drawing the queued lines before any mid-frame VRAM or OAM write (lines
  read them live). We draw a frame later on another processor, so we need
  snapshots or dirty tracking instead (the pipeline's VRAM snapshot today;
  a dirty-tracked tile cache in the plan).
- **Priority as a depth test.** ppu-fast plots each layer with a priority
  compare per pixel, so drawing order does not matter. The RDP has a
  Z-buffer, and alpha compare can drop transparent texels so they write no
  depth: SNES priorities could become depth values and layers be drawn in
  any order (batched by texture). The cost is RDRAM traffic for Z reads and
  writes; strict draw order (the plan) is cheaper. Keep as an option.
- **Main and subscreen as two buffers.** ppu-fast composes the main
  screen and the subscreen into separate arrays and combines them per
  pixel for colour maths. For us: when a band needs layer colour maths,
  draw its subscreen into its own buffer, then combine (RDP blender for add
  and half-add; RSP or another RDP pass for subtraction).
- **HD Mode 7 and perspective correction.** ppu-fast finds runs of Mode 7
  lines whose per-line matrices (from HDMA) trace a perspective plane and
  interpolates them to rebuild it smoothly at higher resolution. The N64
  draws perspective-correct textured triangles in hardware, so such a plane
  could be real geometry at full resolution. The catch is TMEM (the Mode 7
  map is 1024x1024 at 8bpp), so the plane would be split into many small
  triangles of a few tiles each. A later enhancement.

## From Kaze's Majora's Mask filters video (2026-11-15)

The lesson: the RDP's fixed-function pieces are general-purpose hardware.
A colour-indexed texture and its palette are a 256-entry lookup table; two
texture slots with offset coordinates read alternate bytes; the combiner
is a small fixed formula, `(A - B) * C + D`, with texture alpha usable on
the colour side; the framebuffer is just memory, usable as a texture.
Majora's Mask makes its greyscale filter by reading the 16-bit framebuffer
as an 8-bit colour-indexed texture twice (offset by one byte), mapping one
byte through the palette's intensity and the other through its alpha (an
IA16 palette), and adding them in the combiner.

1. **Colour maths on the RDP, subtraction included.** With the main
   screen and the subscreen in separate buffers, a combine pass computes
   main - sub (`A = TEX0, B = TEX1, C = 1, D = 0`) or main + sub
   (`D = TEX1`); halving needs `C = 1/2` from a primitive or environment
   value. To verify: clamping (SNES clamps per channel) and the 5-to-8-bit
   expansion (replicated low bits can put an add one step off). An IA16
   palette can supply exact 8-bit values, as Majora's Mask does. Two
   256-pixel 16-bit rows of each screen fit TMEM together, so the pass works
   in small strips.
2. **The framebuffer is just memory.** Extra buffers (the subscreen) are
   cheap in principle; fades and brightness can be a combiner multiply over
   the finished frame; accessibility filters (greyscale, colour-blind
   remapping) are nearly free. Budget: a 256x224 16-bit buffer is about
   112 KB, and a full-screen memory pass costs on the order of 1 ms (Kaze:
   five passes are a fifth of a 30 fps frame at 320x240).
3. **The VI does the final scaling.** It scans out a framebuffer of
   programmable width, scales it to the screen and filters it after
   drawing. The RDP already draws into the displayed framebuffer (320x240,
   the SNES image centred); a 256-wide framebuffer the VI scales would
   draw and fill fewer pixels per frame.
4. **Tile decode stays on the RSP.** SNES tiles are planar (a pixel's bits
   spread over 2-8 bytes, each byte holding a bit of 8 pixels); a palette
   lookup maps one byte to one colour, so the RDP cannot assemble planar
   pixels. Decode once into the tile cache, then draw with the RDP's
   colour-indexed path.
5. **Memory passes are the cost; fixed-function tricks are free.** Prefer
   one pass straight into the final framebuffer, extra buffers only for
   bands that need colour maths, and palettes and the combiner wherever the
   SNES needs a per-pixel transform.

## Present measured (2026-11-15)

`N64_PRESENT_PROF` times present's parts; `N64_PRESENT_RDPWAIT` also waits
for the RDP after the frame's commands. Per frame, ares:

| | SMW | Pitfall | JM2 |
|---|---|---|---|
| Issuing the frame's RDP commands (CPU, rdpq) | 6.8-7.1 ms | 2.3-2.6 ms | 25 ms (software lines as textures) |
| Waiting for `rspbg` (merging its flags) | 1.8-1.9 ms | 1.3-2.4 ms | ~0 |
| Audio pump (audio out, S-DSP sync) | 1.8 ms | 1.3-1.5 ms | 2.0 ms |
| Status line (CPU text into the framebuffer) | 0.2 ms | 0.2 ms | 0.2 ms |
| display_get, display_show, waiting for the RDP | ~0 | ~0 | ~0 |

Present is the CPU building the RDP command stream, not drawing: the RDP
is done about 20 us after the last command. (ares may not model the RDP's
timing faithfully, so the RDP's own busy time needs hardware.) A 256-wide
framebuffer saves little CPU (the fill is one command); it is about RDP
pixels and RDRAM traffic, which only hardware will show. The large cost
is command generation, which is what the scene builder moves to the RSP
(steps 3-4).

## Step 1: the frame description (2026-11-15)

`n64/include/n64_ppu_desc.h`, `n64/src/n64_ppu_desc.c`. Green's replay
(`draw_frame_rdp`) calls `n64_ppu_desc_line` for each visible line after
the journal and HDMA have produced its registers. Lines with identical
registers and CGRAM merge into a band. A band is the line range, a CGRAM
version, and the PPU's register block byte for byte (`N64PpuRegs`, the same
X-macro list as the head of `struct Ppu`), so the description keeps the
PPU's own semantics and the RSP derives everything else. The frame also
records its CGRAM versions (copied when `cgramWriteCount` moves), its VRAM
and OAM images, and flags for VRAM or OAM written while the frame was drawn
(the game cannot do that during the picture, so a flag means something
unusual) and for running out of CGRAM versions (32).

Completeness check (`N64PPU_DESC_CHECK`, native harness with
`N64_RENDER=2`): a scratch PPU copied from the live one when the replay
starts is fed only from the description and drawn by the software PPU line
by line; its pixels must equal the live software PPU's. Because the
software PPU itself does the drawing, a pass means the description carries
everything every feature needs, not only what the RDP path supports.
`N64PPU_DESC_CHECK_BREAK` (every band drawn with the first band's
registers) shows the check fails when it should (SMW: from frame 345).

Results, 3,000 frames each (attract/boot): SMW, Pitfall and JM2 identical
on every line. Bands per frame: SMW 2.6-3.7, Pitfall 1.0-1.2, JM2 1.3-4.2.
None of the three changes CGRAM, VRAM or OAM while a frame is drawn (one
CGRAM version per frame), so those paths are not exercised by these games;
the flags will show the first game that does.

### Present on hardware (EverDrive, SMW intro, 2026-11-15)

The `N64_PRESENT_PROF` + `N64_PRESENT_RDPWAIT` build's on-screen line, per
frame: CPU issuing RDP commands 12.9 ms (ares: ~7), waiting for `rspbg`
3.6 ms (ares: ~1.8), audio pump 2.4 ms, **RDP pipeline busy 12.1 ms, TMEM
busy 3.6 ms** (ares does not implement the RDP's counters and shows the
RDP done ~20 us after the last command). The RDP's time is a real budget:
today's plane compositing costs about 12 ms of RDP time a frame in SMW.
The scene builder's design (fewer, larger texture loads; fewer overdraw
passes) should be judged on hardware RDP time, not ares.

## Step 0 findings (2026-11-15)

**SMW's world map is drawn entirely by the software PPU** (hardware status
line: PPU stage 166 ms there against about 14 ms in a level). The native
harness (`N64PPU_WHY_LOG`, scripted Start/A presses to game mode 0x0E)
shows every overworld line falling back for one reason, layer colour math
(`N64PPU_WHY_COLOR_MATH`; mosaic too during the fades). The registers:
Mode 1 with BG3 priority (`bgmode 09`), main screen BG1+BG3+OBJ, subscreen
BG2, `cgwsel 03` (add the subscreen), `cgadsub 30`: plain addition, no
halving, for sprites (palettes 4-7) and the backdrop. So the first colour
maths the scene builder needs is "main + subscreen BG2" for OBJ and the
backdrop: the subscreen drawn into its own buffer and an additive combine
(step 6), which becomes SMW's first feature after the basic scene.

*2026-11-24:* the backdrop part was already a palette (`tl_math`). The OBJ
part applies only to sprites of palettes 4-7, and the map as a new game
shows none, so lines without one in range are now RDP lines
(`objmath_line`): the whole map, 253 ms a frame in ares down to 60. Lines
that do hold such a sprite remain software lines; the additive combine is
still the design for them, and the hardware page says it cannot be the
RDP's own add (exact only below a sum of 47).

**Copy-mode flips (`n64/rdptest`, ares first; hardware pending).** 16x8
textures, RGBA16, CI8 and CI4 (palettes 0 and 3), drawn with texture
rectangles, read back and compared:

| | none | H (DsDx < 0) | V (DtDy < 0) | HV |
|---|---|---|---|---|
| copy mode | exact | wrong | exact | wrong |
| 1-cycle | exact | exact | exact | exact |

In copy mode a negative DsDx gives scrambled columns in groups of four
texels (copy mode fetches four texels a cycle), so horizontal flips need
either flipped tile copies in the tile cache or 1-cycle mode (4x slower
fill rate) for those tiles; vertical flips are free. To confirm on the
EverDrive (ares uses parallel-RDP).

**On hardware (EverDrive, owner's photos, 2026-11-24; `n64/testroms/rdptest.z64`).**

- *Flips (page 1):* as ares. Copy mode: none and V exact, H and HV wrong,
  for RGBA16, CI8 and CI4 with either palette, the columns scrambled in
  the same groups of four; 1-cycle mode exact in all four. 16 of 32 cases
  differ, the 16 expected.
- *Scissor (page 3):* as ares. Copy mode with the scissor's left edge
  inside the rectangle is right only at d = 0 and wrong for the five other
  offsets; clipped by hand (the rectangle started later, s advanced) it is
  right for all six, and 1-cycle mode is right both ways. The scene
  builder's by-hand clip stands.
- *Colour maths (page 5), of 1,024 pairs of 5-bit values:* the blender's
  50% is **exact for all** (the SNES's half-add). The blender's add is
  right for 63 (it wraps: 2 + 31 gives 0). The combiner's add is right for
  888: it clamps up to a point and wraps beyond (16 + 31 gives 0; the
  wrong ones are the sums of 47 and more). Its subtract is right for 888
  the same way (0 - 16 gives 31). Its half is right for 736 and one too
  high for the rest. So of the SNES's maths only half-add is exact on the
  RDP in one pass; add and subtract need the palette route (fixed colour,
  done) or the software lines.
- *RDP time for one 256x224 layer (page 6, the RDP's own counters):*

  | | ms |
  |---|---|
  | A: 28 strips of 256x8 CI8, 1-cycle (the planes) | 1.53 |
  | B: 896 tiles, a load and a copy-mode rectangle each | 5.41 |
  | C: the same tiles, 32 to a load (28 loads), copy mode | 3.12 |
  | D: as C in 1-cycle mode | 3.41 |

  The pipe is busy for nearly all of each (TMEM about 0.4 ms in every
  case). A layer from tiles costs the RDP two to three and a half times a
  layer from a plane, and the number of loads matters more than the mode:
  1-cycle is 9% slower than copy with the same loads, although it fills a
  quarter as fast on paper. That makes 1-cycle mode for horizontally
  flipped tiles a candidate against keeping flipped copies in the cache,
  and puts three tile layers at 9-10 ms of RDP time a frame (beside the
  CPU, not on it).
  *Acted on (2026-11-24):* the scene loads tiles in aligned groups of 8
  and keeps four groups in TMEM (`Tile_slot`, rsp_scene.S): 702, 1052 and
  1661 loads a frame in Joe & Mac 2, Pitfall and SMW become 133, 229 and
  275. Variant C's one 32-tile group was not the answer: the games' tile
  rows jump between groups (SMW would still load 663 times, each load
  four times the bytes), and the TMEM counter being the same in B and C
  says a load's time follows its bytes. `-DN64_SCENE_LOADSTAT` counts the
  loads of each policy (`[loadstat]`); `=2` also keeps the groups from one
  command to the next until something else loads a texture (SMW: 193
  instead of 275; not built, since every command would have to know what
  ran before it).
- *256-wide framebuffer (page 2):* shown full width on the CRT, grid and
  edge lines visible.

**Transparency is the TLUT entry's alpha, not the index.** With CI4 and
palette 3, texel 0 reads TLUT entry 48, which is opaque unless its alpha is
0. For 4bpp layers the TLUT gives entries 16n alpha 0 (colour 0 of each
palette); an 8bpp layer needs its own TLUT where only entry 0 is
transparent, so a band mixing 4bpp and 8bpp layers (modes 3 and 4) reloads
the TLUT between them.

## Step 3: the tile cache (2026-11-15)

`n64/rsptile`: an RSP overlay (`rsp_tile.S`, conversion code generated by
`gen_conv.py`), its libdragon side (`tile_cache.c`, `tile_cache.h`) and the
C reference (`tile_cache_ref.c`, portable). Not wired into the game yet:
the scene builder (step 4) is its consumer.

**Tile format.** Every VRAM tile of each enabled depth, as an RDP texture:
2bpp and 4bpp tiles as CI4, 8bpp as CI8. Each row holds the tile's pixels
left to right, then right to left, so copy mode reads the normal half
(s 0-7) or the horizontally flipped half (s 8-15), which it cannot do
with a negative DsDx (step 0 findings); vertical flips use a negative DtDy.
This also makes a CI4 row exactly one 8-byte TMEM word, so consecutive
tiles form one texture 16 texels wide (tile k at t = 8k) and one LOAD_BLOCK
brings in up to 32 CI4 tiles. Tiles are indexed by VRAM byte address
(2bpp: a / 16, 4bpp: a / 32, 8bpp: a / 64); colour indices exclude the
palette number, which the tile descriptor supplies. Sizes: 2bpp 256 KB,
4bpp 128 KB, 8bpp 128 KB, plus a 64 KB copy of VRAM as last converted;
each depth is optional (`TC_FMT_*`).

**Sync** (`TileCmd_Sync`): VRAM is walked in 256-byte blocks; each 16-byte
unit is compared with the last converted copy, and every 64-byte group
with a change is converted into each enabled cache (four 2bpp tiles, two
4bpp, one 8bpp). Conversion is palette-independent and needs no CPU dirty
tracking. Lane r of every vector is tile row r: a pixel's bit is isolated
with VAND (the `vshift` constants double as bit masks and powers of two)
and moved into place with a multiply-accumulate, so a 4bpp tile with both
halves takes about 130 RSP instructions.

**TLUTs** (`TileCmd_Tlut`): CGRAM to 256 RGBA5551 entries with INIDISP
brightness (round(c x l / 15) per component, as `ppu_brightness_level`),
in three arrangements: `TC_TLUT_16` (4bpp BG and OBJ: entries 16n
transparent), `TC_TLUT_256` (8bpp: entry 0 transparent), `TC_TLUT_2BPP`
(entries 16q+1..3 = CGRAM[base + 4q + 1..3] for two bases, or one base and
OBJ in the upper half). Brightness in the TLUT is exact for plain layers;
colour maths then brightness (the SNES order) is a step 6 question.

**Test** (`n64/rsptile/test`, ares): byte-identical to the C reference on
a full conversion of random VRAM, 40 rounds of random changes (single bits,
words, runs, whole blocks) and 300 random TLUTs (every arrangement,
brightness, CGRAM alignment). Times in ares (hardware pending): full
conversion of all three depths 21.6 ms; a sync with nothing changed
2.2 ms (reading 128 KB and comparing); a change round of ~27 groups
2.7 ms. The no-change cost matters, because it is paid on every frame that
touches VRAM: next are larger blocks, double-buffered DMA and a
block-level compare before the per-unit one, or dirty ranges from the
VRAM snapshot path, which already copies all 64 KB whenever VRAM changes.

## Step 4: the scene builder (2026-11-16)

**4a, BG quads (C reference).** `n64_ppu_scene.c` (`n64_scene_bg`) turns
one background layer over a run of lines into quads: one 8x8 tile per tile
row it crosses, with screen x, rows, tile-cache index, first texture row,
flips, priority and palette. `N64PPU_SCENE_CHECK` (native harness) draws
every run's quads from the tile cache's C reference the way the RDP does
and compares them with the plane rows the CPU expansion wrote: SMW (2,600
frames: title, intro, world map, a level; 1.1 million rows) and Pitfall
(3,000 frames) identical; `N64PPU_SCENE_CHECK_BREAK` fails as it should.
About 900 quads per full-screen layer.

**4b, drawing them (`SCENE=1`, opt-in, CPU-issued).** Main-screen BG
passes are drawn as copy-mode texture rectangles from the tile cache
(synced from the rspbg VRAM snapshot at present, after an `rdpq_fence`:
the RDP may still be reading the caches), with TLUTs per CGRAM version and
brightness (`TC_TLUT_16` for 4bpp layers, `TC_TLUT_2BPP` for 2bpp), and
each quad's per-band registers from the frame description. Passes with a
colour-window clip, sprites and the subscreen keep the planes for now.
RENDER=2 in ares (SMW title and attract demo, through frame 600): every
compared frame identical to the software PPU, the BG planes unused.

Found on the way: a copy-mode rectangle cut by the scissor's left edge
does not start its texture at the cut (ares; the leftmost tile under fine
scroll showed scrambled texels). Quads are clipped to the span on the CPU
side, with s advanced, instead.

Cost (ares, `N64_PRESENT_PROF`): issuing the RDP commands takes 99 ms a
frame against 5.9 ms for the planes: one `rdpq_tex_upload` and one
rectangle per quad (~2,700 a frame) from the CPU. This path exists to
prove the drawing; the commands are to be generated on the RSP (4c), with
fewer of them: empty tiles skipped (a per-tile flag from the tile cache),
several tiles per TMEM load, and the BG plane expansion (`rspbg` jobs)
dropped for layers the scene draws.

**4c, the RSP emits the rectangles (`SCENE_RSP=1`, default with
`SCENE=1`).** `n64/rsptile/rsp_scene.S` (`SceneCmd_Bg`): one command per
background pass (layer, priority, desc band rows, span): the RSP reads the
tilemap rows from the VRAM snapshot and writes, per tile, SYNC_LOAD,
SET_TEXTURE_IMAGE, LOAD_BLOCK, [SYNC_TILE, SET_TILE on a palette change]
and a copy-mode TEXTURE_RECTANGLE straight into rdpq's stream
(`rsp_rdpq.inc`, `RDPQ_Send` every 16 tiles), ending with SYNC_LOAD,
SYNC_TILE and SYNC_PIPE so rdpq's following commands need no knowledge of
it. The CPU issues the mode, the TLUT and one command per pass. Main-screen
BG layers drawn this way get no plane any more (`n64_ppu_build_line`
skips their expansion under the same rule the draw uses: mode 0 or 1, no
big tiles, no colour-window clip on the line). RENDER=2 in ares, SMW title
and attract demo: identical to the software PPU on every compared frame
through frame 750 (the RSP version matched on its first run).

Cost so far (ares, SMW attract, per frame): the CPU's scene work is small
(8 passes, 1.5-1.8 ms including 0.4 ms of TLUTs computed on the CPU, and
0.4 ms to start the cache sync), but present's command issue rose from
5.9 ms (planes) to 12.7 ms: the CPU stalls queuing the remaining plane
commands (sprites, subscreen) behind the RSP, which now converts tiles
(2.2 ms when VRAM changed) and emits ~2,700 rectangles. Next, in order:
skip empty tiles (a per-tile flag from the tile cache; SMW's sky is mostly
empty tiles), a cheaper no-change sync, sprites as rectangles (dropping
the OBJ planes), and then hardware RDP time, which ares does not model.

**Empty tiles.** The tile cache sync also keeps empty-tile bitmaps for the
2bpp and 4bpp caches (a tile is empty when its VRAM bytes are all zero:
every pixel colour 0), checked by the tile cache test (including tiles
emptied later); `SceneCmd_Bg` loads the layer's bitmap and emits nothing
for empty tiles. RENDER=2 still identical (SMW through frame 660); present's
command issue in ares went from 12.7 to 11.8 ms, so the number of
rectangles is not the main cost there. Next is measuring on hardware, where
the RSP's and the RDP's own time can be seen (ares models neither).

**Sprites (4d, 2026-11-17).** Sprites are drawn as copy-mode rectangles
from the 4bpp tile cache with the OBJ palettes (8-15 of the 4bpp TLUT), on
every line where that is exact, and those lines get no OBJ plane rows
(`N64PpuLine.obj_scene`). Once per frame, at the first such line, the CPU
turns OAM and OBSEL into strips: one per 8x8 sprite tile and run of rows
(`N64PpuFrame.obj_strip`, with the tile-cache index, x, rows, first tile
row and flips). Each priority pass walks the sprites back to front in OAM
rotation order, clips the strips to the band and span, and queues the
records (8 bytes each) for `SceneCmd_Tiles` (`rsp_scene.S`). That command
emits the same per-tile RDP commands as `SceneCmd_Bg`, skips empty tiles,
and takes at most 256 records per command, under `SCENE_RDP_ROOM`.

A line keeps the planes (`expand_obj`) unless the per-priority drawing
equals the hardware's sprite pixel, which holds when:
- no sprite is dropped: at most 32 in range and 34 tiles (range and time
  over);
- no sprite overlaps, by on-screen x range, a sprite that is later in
  rotation and has higher priority. On hardware the front sprite's pixel
  wins and takes its own, lower, priority, hiding the other sprite there.
  Front sprites of equal or higher priority are fine: they are drawn later,
  on top.
- OAM and OBSEL have not changed since the frame's strips were built.

The verdict is cached while a line's sprite set and rotation repeat.

RENDER=2 in ares: SMW identical through frame 1,380 (43 plane lines),
Pitfall through 510 (~114 rectangles a frame).

**Subscreen and colour-window passes (2026-11-17).** Subscreen BG layers
(under backdrop math) and main-screen spans clipped to black are drawn by
the scene too, through TLUTs in the tile-cache arrangement with the plane
palettes' colour rules (`n64_ppu_scene_tlut`: black for clipped main
pixels; backdrop math; math with the backdrop clipped). Sprites in
clipped spans use the black TLUT. `N64PpuLine.scene_bg` says which
screens' BG layers the scene draws, so build and draw share one rule. A
line now needs only BG mode 0 or 1 and no big tiles. RENDER=2: SMW
identical through frame 1,020 (~9,000 subscreen passes). No scene tested
so far clips to black (Tetris Attack stays in forced blank in this
build), so that path is checked only by construction (the same palette
rule as the planes).

**Sync only what changed (2026-11-17).** The PPU marks each VRAM store's
256-byte block (`Ppu.vramDirty`, host-only, next to `vramWriteCount`); the
build hands the marks on with each frame's VRAM snapshot, and the tile
cache syncs one job per run of dirty blocks (`n64_tile_cache_sync_dirty`:
gaps of up to two clean blocks joined, at most 16 jobs) instead of
comparing all 256 blocks. SMW attract: about 5-10 blocks a frame.

**TLUTs on the RSP (2026-11-17).** Plain TLUTs come from `TileCmd_Tlut`.
Each CGRAM version gets a per-frame copy, because the description's copy
is rewritten by the next frame's replay. The black and math TLUTs stay on
the CPU, now as three 32-entry per-component tables per TLUT. CPU time for
TLUTs, SMW attract: 935 us a frame before, 584 with plain TLUTs on the
RSP, 260 with the table-driven math TLUTs.

RENDER=2 in ares after all of 4d: SMW, Pitfall and Joe & Mac 2 identical
on every compared frame. Joe & Mac 2's lines use big tiles, so they keep
the planes.

**Where present's time goes (2026-11-17, ares, SMW attract).** With
`N64_PRESENT_PROF` the RDP-command phase is split by part, and
`N64_PRESENT_RSPSPLIT` drains the RSP after each part to attribute RSP
time. Scene build, before the change below: 15.7-16.0 ms a frame, of which
the CPU's own scene work is ~2.5 ms. The remaining 12.3-12.7 ms is spent
in `rdpq_detach_wait` waiting for the RSP. RSP time by part:
`SceneCmd_Bg` 13.5 ms, sprites 0.4 ms, work queued before present (tile
sync, TLUTs) 1.1-1.4 ms. Planes build: 5.6 ms (3.7 ms of uploads).

`SceneCmd_Bg` register cleanup: the per-pass values (priority, span,
cache, tile base, depth) stay in registers and are reloaded only after
`RDPQ_Send` and the row's DMAs. The row's TEXTURE_RECTANGLE words are
computed without x once per row. The tilemap entry is a pointer that steps
by two bytes and switches screens once at the column wrap. Result:
`SceneCmd_Bg` RSP time 13.5 -> 9.8 ms a frame; present's RDP commands
15.7-16.0 -> 11.7-11.8 ms; present 18.0 -> 14.4 ms. RENDER=2 identical
(SMW through frame 570, Pitfall 90, Joe & Mac 2 390). Next candidates:
one tilemap walk for both priorities (each layer is walked twice today),
and several tiles per TMEM load.

**What draws from planes now** (SCENE=1): lines with BG mode 2-7 or big
tiles; sprite lines that fail the rules above. Software lines are as
before (mosaic, hires/interlace, layer colour math, subscreen OBJ, direct
colour).

## The RSP must never wait on the RDP mid-command (owner, 2026-11-16)

A buffer only absorbs bursts: if the RDP is slower than the frame rate for
long enough, any buffer fills and the RSP blocks in `RDPQ_Send` (spinning
on DP_STATUS until the RDP has fetched a buffer), stalling every RSP job
queued behind, the S-DSP included. Decided:

- **Double-buffer what the RDP reads** (the tile caches, ~384 KB for 2bpp
  and 4bpp; RAM allows it: ~2.2 MB free with SCENE=1, `N64_MEM_REPORT`), so
  the next frame's tile sync never waits for the RDP and the per-frame
  `rdpq_fence` goes away.
- **Bound every graphics command's output** (scene passes split into a
  few tile rows, at most ~16 KB of RDP commands each) and have Red's
  dispatch check the RDP buffer for that much room before starting one;
  without room it keeps servicing its ring (the S-DSP) and retries. Waits
  for the RDP then happen only between commands, where audio can run.
  The frame-end SYNC_FULL follows the same rule.
- **Frame-level pacing in Green:** before queuing frame N's graphics, check
  the RDP has finished frame N-2 (or N-1); if not, skip drawing frame N
  (game logic and audio continue). A slow RDP becomes dropped frames, never
  an unbounded queue.

(Native N64 games solved the same problem with task yielding: the audio
task preempts the graphics task, whose microcode saves its state at safe
points.)

### Options for "never spins forever", and what is in (2026-11-16)

"Forever" is two cases: a slow RDP (the wait ends) and a hung RDP (it
does not).

1. **Room check before a graphics command starts (in).** A command that
   emits RDP commands first checks that the RDP buffer can take its
   worst-case output: room left in the current 64 KB buffer
   (`RDPQ_SENTINEL - RDPQ_CURRENT`), or no buffer pending at the RDP
   (`END_VALID` clear in `DP_STATUS`, and no SYNC_FULL in progress), as
   `RSPQCmd_RdpSetBuffer` waits. Without room it steps the queue pointer
   back over itself and returns to the dispatch loop, which serves Red's
   ring (the S-DSP) first and then runs it again. Done in `SceneCmd_Bg`
   (`SCENE_RDP_ROOM`, 17 KB); retries counted in the overlay's saved state
   (`n64_scene_rsp_room_waits`, `q` on the present line).
   Not in the shared dispatch loop: the rspq kernel is linked into every
   overlay's 4 KB of IMEM, and the S-DSP microcode already fills it to the
   last word, so 18 more kernel instructions broke its build (and
   libdragon's GL microcode). libdragon's own rdpq commands are therefore
   not covered: each emits at most 176 bytes and can still spin at a
   buffer switch when the RDP is a full buffer behind. Covering them needs
   kernel IMEM (freed elsewhere in the kernel, or in the S-DSP microcode).
2. **Bounded output per command (in).** Scene passes go to the RSP 32 rows
   at a time: at most five tile rows of 33 tiles at 56 bytes, ~9.3 KB,
   under the room the check requires (a static assert in `scene_rsp.c`).
   The tile cache sync emits no RDP commands, so it cannot spin, but it is
   long; splitting it is for audio latency, not for this.
3. **Frame-level pacing in Green** keeps the room check rarely failing
   (planned, above).
4. **A watchdog for a hung RDP** (planned): count how long a retry loop
   sees no progress in the RDP's fetch pointer (`DP_CURRENT`) and raise
   an error the hang probe shows, instead of freezing silently.
5. Not for us: yielding inside `RDPQ_Send`'s wait. The S-DSP overlay does
   not fit in IMEM beside a graphics overlay, and our overlays do not save
   and restore themselves mid-command.

RENDER=2 in ares with 1 and 2 in: SMW identical through frame 540, no
retries (ares draws instantly, so only hardware exercises the check).

## Mode 7 (2026-11-24)

Mode 7 lines were software lines (`N64PPU_WHY_MODE`). They are RDP lines
now, with the software PPU's mode 7 rules:

- **BG1 is a plane row.** `mode7_row` (n64_ppu_build.c) samples the line
  into BG1's plane (or SUB_BG1's) on the CPU, with the arithmetic of
  `PpuDrawBackground_mode7` (ppu.c): the 13-bit scrolls and centre, the
  matrix products masked to 1/4 texel, the flips, the large field's
  outside (transparent or tile 0). A pixel is a CGRAM index, 0 transparent.
- **Its palette** is selector `N64PPU_SEL_M7`: all 256 entries of CGRAM
  (the fixed-colour and subscreen variants come from the same code as the
  other selectors'), built only on mode 7 lines, which in turn skip the
  tile layers' selectors.
- **Order** `N64PPU_ORDER_MODE7`: OBJ 0, BG1, OBJ 1, OBJ 2, OBJ 3. Windows,
  the colour window, fixed-colour maths and the subscreen are the spans'
  and palettes' as for any plane.
- **Mosaic, EXTBG and direct colour** (same day, against the test ROM
  below): the sampler does the mosaic, by the rule of whichever software
  renderer draws the line (see `mode7_row`'s comment: the two place a
  mosaic differently); EXTBG's BG2 is a second plane row with the same
  texels, drawn below OBJ 0 through a palette of the pixels without bit 7
  and above OBJ 1 through one of those with it (`N64PPU_SEL_M7E_LO/HI`,
  orders `N64PPU_ORDER_MODE7_EXT*`); direct colour is BG1's palette
  computed from the entry number (`N64PPU_SEL_M7D`).
- The registers change line by line under HDMA, so a mode 7 line never
  takes the "same registers as the line above" shortcut.

The sampler looks a texel up only when the position leaves the last one
(its tile only when it leaves the tile), and without rotation (matrix C =
0) the line stays on one row of the field, which is a shorter loop.

Joe & Mac 2's mode 7 scene (frames 2650-2850; ares, ms a frame):

| | run | PPU | present | frame |
|---|---|---|---|---|
| software lines | 22.9 | 115 | 27.8 | 166 |
| RDP lines, first sampler | 22.9 | 45 | 4.6 | 73 |
| RDP lines, this sampler | 22.8 | 28-34 | 4.5 | 56-62 |

The picture against the software renderer (RENDER=2): no frame differs
through the scene, 224 RDP lines a frame. Of the PPU's 30 ms, the sampler
is about 17 (27 cycles a pixel) and the per-line work of a line whose
registers changed about 10 (`n64_ppu_build_line` 4.9, clearing the sprite
planes' rows 2, the description, the spans). Next, if a game needs more:
the sampler on the RSP (the lookups are scalar there too, but it runs
beside the CPU), and the per-line work.

## The PPU test ROM (2026-11-24)

None of the three games shows mode 7's rotation, EXTBG, direct colour or
modes 2-6, so the comparison needed its own pictures.
`n64/tools/ppu_test_rom.py` writes a LoROM image of generated data only:
pseudo-random VRAM, CGRAM and OAM, a short loop, and a script of register
writes a frame -- 31 scenes of 12 frames (mode 7: plain, zoom, rotation,
flips and the large field, negative positions, sprites with windows,
fixed-colour maths, the subscreen, mosaic, EXTBG, direct colour; modes 0-6;
mode 1 with mosaic; mode 2 with 16x16 tiles and windows; modes 4-6 with
mosaic, windows and colour math; 16x16 tiles in mode 3 and under a mosaic;
direct colour in modes 3 and 4; SETINI's bits; two-screen colour math in
modes 0-3 and 7). `n64/tools/ppu_test.sh`
builds the runner around it with RENDER=2 and prints, a scene, the RDP
lines, the software lines with their reasons, and the frames that differ.

It found a bug on its first day: the software PPU's per-pixel renderer
(`ppu_render_pixel`, ppu_legacy.c: the lines with EXTBG or direct colour,
and modes 4-6) stored a pixel as the bytes B, G, R, 0, which is the 32-bit
pixel the rest expects only on a little-endian host. On the N64 those
lines had red and blue exchanged and shifted. It stores the 32-bit value
now.

State after that commit: every mode 7 scene and modes 0 and 1 are RDP
lines with no frame differing; mode 1 with mosaic and modes 2-6 are
software lines.

**Mosaic in the tile modes (same day).** A layer with a mosaic gets its
plane row made on the CPU, a line at a time (`bg_mosaic_row`), as
`PpuDrawBackground_4bpp_mosaic` / `_2bpp_mosaic` draw it: the BG line is
the mosaic block's first; across, the blocks lie on the screen's grid, and
each part the layer's own windows cut the line into (`layer_parts`) starts
again with the pixel under its first pixel. Such a line draws every tile
layer from planes (the others' rows by the RSP as before the scene), so
its tile-layer palettes are built again. 16x16 tiles with a mosaic stay
software lines. Test ROM: mode 1 with mosaic, and mosaic with windows in
modes 0, 1 and 7, no frame differing; SMW's fades are RDP lines now.

**Modes 2 to 6 (same day).** Every mode has a row builder now; all of them
run on the CPU, a line at a time, and write plane rows.

| mode | builder | reference | planes and order |
|---|---|---|---|
| 2 | `bg_opt_row` | `PpuDrawBackground_4bpp_opt` | BG1, BG2 of 16 colours; `ORDER_MODE2` |
| 3 | `bg8_row` (BG1), the RSP's rows (BG2) | line renderer | BG1's priority 0 in the BG1 plane, priority 1 in the BG3 plane, 256-entry palette; `ORDER_MODE3` |
| 4 | `bg_px_row` | per-pixel renderer | BG1 as mode 3, BG2 of 4 colours; `ORDER_MODE4` |
| 5, 6 | `bg_px_row` | per-pixel renderer | BG1 of 16 colours, BG2 of 4 (mode 6: BG1 alone); `ORDER_MODE5` |

Each copies what its reference does, including where the references
disagree with each other: mode 2's offset columns come from the line
renderer's rules (a segment a source tile, none in the leftmost column,
16x16 tiles), modes 4 and 6's from the per-pixel renderer's; modes 5 and 6
are the software PPU's 256-wide reading of the 512-wide picture (the main
screen takes the odd columns, the subscreen and a mosaic layer the even
ones); a mosaic exists in modes 4-6 (by block origin) and not in 2 and 3.
`bg_px_row` fetches a tile again only when the pixel leaves it.

Cost is not measured against a game: none of the three uses these modes.
The builders are plain C at a pixel or a tile a step; if a game leans on
one, that builder is the place to look (the RSP, or tile-at-a-time code
as `bg8_row`).

## The last software lines (2026-11-25)

What still fell back after modes 2-6, and what became of each:

| case | now |
|---|---|
| 16x16 tiles in mode 3; with a mosaic in modes 0 and 1 | `bg_big_row` (as `PpuDrawBackgroundBig`, `PpuDrawBackground_8bpp`): any 16x16-tile layer of a line drawn from planes |
| direct colour, mode 3 | nothing to do: the software line renderer has no direct colour in mode 3, so the RDP line already matches it |
| direct colour, mode 4 | `N64PPU_SEL_M7D` when the line's BG1 tiles have palette number 0 (`bg_px_row` run dry tells); another palette number: software line (`N64PPU_WHY_DIRECT_COLOR`), since the colour takes three bits from it and a plane byte has no room |
| pseudo-hires | ignored, as the software PPU's 256-wide picture ignores it |
| interlace | only modes 5 and 6 change (`bg_px_row`: the field's rows) |
| OBJ interlace | `expand_obj` (half-height sprites, every other row); such lines take sprites from planes, not the scene |
| colour math with a colour from each screen | composed lines, below |

**Composed lines (`N64PPU_LINE_COMPOSE`).** Layer math against a composed
subscreen, and backdrop math with sprites on the subscreen, need two
colours for a pixel, and the RDP's own add and subtract are not exact
(page 5 of rdptest: only its 50% blend is). So the RDP does not combine
them. Such a line gets every layer of both screens as plane rows (as a
mosaic line does; sprites from `expand_obj`, the subscreen's too) and
palettes of the 15-bit colours themselves (`kTlutRaw`). At draw time
`n64_ppu_compose_line` finds each screen's top pixel -- eight pixels at a
time, the top order entry first, words already full skipped -- then does
the software PPU's math and brightness a pixel, and the row goes to the
RDP as a 16-bit strip, the way a software line does. `N64PPU_COMPOSE=0`
keeps the software lines.

On the CPU it reads the RSP's rows through the data cache (the lines are
dropped again after), and the test ROM's `math-two-screens` scene -- the
worst case: noise in every layer of both screens, about 24 rows read a
line -- costs about 0.25 ms a line for the top pixels and 0.22 ms for the
pixel loop (`-DN64_COMPOSE_PROF`).

**On the RSP (`n64/rspcmp`, `COMPOSE_RSP=1`, the default).** The same
work as a microcode, `rsp_cmp.S`; the CPU function stays as the reference
and as the path for what the microcode has not (direct colour; more
bands than a frame's 48 descriptors). `cmp_rsp.c` writes a descriptor a
band (spans, the entries' plane addresses and selector rules, the math's
parameters, the address of CGRAM as 256 colours) and a word a line (the
row and which entries have pixels on it); a command is up to 16 lines.
For each line the microcode:

1. paints each screen's entries bottom first into a plane of 16-bit
   values, 2 x CGRAM index plus 0x200 for "has math" (main) or "is there"
   (sub): the entry's row comes in by DMA, `LUV` loads 8 bytes as
   byte << 7, one compare decides opaque, `VMRG` takes the pixel;
2. turns values into colours with one table for every entry (a BG byte's
   low 7 bits and an OBJ byte's 8 are CGRAM indices), whose second half
   repeats the colours with bit 15 set so the flag rides through the
   lookup -- scalar, the only part that is;
3. does the math 16 pixels a pass: second colour (subscreen or fixed),
   add or subtract a channel, clamp, halve by a per-pixel factor,
   brightness as `(2cl + 15) * 2185 >> 16`, pack, and DMAs the row out
   for the strip the RDP draws next.

*The pipeline matters more than the instruction count.* ares models it
(and the hardware has it): one scalar and one vector instruction issue a
cycle at best, and an instruction reading a vector register stalls until
three cycles after the one that wrote it (a general register: two). The
first version, written as plain dependent chains, took 325 us of RSP time
a line; staged across pixel groups (the same operation for four groups,
then the next; the math as two groups' three channels, six streams) the
same work takes 144 us. Compare and `VMRG` can stay adjacent: `VCC` is
not a vector register and no stall hangs on it.

Measured (ares, test ROM, PPU + present time of a frame on the CPU, and
the RSP's time a composed line with the queue otherwise empty):

| scene | software lines | composed on the CPU | on the RSP | RSP a line |
|---|---|---|---|---|
| `math-one-layer` (BG1 over subscreen BG2, 224 lines) | 220 ms | 122 ms | 41 ms | 0.14 ms |
| `math-two-screens` (worst case, 205 lines) | 225 ms | 175 ms | 92 ms | 0.18 ms |

(The worst case's 92 ms is mostly its 61 ms of building noise layers'
rows, which is not the math.) Every row the RSP made was compared with
the CPU's (`-DN64_COMPOSE_CHECK`): none differs.

What it costs elsewhere: a full screen is about 32 ms of RSP time in the
frame, in commands of 2-3 ms. The queue is first in, first out, and the
S-DSP's batches do not use the high-priority queue, so a batch queued
behind a frame's composed lines waits for them, as it waits for the
scene's work today. Not measured: how late that makes audio in a game,
since no test game has more than a few composed lines. If it matters,
the commands are already short enough for the high-priority queue to cut
in between them.

Still software lines: mode 4 direct colour with a palette number other
than 0, and a frame that runs out of palettes (`N64PPU_WHY_PALETTES`).

Test ROM (31 scenes): no frame differing; software lines only in
`mode4-direct`'s frames that use every palette number.
## Plans: the CPU's per-line work, and colour math on the RDP (2026-11-25)

Owner decisions this day: minute, documented picture differences are
accepted for fades and mosaic in exchange for less CPU work; plan the
elimination of the PPU stage's per-line work, and a path for two-screen
colour math on the RDP.

### A. The PPU stage on the CPU

The stage replays the frame a line at a time (`draw_frame_rdp`,
n64_host.c): journal -> registers, description, build, the software PPU's
state-only line, HDMA; 224 times whatever the frame holds. Measured a part
(`-DN64_PPUSTAGE_PROF`, ares, us a frame):

| part | SMW, title | SMW, level | Joe & Mac 2, level | JM2, its mode 7 scene |
|---|---|---|---|---|
| replay (`ppu_rasterApplyLine`) | 420 | 540 | 450 | 3,500 |
| description (`n64_ppu_desc_line`) | 840 | 1,390 | 780 | 1,760 |
| build (`n64_ppu_build_line`) | 3,900 | 3,700 | 3,000 | 10,600 |
| line state (`ppu_runLineState`) | 880 | 2,340 | 1,500 | 1,720 |
| HDMA (`dma_doHdma`) | 1,200 | 5,460 | 560 | 920 |
| lines equal to the line above | 99% | 99% | 99% | 55% |

(The timers add to the stage: 14.7 ms with them against about 10 without
in SMW's level, so read the columns as shares.) Two things the sampled
profile of the 24th did not show: HDMA is the largest part in SMW's level,
5.5 ms for writes that leave 99% of the lines as the line above; and
replay proper is small.

The idea for all three: a line on which nothing changed should cost
almost nothing, and work that depends on OAM alone should be done once a
frame.

**A1. HDMA: a direct path, then no work for values that repeat.**
- `dma_doHdma` moves each byte through `dma_transferByte`: the S-DD1
  check, `snes_read` (the whole bus decode, the mapper, ROM paging), the
  debug hook, `snes_writeBBus`. First find out what SMW's level does a
  line (channels, bytes, direct or indirect, repeat or not): a counter a
  frame, logged.
- A channel's line as one call: resolve the table (and the indirect data)
  to a host pointer once per table entry -- WRAM and ROM are both plain
  memory to the host -- and write its 1 to 4 bytes with `ppu_write`
  directly when the B address is a PPU register (it almost always is).
  Anything else (S-DD1, a table that crosses a mapping, a B address
  outside $2100-$213F) keeps today's path.
- A mask in `Dma` of channels that are active and not terminated:
  `dma_doHdma` returns at once on 0 and walks only the set bits.
- Then, if the counter shows it: a repeat-mode channel that writes the
  bytes it wrote on the line above to registers nothing else has written
  since leaves the PPU as it was. That needs care with the write-twice
  registers (a scroll's value depends on the latch), so it is an option
  to measure after the direct path, not the first step.
- `ppu_write` bumps one counter, `regWriteCount`: a line on which it has
  not moved has the registers of the line above, with no comparison (A2
  uses this).
- Check: the state hashes of the three games (`gx.sh C`), which cover the
  registers the game can read back, and RENDER=2.

**A2. Description and build: bands, not lines.**
- `n64_ppu_desc_line` compares the whole register block with the band's
  on every line (its I-cache footprint alone was 1,165 misses a frame).
  With `regWriteCount`, CGRAM's, VRAM's and OAM's counters unchanged since
  the line above, extend the band and return true without the compare.
- `n64_ppu_build_line` with `same`: when the line's sprite verdict equals
  the line above's (A3 makes that a table lookup), set `ref[r] = ref[r-1]`
  and return; no temporary line, no `line_sprites` call.
- Lines drawn from planes (mode 7, mosaic, modes 2-6, composed lines) keep
  their per-line row builders: their content is per line by nature.
- Check: RENDER=2 on the games and the PPU test ROM (its scenes change
  registers every frame, and `mosaic-windows` and `setini` mid-screen).

**A3. Sprites: once a frame.**
- OAM and the OBJ registers rarely change during a frame. When
  `oamWriteCount` and OBSEL are what they were at the frame's first line,
  one pass over the 224 lines computes what each line needs: the sprites
  in range (already `s_obj_lines.mask`), the scene verdict
  (`obj_line_scene`: 32 sprites, 34 tiles, priority conflicts), the first
  sprite of the rotation, and the range-over and time-over flags that
  `PpuRunLineJob` computes a line at a time through `ppu_evaluateSprites`
  (they are a function of the same masks and tile counts).
- The line loop then reads a byte a line; `ppu_runLineState` is not called
  for lines the pass covered. A change of OAM or OBSEL mid-frame falls
  back to the per-line code from that line on.
- Check: the state hashes (the flags are readable at $213E), RENDER=2,
  and the PPU test ROM's sprite scenes.

Order: A1 (the largest part in SMW, and A2 and A3 use its counter), then
A3 (the line state and most of the build's time on a quiet line are
sprites), then A2; a measurement after each with `-DN64_PPUSTAGE_PROF`. Each is exact; none
needs the tolerance.

**What the tolerance buys (later, if fades or mosaic show up in a
profile):** a brightness below 15 as one combiner multiply over the
finished band (Kaze's "fades are a multiply over the frame") instead of
palettes rebuilt for every brightness step -- the RDP rounds differently
from `(2cl + 15) / 30` by at most a level; a layer's mosaic as a
point-sampled shrink and enlarge of the layer drawn off screen, instead of
`bg_mosaic_row`. Both touch only frames in transition.

**Done the same day (A1, the flags half of A3, the description half of
A2), and what the numbers meant.** Without the time categories, which
weigh on every `ppu_write`, SMW's level reads: replay 0.3 ms, description
1.2, build 3.2, line state 2.0, HDMA 2.7 -- for 448 HDMA bytes a frame,
one channel sending two bytes a line. So no part is slow at what it does;
each costs about a thousand cycles a line for being called at all, five
bodies of code taking turns 224 times. What was built:

- *HDMA* (`dma.c`): a transfer from work RAM to PPU registers goes
  straight to `ppu_write`; and it is not made when the channel sends the
  bytes of its last transfer to registers nothing has written since
  (`g_ppu_reg_stamp`) and whose write only stores the value (BGMODE,
  BGnSC, NBA, the window, screen and colour-math registers: not the
  write-twice scrolls and matrix, the memory ports, or MOSAIC). SMW: 446
  of 448 bytes a frame.
- *The description*: `ppu_write` counts its writes (`g_ppu_reg_writes`;
  whatever replaces the register block without it -- the replay's start, a
  load, a reset -- counts as writing all of it), and a line on which the
  count and the three memory counters have not moved extends the band
  without the compare.
- *The state-only line*: `ppu_runLineState` answers from the sprite table
  (`ppu_lineStateFast`) when the table is current and the frame's first
  line has been through `PpuRunLineJob`.

| us a frame, SMW level | before | after |
|---|---|---|
| replay | 290 | 280 |
| description | 1,220 | 180 |
| build | 3,220 | 2,820 |
| line state | 2,070 | 940 |
| HDMA | 2,670 | 800 |
| the stage (with the timers) | 10,820 | 6,280 |

The games' state hashes equal the build before, and RENDER=2 shows no
differing frame in the three games or the test ROM. Each change has its
reference behind a macro (`SNESRECOMP_HDMA_DIRECT`, `N64PPU_DESC_COUNT`,
`SNESRECOMP_LINE_STATE_FAST`).

Left: the build's 2.8 ms (the quiet line still goes through
`line_sprites`: the per-frame sprite verdicts of A3 and the `ref`
shortcut of A2), and the line state's and HDMA's remaining 0.9 and 0.8,
which are the price of the calls. The step after those is the loop
itself: replay, HDMA and the description for all 224 lines first, then
the build from the bands, so each body of code runs once a frame instead
of taking turns.

**Fact F2/F3 in ares (rdptest page 8):** add and subtract in three parts,
in place on the framebuffer, are right for all 1,024 pairs; in two parts
1,023 (m = 31 counts as 30). Hardware photo pending.

### B. Two-screen colour math on the RDP

Today the RSP composes these lines (n64/rspcmp): 0.14 ms of RSP time a
line, 32 ms for a full screen. Two ways to use the RDP were asked about.

**B1. The RDP does the lookups, the RSP only the math: not recommended.**
The RDP would composite each screen into a buffer and the RSP add the two
rows. But the lookups are a third of the RSP's time a line (the scalar
loop), the top-pixel search is already cheap in vectors, the RSP still
needs to know which main pixels have math (the RDP's buffer has no spare
bit: copy mode writes the palette entry, whose alpha bit must be 1 to be
drawn at all), and the RSP would have to wait for the RDP mid-frame, which
the design forbids. A third saved, for two more composites and a wait.

**B2. All of it on the RDP: plausible, to be proven a fact at a time.**
From the Majora's Mask filters (above): the framebuffer is a texture; two
tiles give two inputs a pixel; the combiner is `(A - B) * C + D` with
texture alpha usable on the colour side; a memory pass costs on the order
of a millisecond. And from our hardware page 5: the blender's 50% is exact,
the combiner's add is exact while the 5-bit sum stays below 47, its
subtract while the difference stays above -16.

1. *The main screen goes to the framebuffer as now, but in 1-cycle mode
   writing Z without comparing:* layers with math write a far Z, layers
   without a near one (the backdrop's is the fill). Painter's order leaves
   the top layer's Z at each pixel: a mask of "this pixel has math" that
   costs no colour bit. (Sprites: palettes 4-7 far, 0-3 near.)
2. *The subscreen goes to a small buffer B, through split palettes:* a
   channel value s becomes min(s, 15) in the first palette set, min(s - 15,
   15) in the second, s - 30 in the third (0 or 1). The fixed colour, split
   the same way, is B's fill, so "no subscreen pixel" needs no case in the
   unhalved modes.
3. *Add passes:* for each part, rows of the framebuffer as TEXEL0 and of B
   as TEXEL1 (four rows of each fit TMEM), 2-cycle,
   `(TEXEL1 - 0) * 1 + TEXEL0` with Z compare so only math pixels are
   written, in place. No sum reaches 47 (31 + 15), so each clamps right.
   Subtract: `(TEXEL0 - TEXEL1)`, parts of at most 15.
4. *Half:* where the subscreen has a pixel (B's alpha bit: a drawn pixel
   has it, the fill has not), the blender's 50% of B over the framebuffer,
   under the Z mask and alpha compare, then those pixels' Z set near so
   the fixed-colour passes skip them (Z as a third state).
5. *Memory:* Z and B only for the rows in hand: the colour and Z image
   addresses can point above a buffer of 32 rows, the scissor keeping the
   RDP inside it. About 40 KB in all, not a second and third screen.

Passes for a plain add: the main composite (1-cycle instead of copy), three
subscreen composites (usually one layer each), three add passes. If each
is 1 to 3 ms for a full screen, 10 to 15 ms of RDP time against 32 ms of
RSP time, and the RSP is free. With the third part dropped (a subscreen
channel of 31 added as 30: one level, where the main channel is 0) it is
two and two; that is a tolerance to ask for when the numbers are in.

**Facts to establish first, as pages of rdptest (ares, then photos):**
- F1. Z written without compare in 1-cycle mode by alpha-compared CI
  rectangles, and a later Z-compare-only pass: is the mask exact at
  transparent texels and at rectangle edges?
- F2. The split add and subtract over all 1,024 channel pairs, parts
  (15, 15, 1) and (15, 15): every result right?
- F3. A strip loaded from the framebuffer and drawn back onto the same
  rows; colour and Z images addressed above a short buffer.
- F4. The 50% blend under Z and alpha compare, and Z set by that pass.
- F5. Time a pass, by the RDP's counters, in 2-cycle mode with two loads
  a strip.

**Then:** `COMPOSE_RDP=1` beside the RSP and CPU versions, for add and
subtract first, checked by RENDER=2 on the test ROM's math scenes; the
default follows the measured times. No test game needs it today, so the
facts (F1-F5) are the work worth doing now; the rest waits for them.
