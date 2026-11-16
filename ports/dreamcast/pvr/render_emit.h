/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * render_emit: turns a dl_frame into textures and depth-ordered quads through a small backend
 * interface, so the same code drives the PowerVR (pvr_backend.c) and a host rasteriser that checks
 * it (tools/dreamcast/drawlist_check.c). Quads are in source pixels (320 x 224); u, v in texels.
 */
#ifndef DC_RENDER_EMIT_H
#define DC_RENDER_EMIT_H

#include "drawlist.h"

typedef struct rt_backend
{
	/* a w x h ARGB1555 texture (powers of two), or -1 when out of texture memory */
	int (*tex_alloc)(int w, int h);
	void (*tex_free)(int handle);
	/* rectangle upload: 'pixels' rows of 'stride' ARGB1555 values */
	void (*tex_write)(int handle, int x, int y, int w, int h, const uint16_t *pixels, int stride);
	/* textured quad (wrap: repeat, else clamp); z larger is nearer; alpha-tested (bit 15). Colour:
	   texel * mod / 255 + add per channel (ARGB8888 values, alpha ignored), clamped */
	void (*quad)(int handle, bool wrap, float x0, float y0, float x1, float y1,
		float u0, float v0, float u1, float v1, float z, uint32_t mod, uint32_t add);
	/* translucent shadow: where the texel's bit 15 is set and z >= the depth there, halve the colour */
	void (*shadow)(int handle, float x0, float y0, float x1, float y1,
		float u0, float v0, float u1, float v1, float z);
	/* opaque solid quad (backdrop), coloured as quads are */
	void (*solid)(uint16_t argb1555, float x0, float y0, float x1, float y1, float z, uint32_t mod, uint32_t add);
	/* optional: microseconds, for the phase timings in rt_stats */
	uint64_t (*clock_us)(void);
	/* optional (NULL: every sprite texture is ARGB1555): 4-bit paletted sprite textures, the colours in
	   palette banks of 16, so one texture serves a graphic in any colours. A w x h texture, freed with
	   tex_free; rows y .. y + h - 1 (y and h even) from one index byte a texel; bank 0..RT_PAL_BANKS-1's
	   colours (ARGB1555, bit 15 clear: transparent); a quad drawn from a paletted texture with a bank */
	int (*tex_alloc_pal4)(int w, int h);
	void (*tex_write_pal4)(int handle, int y, int h, const uint8_t *indices, int stride);
	void (*pal_set)(int bank, const uint16_t colours[16]);
	void (*quad_pal)(int handle, int bank, float x0, float y0, float x1, float y1,
		float u0, float v0, float u1, float v1, float z, uint32_t mod, uint32_t add);
	/* optional (NULL: not drawn): a translucent quad of one colour over what is at or behind z, its alpha
	   the colour's top byte (a shadow sprite's rectangle, the dimmed frame under a port's overlay) */
	void (*tint)(float x0, float y0, float x1, float y1, float z, uint32_t argb);
	/* optional (NULL: drawn as quad/quad_pal): a translucent textured quad, the texture's colours at 'alpha'
	   (0-255) over what is at or behind z; bank -1 for an ARGB1555 texture */
	void (*quad_alpha)(int handle, int bank, float x0, float y0, float x1, float y1,
		float u0, float v0, float u1, float v1, float z, uint32_t mod, uint32_t add, int alpha);
	/* optional (NULL: one screen): the screen the quads from here on are for (0 the left, 1 the right) */
	void (*view)(int screen);
	/* optional (NULL: the tile planes are converted to 16-bit plane textures): a rectangle of a paletted
	   texture (x, y, w, h even), and a palette bank's colours for the frame being made (applied before the
	   frame is drawn, not while the one before it may still be drawing) */
	void (*tex_write_pal4_rect)(int handle, int x, int y, int w, int h, const uint8_t *indices, int stride);
	void (*pal_stage)(int bank, const uint16_t colours[16]);
	/* Optional: a new texture made all clear (transparent), so no cell shows what the memory held before */
	void (*tex_clear)(int handle);
} rt_backend;

#define RT_PAL_BANKS 64

typedef struct rt_stats
{
	uint32_t cells_converted;
	uint32_t sprite_textures_built;
	uint32_t sprite_cache_hits;
	uint32_t sprite_evictions;
	uint32_t quads;
	uint32_t palette_banks_changed;
	uint32_t skipped;              /* features not drawn (shadow sprites, exclusion windows...) */
	uint32_t texture_memory_failures;
	uint32_t sprites_pending;      /* not drawn: graphics still loading */
	uint32_t sprites_deferred;     /* not drawn: this frame's texture budget spent */
	uint32_t build_us_max, build_texels_max;    /* the frame's slowest sprite texture build (with clock_us) */
	uint32_t sprites_fallback;     /* drawn with the same graphic's last texture while the new one waits */
	uint32_t sprites_missing;      /* not drawn at all: nothing of the graphic ready */
	uint32_t built_recolour;       /* textures started for a graphic already built in other colours */
	uint32_t built_new;            /* textures started for a graphic with no texture yet */
	uint32_t build_us_total;       /* time spent building sprite textures (with clock_us) */
	uint32_t plane_cells_late;     /* tile cells whose update waits for a later frame */
	uint32_t pal_sprites;          /* sprites drawn from paletted textures */
	uint32_t pal_banks_full;       /* sprites that could not get a palette bank (drawn ARGB1555) */
	uint32_t sprites_preview;      /* previews built (large sprites drawn from a coarse copy while theirs is built) */
	uint32_t missing_px;           /* screen pixels of sprites not drawn at all (host checks) */
	uint32_t phase_us[5];          /* palette scan, plane textures, backdrop + plane strips, sprites, shadows */
	uint32_t cells_margin;         /* tile cells beside those in sight redone ahead (margin_plane) */
	uint32_t direct_quads;         /* tile planes drawn straight from the tile store: quads */
	uint32_t direct_uploads;       /* tiles put in the tile store */
	uint32_t direct_argb;          /* 16-bit (tile, colours) copies made, for banks without a palette slot */
	uint32_t direct_banks;         /* palette slots in use for the planes */
	uint32_t p1_us[4];             /* (test overlay) phase 1 split: text, tile planes, margin, getting planes ready and choosing */
	uint32_t direct_runs;          /* (host checks) runs of the same cell along a line: quads if a tile were a texture of its own */
} rt_stats;

void rt_init(const rt_backend *backend);
void rt_palettes_lost(void);    /* (after a change of video mode: the palette memory cleared) */
/* Emits one frame (textures updated first, then quads). The backend must allow texture writes
   at this point (the previous frame's rendering finished). */
void rt_frame(const dl_frame *frame, rt_stats *stats);
/* Optional, before rt_frame for the same frame, while the backend cannot yet take texture writes to
   textures in flight: builds the frame's new sprite textures (fresh textures only) until keep_going()
   says stop or limit_us has passed, so time a backend spends waiting goes into them */
/* the time rt_frame may spend on sprite texture builds (RT_BUILD_US builds; default RT_BUILD_US) */
void rt_set_build_limit(uint32_t us);
extern rt_stats rt_prepare_stats;    /* (what the last rt_prepare counted) */
void rt_prepare(const dl_frame *frame, bool (*keep_going)(void), uint32_t limit_us);
/* The same for both screens' frames (2 PLAYERS: 'count' 2), each emitted after a backend view(screen) */
void rt_prepare_n(const dl_frame *const *frames, int count, bool (*keep_going)(void), uint32_t limit_us);
void rt_frame_n(const dl_frame *const *frames, int count, rt_stats *stats);

#endif
