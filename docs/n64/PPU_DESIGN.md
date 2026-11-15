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

## Work plan (2026-11-15)

Today: Blue records the raster journal; Green's PPU stage
(`draw_frame_rdp`) replays it with HDMA (the real unit, `dma.c`) to get
each line's registers, and builds lines for the RDP (`n64_ppu_build`): the
CPU makes palettes, windows and sprite planes, `rspbg` expands BG rows into
8-bit planes, the RDP composites the planes; unsupported lines fall back
to the software PPU (JM2: all of them, big tiles). Costs (ares, gameplay):
SMW PPU stage 16 ms + present 9 ms; JM2 230 ms + 27 ms.

0. **Measure and reference.** Per-stage timing (Green's replay, the
   builder, RSP and RDP busy time); `RENDER=2` pixel compare against the
   software PPU; ROM-free RSP test ROMs per overlay (as `rspbg/test`).
1. **Frame description.** The replay emits bands of lines with identical
   PPU state (the registers that matter per band) instead of building
   lines; this record, plus VRAM/CGRAM/OAM snapshots, is what the RSP
   consumes. The replay (journal + HDMA) stays on Green at first; moving
   it to the RSP is a later step (HDMA reads WRAM and ROM, which need
   snapshotting).
2. **Tile cache overlay.** VRAM to CI4/CI8 textures for dirty tiles
   (flipped variants if copy-mode texrects cannot flip), palettes to
   TLUTs.
3. **Scene builder overlay.** Mode 1 BG layers as texrects per band, in
   priority order, scissored by windows; then sprites (evaluation and
   texrects); the RDP draws straight into the display framebuffer.
   Replaces `rspbg`'s plane expansion and the CPU's sprite planes; SMW and
   Pitfall are the first consumers (they are fully on the RDP path today).
4. **Green as the closer.** Frame handoff and present only; the CPU line
   builder retires for covered features. Unsupported bands keep the
   software PPU (drawn as a texture) until covered, so migration is
   incremental.
5. **Features**, each against the software PPU: big tiles and
   offset-per-tile (cheap with the tile cache), colour maths (blender for
   add/half-add, RSP line renderer for subtraction), mosaic, Mode 7,
   hi-res.
