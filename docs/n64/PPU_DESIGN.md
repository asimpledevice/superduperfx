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
8-bit planes, the RDP composites the planes into an offscreen image the
CPU then presents; unsupported lines fall back to the software PPU (JM2:
all of them, big tiles). Costs (ares, gameplay): SMW PPU stage 16 ms +
present 9 ms; JM2 230 ms + 27 ms. The software PPU stays the reference
throughout (`RENDER=0`; `RENDER=2` compares).

0. **Measure, reference, and check the hardware facts the plan rests on.**
   - Per-stage timing: Green's replay (journal + HDMA), the line builder
     (palettes, windows, sprites), RSP busy time, RDP busy time, present.
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
2. **Straight to the display.** The RDP draws into the framebuffer the VI
   shows (256 wide, VI-scaled, if step 0 confirms), retiring the CPU's
   present copy. Independent of the rest; an early, measurable win.
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
3. **The VI can replace the present stage.** It scans out a framebuffer of
   programmable width, scales it to the screen and filters it after
   drawing; if the RDP draws a 256-wide image the VI shows directly, the
   CPU's present copy (SMW 9 ms, JM2 27 ms) mostly goes away.
4. **Tile decode stays on the RSP.** SNES tiles are planar (a pixel's bits
   spread over 2-8 bytes, each byte holding a bit of 8 pixels); a palette
   lookup maps one byte to one colour, so the RDP cannot assemble planar
   pixels. Decode once into the tile cache, then draw with the RDP's
   colour-indexed path.
5. **Memory passes are the cost; fixed-function tricks are free.** Prefer
   one pass straight into the final framebuffer, extra buffers only for
   bands that need colour maths, and palettes and the combiner wherever the
   SNES needs a per-pixel transform.
