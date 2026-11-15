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
