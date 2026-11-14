/* SPDX-License-Identifier: BSD-3-Clause */
/* render_emit.c: see render_emit.h and PVR-RENDERER-DESIGN.md */
#include "render_emit.h"
#include <s32/machine.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#ifdef DL_CHECK
#include <stdio.h>
#include <stdlib.h>
#endif


static const rt_backend *be;
static uint32_t frame_no;
static rt_stats *st;

/* ---- palette banks: a version per 16 colours, bumped when any of them changes ---- */

/* (each screen's palette: a bank's number below is the screen's * 1024 + its bank; the right screen's
   shadow is allocated for 2 PLAYERS) */
#define BANKS (S32_PALETTE_ENTRIES / 16)
static uint16_t pal_shadow0[S32_PALETTE_ENTRIES] __attribute__((aligned(4)));
static uint16_t *pal_shadow[2] = { pal_shadow0, NULL };
static uint16_t bank_ver[2 * BANKS];
static uint32_t pal_epoch;
static bool pal_primed[2];

static void scan_palette(int screen)
{
	const uint16_t *p = s32_board.palette[screen];
	if (!pal_shadow[screen] && !(pal_shadow[screen] = memalign(32, sizeof(pal_shadow0))))
		return;
	uint16_t *shadow = pal_shadow[screen];
	uint16_t *ver = &bank_ver[screen * BANKS];
	for (int bank = 0; bank < BANKS; bank++)
	{
		if (pal_primed[screen])
		{
			/* 16 colours as 8 words; the palette and its shadow are 32-bit aligned */
			const uint32_t *a = (const uint32_t *)(const void *)&shadow[bank * 16];
			const uint32_t *b = (const uint32_t *)(const void *)&p[bank * 16];
			if (((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3]) |
				(a[4] ^ b[4]) | (a[5] ^ b[5]) | (a[6] ^ b[6]) | (a[7] ^ b[7])) == 0)
				continue;
		}
		memcpy(&shadow[bank * 16], &p[bank * 16], 32);
		ver[bank]++;
		pal_epoch++;
		if (pal_primed[screen]) st->palette_banks_changed++;
	}
	pal_primed[screen] = true;
}

static uint16_t argb(uint16_t c)    /* xBBBBBGGGGGRRRRR to ARGB1555 */
{
	return (uint16_t)(0x8000 | (c & 0x1f) << 10 | (c & 0x3e0) | (c >> 10 & 0x1f));
}

static int pal_bank(const dl_mix *m, int pixel)
{
	return (((m->palette_base + ((pixel >> m->shift) & 0xfff0)) >> 4) & (BANKS - 1)) | (m->screen & 1) * BANKS;
}

static uint32_t mix32(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

/* A colour offset set (video.c: offsets[] added to each 5-bit channel, then clamped) as the PowerVR
   can apply it: positive parts added (exact), negative parts as a multiplier (approximate) */
static const dl_frame *cur;
static void offset_colours(int set, uint32_t *mod, uint32_t *add)
{
	uint32_t m = 0xff000000u, a = 0xff000000u;
	for (int c = 0; c < 3; c++)
	{
		const int o = set < 2 ? cur->offsets[set][c] : 0;    /* r, g, b */
		const int shift = 16 - 8 * c;
		const int mul = o < 0 ? (31 + o) * 255 / 31 : 255;
		const int ad = o > 0 ? o * 255 / 31 : 0;
		m |= (uint32_t)(mul < 0 ? 0 : mul) << shift;
		a |= (uint32_t)(ad > 255 ? 255 : ad) << shift;
	}
	*mod = m;
	*add = a;
}

/* ---- planes ---- */

typedef struct plane_tex
{
	int tex;
	int w, h, cell, cols, rows;
	uint32_t config;
	uint32_t *sig;        /* 64 x 64 (allocated with the texture) */
	bool ring;            /* a window on the plane: 32 x 16 cells, plane cell (cx, cy) at (cx % 32, cy % 16) */
	uint16_t *owner;      /* (ring: the plane cell each of the window's cells holds, + 1; 0 none) */
	/* the colours the texture's cells were made in, by the cell's colour (its 512 banks from the layer's
	   palette base), and the version of each (0x10000 | the bank's version; 0 none yet): the layer moves to
	   new colours only all at once (update_tile_plane) */
	uint32_t *snap_ver;
	uint16_t *snap_col;
	uint8_t waited;       /* (frames the move to new colours has waited for room) */
} plane_tex;
/* 2 PLAYERS: the road planes (NBG0, NBG1: zoomed, their cells in sight never more than 21 columns by 15
   rows) as 512 x 256 windows, a quarter of a whole plane's texture, so both screens' planes fit in the
   texture memory with room for the sprites (the scenery planes, up to the whole width in sight, stay
   whole) */
bool rt_ring_planes;
#define SIG_BYTES (64 * 64 * sizeof(uint32_t))

/* Two sets of plane textures, alternated each frame: the PowerVR may still be drawing last frame's
   while this one's are updated (each set keeps its own record of what it holds) */
/* (one copy of each plane: the backend holds texture writes until the previous render is done; a backend
   without that can build with PLANE_SETS 2, at twice the plane memory) */
#ifndef PLANE_SETS
#define PLANE_SETS 1
#endif
static plane_tex plane_sets[PLANE_SETS][2][DL_SPRITES];
static int plane_set;
static int scr;    /* the screen being drawn (0 the left) */
#define PLANE(layer) (&plane_sets[plane_set][scr][layer])

#define TILE_CACHE 2048
typedef struct { uint16_t px[256]; uint64_t key; uint8_t pad[24]; } __attribute__((aligned(32))) tile_entry;
static tile_entry *tile_cache;    /* (1 MiB, allocated on first use; without it every cell converts) */
static uint16_t cellbuf[16 * 16] __attribute__((aligned(32)));

static int rt_screens_now = 1;    /* (the screens of the frame being drawn: 2 in 2 PLAYERS) */

static bool plane_ready(int layer)
{
	plane_tex *p = PLANE(layer);
	const bool ring = rt_ring_planes && (layer == DL_NBG0 || layer == DL_NBG1);
	if (p->tex >= 0 && p->w && p->ring != ring)
	{
		be->tex_free(p->tex);    /* (2 PLAYERS began or ended: the texture is made again its other size) */
		p->tex = -1;
		p->w = 0;
	}
	if (p->tex >= 0 && p->w)
		return true;
	p->ring = ring;
	if (ring && !p->owner && !(p->owner = malloc(32 * 16 * sizeof(uint16_t)))) p->ring = false;
	if (p->ring) memset(p->owner, 0, 32 * 16 * sizeof(uint16_t));
	if (layer == DL_TEXT) { p->w = 512; p->h = 256; p->cell = 8; }
	else if (layer == DL_BITMAP) { p->w = 512; p->h = 512; p->cell = 16; }
	else { p->w = 1024; p->h = 512; p->cell = 16; }
	p->cols = p->w / p->cell;
	p->rows = p->h / p->cell;
	if (p->ring) p->w = 512, p->h = 256;    /* (the texture; cols and rows stay the plane's) */
	if (!p->sig && !(p->sig = malloc(SIG_BYTES))) { p->w = 0; return false; }
	if (layer != DL_TEXT && layer != DL_BITMAP && !p->snap_ver)
	{
		p->snap_ver = malloc(512 * sizeof(uint32_t));
		p->snap_col = malloc(512 * 16 * sizeof(uint16_t));
		if (!p->snap_ver || !p->snap_col) { free(p->snap_ver); free(p->snap_col); p->snap_ver = NULL; p->snap_col = NULL; }    /* (then as in 2 PLAYERS) */
	}
	if (p->snap_ver) memset(p->snap_ver, 0, 512 * sizeof(uint32_t));
	p->tex = be->tex_alloc(p->w, p->h);
	memset(p->sig, 0, SIG_BYTES);
	if (p->tex < 0) { st->texture_memory_failures++; p->w = 0; return false; }
	return true;
}

/* the cells of a 1024 x 512 plane the frame's rows read, as bits per cell row (64 columns) */
static uint64_t visible[32];

static void mark_visible(const dl_plane *pl)
{
	memset(visible, 0, sizeof(visible));
	for (int y = 0; y < DL_HEIGHT; y++)
	{
		const int cy = (pl->sy[y] >> 4) & 31;
		for (int i = 0; i < pl->span_count[y]; i++)
		{
			const int first = (pl->sx[y] + pl->spans[y][i][0]) & 0x3ff;
			const int count = pl->spans[y][i][1] - pl->spans[y][i][0];
			int c0 = first >> 4, c1 = ((first + count - 1) >> 4);
			for (int c = c0; c <= c1; c++)
				visible[cy] |= 1ull << (c & 63);
		}
	}
}

/* Tile cells a frame may redo over all its planes, and the time for it. A palette fade or cycle changes
   every cell that uses the bank at once (a sky: 300 cells and more, each frame of the fade); redoing
   them all took the console and the emulators 10 ms a frame and overflowed the held-write buffer, whose
   drain waits a whole render: the slowdown in busy stretches. Past the allowance the rest keep their
   last texels and are redone over the next frames, starting each frame a few rows on so every cell
   gets its turn. */
#define PLANE_CELLS_A_FRAME 280          /* (x 512 bytes: inside the backend's held-write buffer) */
#define PLANE_NEW_CELLS 96               /* cells come into sight, done first (counted in the 280 too) */
static uint64_t seen[PLANE_SETS][2][DL_SPRITES][32];
bool rt_new_cells_first = true;   /* (test builds: X + B, a plane's move to new colours all at once) */
static int plane_new_left;
#ifndef PLANE_US_A_FRAME
#define PLANE_US_A_FRAME 3500
#endif
static int plane_cells_left;
static uint64_t plane_deadline;
static uint32_t plane_cells_late;        /* (cells left for a later frame, for the overlay) */

static bool plane_spent(void)
{
	if (plane_cells_left <= 0) return true;
	return be->clock_us && plane_deadline && be->clock_us() > plane_deadline;
}

/* A plane without memory for its own colours: the cells in the palette's colours as they are now, a few at a
   time, as before the planes kept their own (update_tile_plane). Without this the plane was not drawn at all
   (2 PLAYERS with a ghost loaded: the right screen's road black) */
static void update_tile_plane_live(const dl_frame *f, int layer)
{
	plane_tex *p = PLANE(layer);
	mark_visible(&f->planes[layer]);
	const dl_mix *m = &f->layers[layer];
	int pages[4], bank;
	dl_plane_pages(layer, pages, &bank);
	/* a change of the layer's palette base or shift, or of its tile bank, redoes every cell */
	const uint32_t config = (uint32_t)m->palette_base << 8 | (uint32_t)m->shift << 4 | (uint32_t)bank;
	if (p->config != config) { p->config = config; memset(p->sig, 0, SIG_BYTES); }
	const int first_row = (int)((frame_no * 5u) % (uint32_t)p->rows);
	/* Cells that came into sight this frame first, whatever the allowance (up to PLANE_NEW_CELLS): a cell
	   scrolled in at the screen's edge (the road and the ground, which swing sideways in a turn) showed
	   whatever the texture held there before until its turn came: the garbage at the bottom corners. A
	   cell already in sight that waits keeps its last colours, which shows far less. */
	uint64_t *before = seen[plane_set][scr][layer];
	for (int pass = 0; pass < 2; pass++)
	{
	bool spent = plane_spent();
	for (int n = 0; n < p->rows; n++)
	{
		const int cy = (first_row + n) % p->rows;
		if (!spent && (n & 3) == 0) spent = plane_spent();
		const uint64_t these = pass == 0 ? (rt_new_cells_first ? visible[cy] & ~before[cy] : 0) : visible[cy];
		if (!these) continue;
		for (int cx = 0; cx < p->cols; cx++)
		{
			if (!(these >> cx & 1))
				continue;    /* (a cell out of sight is checked when it comes into sight) */
			const int page = pages[((cy >> 4) & 1) * 2 + ((cx >> 5) & 1)];
			const uint16_t cell = dl_video_word((uint32_t)page << 9 | (uint32_t)(cy & 15) << 5 | (uint32_t)(cx & 31));
			const int colour = (cell >> 4) & 0x1ff;
			const int pb = pal_bank(m, colour << 4);
			/* the cell's word and its palette bank's version: 32 bits, 0 never (bit 31 set) */
			const uint32_t sig = 0x80000000u | (uint32_t)(bank_ver[pb] & 0x7fff) << 16 | cell;
			int slot = 0;
			if (p->ring)
			{
				/* (the window's cell holds another plane cell: this one is redone, the other forgotten) */
				slot = (cy & 15) * 32 + (cx & 31);
				const uint16_t mine = (uint16_t)(cy * p->cols + cx + 1);
				if (p->owner[slot] != mine)
				{
					if (p->owner[slot]) p->sig[p->owner[slot] - 1] = 0;
					p->sig[cy * p->cols + cx] = 0;
				}
			}
			if (p->sig[cy * p->cols + cx] == sig)
				continue;
			if (spent || plane_cells_left <= 0) { if (pass == 1) plane_cells_late++; continue; }
			if (pass == 0)
			{
				if (plane_new_left <= 0) continue;    /* (then in pass 1, as any other) */
				plane_new_left--;
			}
			plane_cells_left--;
			p->sig[cy * p->cols + cx] = sig;
			if (p->ring) p->owner[slot] = (uint16_t)(cy * p->cols + cx + 1);
			const int tx = p->ring ? (cx & 31) * 16 : cx * 16, ty = p->ring ? (cy & 15) * 16 : cy * 16;
			/* converted tiles are kept in RAM by everything their texels depend on, so a tilemap the game
			   double-buffers by switching pages costs copies, not conversions */
			const uint64_t key = (uint64_t)cell | (uint64_t)bank << 16 | (uint64_t)pb << 18 | (uint64_t)bank_ver[pb] << 29 |
				(uint64_t)(p->config & 0x3fff) << 45 | 1ull << 63;
			tile_entry *te = tile_cache ? &tile_cache[(key ^ key >> 23 ^ key >> 41) % TILE_CACHE] : NULL;
			if (te && te->key == key)
			{
				be->tex_write(p->tex, tx, ty, 16, 16, te->px, 16);
				st->cells_converted++;
				continue;
			}
			const uint8_t *pens = dl_tile_pens((uint32_t)bank << 13 | (cell & 0x1fff));
			/* the cell's 16 colours once (pen 0 transparent), then a lookup a texel */
			uint16_t lut[16];
			lut[0] = 0;
			for (int pen = 1; pen < 16; pen++)
				lut[pen] = argb(dl_color(m, colour << 4 | pen));
			const bool fx = (cell & 0x4000) != 0, fy = (cell & 0x8000) != 0;
			for (int y = 0; y < 16; y++)
			{
				const uint8_t *row = pens ? pens + (fy ? 15 - y : y) * 16 : NULL;
				uint16_t *out = &cellbuf[y * 16];
				if (!row)
					for (int x = 0; x < 16; x++) out[x] = lut[15];
				else if (fx)
					for (int x = 0; x < 16; x++) out[x] = lut[row[15 - x] & 15];
				else
					for (int x = 0; x < 16; x++) out[x] = lut[row[x] & 15];
			}
			be->tex_write(p->tex, tx, ty, 16, 16, cellbuf, 16);
			if (te) { te->key = key; memcpy(te->px, cellbuf, sizeof(te->px)); }
			st->cells_converted++;
		}
	}
	}
	memcpy(before, visible, sizeof(visible));
}


/* One cell into the plane's texture, in the colours the plane holds for it (snap_col) */
typedef struct { uint8_t cx, cy; uint16_t cell, colour, pb, slot; bool fresh, done, extra; } dirty_cell;

static void convert_cell(plane_tex *p, int bank, const dirty_cell *d)
{
	const int cx = d->cx, cy = d->cy, colour = d->colour;
	const uint16_t cell = d->cell;
	const uint32_t ver = p->snap_ver[colour];
	plane_cells_left--;
	p->sig[cy * p->cols + cx] = 0x80000000u | (ver & 0x7fff) << 16 | cell;
	if (p->ring) p->owner[d->slot] = (uint16_t)(cy * p->cols + cx + 1);
	const int tx = p->ring ? (cx & 31) * 16 : cx * 16, ty = p->ring ? (cy & 15) * 16 : cy * 16;
	/* converted tiles are kept in RAM by everything their texels depend on, so a tilemap the game
	   double-buffers by switching pages costs copies, not conversions */
	const uint64_t key = (uint64_t)cell | (uint64_t)bank << 16 | (uint64_t)d->pb << 18 | (uint64_t)(ver & 0xffff) << 29 |
		(uint64_t)(p->config & 0x3fff) << 45 | 1ull << 63;
	tile_entry *te = tile_cache ? &tile_cache[(key ^ key >> 23 ^ key >> 41) % TILE_CACHE] : NULL;
	if (te && te->key == key)
	{
		be->tex_write(p->tex, tx, ty, 16, 16, te->px, 16);
		st->cells_converted++;
		return;
	}
	const uint8_t *pens = dl_tile_pens((uint32_t)bank << 13 | (cell & 0x1fff));
	/* the cell's 16 colours once (pen 0 transparent), then a lookup a texel */
	uint16_t lut[16];
	lut[0] = 0;
	for (int pen = 1; pen < 16; pen++)
		lut[pen] = argb(p->snap_col[colour * 16 + pen]);
	const bool fx = (cell & 0x4000) != 0, fy = (cell & 0x8000) != 0;
	for (int y = 0; y < 16; y++)
	{
		const uint8_t *row = pens ? pens + (fy ? 15 - y : y) * 16 : NULL;
		uint16_t *out = &cellbuf[y * 16];
		if (!row)
			for (int x = 0; x < 16; x++) out[x] = lut[15];
		else if (fx)
			for (int x = 0; x < 16; x++) out[x] = lut[row[15 - x] & 15];
		else
			for (int x = 0; x < 16; x++) out[x] = lut[row[x] & 15];
	}
	be->tex_write(p->tex, tx, ty, 16, 16, cellbuf, 16);
	if (te) { te->key = key; memcpy(te->px, cellbuf, sizeof(te->px)); }
	st->cells_converted++;
}

/* the colour's 16 colours as the palette has them now, into the plane's */
static void take_colours(plane_tex *p, const dl_mix *m, int colour, int pb)
{
	for (int pen = 0; pen < 16; pen++)
		p->snap_col[colour * 16 + pen] = dl_color(m, colour << 4 | pen);
	p->snap_ver[colour] = 0x10000u | bank_ver[pb];
}

/* Cells a plane may redo at once to move to new colours, past the frame's allowance (the held-write buffer
   takes 384); and the frames a move waits for room before it is made anyway (a frame or two late) */
#define PLANE_CELLS_MOST 360
#define PLANE_WAIT_MOST 2
static dirty_cell dirty[2048];
static bool colour_behind[512];

/* A tile plane's cells in sight. The texture holds every cell in the colours the plane last moved to
   (snap_col), the cells that come into sight (scrolled in at an edge in a turn) included, so the picture
   is always of one palette. When the palette changes (a stage's colours fading in, a cycle), the plane
   moves to the new colours in one frame: every cell in sight of the colours that changed is redone at
   once, or none is and the plane waits a frame. (Redone a few at a time, as the frame's allowance came,
   the old and new colours mixed in steps and bands across the ground: the road and ground draw each line
   from another row of their plane, so a cell is a band.) */
static void update_tile_plane(const dl_frame *f, int layer)
{
	plane_tex *p = PLANE(layer);
	if (!p->snap_ver)
	{
		update_tile_plane_live(f, layer);
		return;
	}
	mark_visible(&f->planes[layer]);
	const dl_mix *m = &f->layers[layer];
	int pages[4], bank;
	dl_plane_pages(layer, pages, &bank);
	/* a change of the layer's palette base or shift, or of its tile bank, redoes every cell */
	const uint32_t config = (uint32_t)m->palette_base << 8 | (uint32_t)m->shift << 4 | (uint32_t)bank;
	if (p->config != config)
	{
		p->config = config;
		memset(p->sig, 0, SIG_BYTES);
		memset(p->snap_ver, 0, 512 * sizeof(uint32_t));
	}
	uint64_t *before = seen[plane_set][scr][layer];
	int nd = 0, behind_cells = 0, behind_colours = 0;
	for (int cy = 0; cy < p->rows && nd < 2048; cy++)
	{
		const uint64_t these = visible[cy];
		if (!these) continue;
		for (int cx = 0; cx < p->cols && nd < 2048; cx++)
		{
			if (!(these >> cx & 1))
				continue;    /* (a cell out of sight is checked when it comes into sight) */
			const int page = pages[((cy >> 4) & 1) * 2 + ((cx >> 5) & 1)];
			const uint16_t cell = dl_video_word((uint32_t)page << 9 | (uint32_t)(cy & 15) << 5 | (uint32_t)(cx & 31));
			const int colour = (cell >> 4) & 0x1ff;
			const int pb = pal_bank(m, colour << 4);
			if (!p->snap_ver[colour])
				take_colours(p, m, colour, pb);    /* (colours the plane has not had: as they are) */
			const bool behind = p->snap_ver[colour] != (0x10000u | bank_ver[pb]);
			int slot = 0;
			if (p->ring)
			{
				/* (the window's cell holds another plane cell: this one is redone, the other forgotten) */
				slot = (cy & 15) * 32 + (cx & 31);
				const uint16_t mine = (uint16_t)(cy * p->cols + cx + 1);
				if (p->owner[slot] != mine)
				{
					if (p->owner[slot]) p->sig[p->owner[slot] - 1] = 0;
					p->sig[cy * p->cols + cx] = 0;
				}
			}
			const uint32_t sig = 0x80000000u | (p->snap_ver[colour] & 0x7fff) << 16 | cell;
			const bool stale = p->sig[cy * p->cols + cx] != sig;
			if (!stale && !behind)
				continue;
			if (behind)
			{
				behind_cells += !stale;    /* (a stale cell is redone anyway: in the new colours costs nothing more) */
				if (!colour_behind[colour]) { colour_behind[colour] = true; behind_colours++; }
			}
			dirty[nd++] = (dirty_cell){ (uint8_t)cx, (uint8_t)cy, cell, (uint16_t)colour, (uint16_t)pb, (uint16_t)slot,
				!(before[cy] >> cx & 1), !stale, behind && !stale };
		}
	}
	memcpy(before, visible, sizeof(visible));
	/* the move to new colours: all at once, when it fits (or has waited long enough) */
	const int used = PLANE_CELLS_A_FRAME - plane_cells_left;
	bool move = false;
	if (behind_colours)
	{
		move = !rt_new_cells_first ||    /* (test switch off: as before, a few at a time) */
			(used + behind_cells <= PLANE_CELLS_MOST && !plane_spent()) || p->waited >= PLANE_WAIT_MOST;
		p->waited = move ? 0 : (uint8_t)(p->waited + 1);
	}
	else
		p->waited = 0;
	if (move)
	{
		for (int i = 0; i < nd; i++)
			if (colour_behind[dirty[i].colour] && (rt_new_cells_first || plane_cells_left > 0))
			{
				if (p->snap_ver[dirty[i].colour] != (0x10000u | bank_ver[dirty[i].pb]))
					take_colours(p, m, dirty[i].colour, dirty[i].pb);
				dirty[i].done = false;    /* (redone in the new colours, below) */
			}
	}
	for (int i = 0; i < nd; i++)
		colour_behind[dirty[i].colour] = false;
	/* the cells to redo: those that came into sight first, then the rest; past the allowance only a move
	   (a cell that waits keeps what it holds: of the plane's colours, but its old tile) */
	bool spent = plane_spent();
	int since = 0;
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < nd; i++)
		{
			dirty_cell *d = &dirty[i];
			if (d->done || (pass == 0) != d->fresh)
				continue;
			const bool moving = move && d->extra;    /* (redone past the allowance: the picture moves at once) */
			if (!spent && ++since >= 32) { since = 0; spent = plane_spent(); }    /* (the clock read now and then) */
			if (!moving && (plane_cells_left <= 0 || spent)) { plane_cells_late++; continue; }
			if (moving && PLANE_CELLS_A_FRAME - plane_cells_left >= PLANE_CELLS_MOST + PLANE_CELLS_A_FRAME) { plane_cells_late++; continue; }
			convert_cell(p, bank, d);
			d->done = true;
		}
}

/* a character's 32 bytes of graphics (16 words of video RAM), folded word by word */
static uint32_t char_sig[4096], char_sig_frame[4096];

static uint32_t char_signature(uint32_t character)
{
	character &= 4095;
	if (char_sig_frame[character] == frame_no)
		return char_sig[character];
	char_sig_frame[character] = frame_no;
	uint32_t h = 2166136261u;
	const uint32_t index = character * 16;
	for (uint32_t i = 0; i < 16; i++)
		h = (h ^ dl_video_word(index + i)) * 16777619u;
	return char_sig[character] = h;
}

static void update_text_plane(const dl_frame *f)
{
	plane_tex *p = PLANE(DL_TEXT);
	const dl_mix *m = &f->layers[DL_TEXT];
	const uint32_t config = (uint32_t)m->palette_base << 4 | m->shift;
	if (p->config != config) { p->config = config; memset(p->sig, 0, SIG_BYTES); }
	for (int cy = 0; cy < DL_HEIGHT / 8; cy++)
		for (int cx = 0; cx < DL_WIDTH / 8; cx++)
		{
			uint32_t character;
			const uint16_t cell = dl_text_cell(cx, cy, &character);
			const int pb = pal_bank(m, ((cell >> 9) & 0x7f) << 4);
			uint32_t sig = mix32(mix32(mix32(2166136261u, cell), char_signature(character)),
				(uint32_t)pb << 16 | bank_ver[pb]) | 1u;
			if (p->sig[cy * 64 + cx] == sig)
				continue;
			p->sig[cy * 64 + cx] = sig;
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++)
				{
					const uint16_t raw = dl_plane_pixel(DL_TEXT, cx * 8 + x, cy * 8 + y);
					cellbuf[y * 8 + x] = raw ? argb(dl_color(m, raw)) : 0;
				}
			be->tex_write(p->tex, cx * 8, cy * 8, 8, 8, cellbuf, 8);
			st->cells_converted++;
		}
}

static void update_bitmap_plane(const dl_frame *f)
{
	plane_tex *p = PLANE(DL_BITMAP);
	const dl_mix *m = &f->layers[DL_BITMAP];
	static uint16_t rowbuf[512];
	const dl_plane *pl = &f->planes[DL_BITMAP];
	for (int y = 0; y < DL_HEIGHT; y++)
	{
		if (pl->span_count[y] == 0)
			continue;
		const int sy = pl->sy[y] & 0x1ff;
		/* the row's bytes: 8 bits a pixel 512 bytes at 512 * row; 4 bits 256 bytes at 256 * row */
		const uint32_t sig = mix32(mix32(mix32(dl_video_hash((uint32_t)sy * 512, 512), dl_video_hash((uint32_t)sy * 256, 256)), pal_epoch),
			(uint32_t)m->palette_base << 4 | m->shift) | 1u;
		if (p->sig[sy] == sig)
			continue;
		p->sig[sy] = sig;
		for (int x = 0; x < 512; x++)
		{
			const uint16_t raw = dl_plane_pixel(DL_BITMAP, x, sy);
			rowbuf[x] = raw ? argb(dl_color(m, raw)) : 0;
		}
		be->tex_write(p->tex, 0, sy, 512, 1, rowbuf, 512);
		st->cells_converted++;
	}
}

static bool bitmap_shows(const dl_frame *f)
{
	const dl_plane *pl = &f->planes[DL_BITMAP];
	for (int y = 0; y < DL_HEIGHT; y++)
		for (int i = 0; i < pl->span_count[y]; i++)
			for (int x = pl->spans[y][i][0]; x < pl->spans[y][i][1]; x++)
				if (dl_plane_pixel(DL_BITMAP, (pl->sx[y] + x) & 0x1ff, pl->sy[y]))
					return true;
	return false;
}

static void emit_plane(const dl_frame *f, int layer)
{
	const plane_tex *p = PLANE(layer);
#ifdef DL_CHECK
	if (getenv("RT_PLANE_LOG") && frame_no % 500 == 0)
		fprintf(stderr, "emit frame %u scr %d layer %d tex %d sx0 %d sy0 %d sy200 %d\n", frame_no, scr, layer, p->tex, f->planes[layer].sx[0], f->planes[layer].sy[0], f->planes[layer].sy[200]);
#endif
	const dl_plane *pl = &f->planes[layer];
	const float z = (float)f->layers[layer].priority * 1024.0f;
	int y = 0;
	while (y < DL_HEIGHT)
	{
		/* rows that continue each other (same column, next plane row, same spans) make one strip */
		int n = 1;
		while (y + n < DL_HEIGHT && pl->sx[y + n] == pl->sx[y] && pl->sy[y + n] == ((pl->sy[y] + n) & (p->rows * p->cell - 1)) &&
			pl->span_count[y + n] == pl->span_count[y] &&
			memcmp(pl->spans[y + n], pl->spans[y], sizeof(pl->spans[y][0]) * pl->span_count[y]) == 0)
			n++;
		for (int i = 0; i < pl->span_count[y]; i++)
		{
			const int x0 = pl->spans[y][i][0], x1 = pl->spans[y][i][1];
			uint32_t mod, add;
			offset_colours(f->layers[layer].offsets, &mod, &add);
			be->quad(p->tex, true, (float)x0, (float)y, (float)x1, (float)(y + n),
				(float)(pl->sx[y] + x0), (float)pl->sy[y], (float)(pl->sx[y] + x1), (float)(pl->sy[y] + n), z, mod, add);
			st->quads++;
		}
		y += n;
	}
}

/* ---- sprites ---- */

#define SP_SLOTS 2048
typedef struct sp_entry
{
	uint64_t key;
	int tex;
	uint16_t w, h;
	uint32_t last;
	int shadow_tex;      /* shadow pens, or -1 */
	uint16_t rows_done;  /* while still being built (a large texture converts over a few frames) */
	bool partial;
	bool dead;           /* evicted: probing goes on past it */
	bool pal4;           /* a 4-bit paletted texture: its colours come from a palette bank when drawn */
} sp_entry;

static sp_entry cache[SP_SLOTS];
/* textures are converted and sent CHUNK rows at a time */
#define CHUNK 16
static uint16_t spbuf[512 * CHUNK] __attribute__((aligned(32)));
static uint16_t shbuf[512 * CHUNK] __attribute__((aligned(32)));
static const uint16_t zero_rows[512 * CHUNK] __attribute__((aligned(32)));
static uint8_t idxbuf[512 * (CHUNK + 1)] __attribute__((aligned(32)));    /* (paletted: one index a texel) */

static uint64_t fnv64(uint64_t h, uint32_t v)
{
	for (int i = 0; i < 4; i++) { h ^= (v >> (8 * i)) & 0xff; h *= 1099511628211ull; }
	return h;
}

#ifdef RT_BUILD_US
static uint64_t build_start;    /* (the frame's start: plane updates count against the build budget too) */
static uint64_t build_limit = RT_BUILD_US;
static bool (*build_keep_going)(void);    /* (during rt_prepare) */
static uint64_t frame_build_limit = RT_BUILD_US;
void rt_set_build_limit(uint32_t us) { frame_build_limit = us; }
#else
void rt_set_build_limit(uint32_t us) { (void)us; }
#endif
rt_stats rt_prepare_stats;
static bool prepared_memo;
bool rt_pal_enabled = true;    /* (a test switch: off, new sprite textures are ARGB1555) */
static bool prepared;           /* rt_prepare has started this frame */

static int pow2(int v) { int p = 8; while (p < v) p <<= 1; return p; }

/* The graphic a sprite shows, whatever its colours: its texture key without the palette. While a
   sprite's texture for new colours (or a new palette version) is not ready, the same graphic's last
   texture is drawn instead of nothing */
#define FALLBACKS 2048
static struct { uint64_t graphic, key; uint16_t slot; } fallback[FALLBACKS];

static uint64_t graphic_key(const dl_sprite *s)
{
	uint64_t h = 1469598103934665603ull;
	h = fnv64(h, s->place);
	h = fnv64(h, (uint32_t)s->words << 16 | s->rows);
	h = fnv64(h, (uint32_t)s->bits8 | (uint32_t)s->indirect << 1 | (uint32_t)s->end_pen << 8);
	return h | 1;
}

static int sprite_group(const dl_frame *f, const dl_sprite *s)
{
	if (!s->indirect)
		return (s->color >> f->group_shift) & f->group_mask;
	for (int i = 0; i < 16; i++)
		if ((s->table[i] & s->transparent_bits) != s->transparent_bits)
			return (s->table[i] >> f->group_shift) & f->group_mask;
	return 0;
}

static uint64_t sprite_key(const dl_frame *f, const dl_sprite *s)
{
	uint64_t h = 14695981039346656037ull;
	h = fnv64(h, s->place);
	h = fnv64(h, (uint32_t)s->words << 16 | s->rows);
	h = fnv64(h, (uint32_t)s->bits8 | (uint32_t)s->indirect << 1 | (uint32_t)s->end_pen << 8);
	h = fnv64(h, (uint32_t)s->color << 16 | s->transparent_bits);
	h = fnv64(h, (uint32_t)f->group_shift << 24 | (uint32_t)f->group_mask << 16 | f->shadow_mask);
	for (int g = 0; g <= f->group_mask; g++)
		h = fnv64(h, (uint32_t)f->groups[g].palette_base << 4 | f->groups[g].shift);
	if (s->extra) h = fnv64(h, 0xe7a0u);
	/* the pixel values the sprite can produce, and their palette banks' versions */
	const int count = s->indirect ? 16 : (s->bits8 ? 16 : 1);
	for (int i = 0; i < count; i++)
	{
		const uint16_t v = s->indirect ? s->table[i] : (uint16_t)(s->color | (s->bits8 ? i << 4 : 1));
		const int g = (v >> f->group_shift) & f->group_mask;
		const int pb = pal_bank(&f->groups[g], v & f->sprite_bits);
		if (s->indirect) h = fnv64(h, v);
		h = fnv64(h, (uint32_t)pb << 16 | bank_ver[pb]);
		if (s->extra)
		{
			/* (the extra palette has no versions: its colours themselves) */
			const dl_mix *m = &f->groups[g];
			const int pixel = v & f->sprite_bits;
			for (int pen = 0; pen < 16; pen++)
			{
				uint16_t c = 0;
				const bool set = s32_video_extra_at(0, (uint32_t)(m->palette_base + ((pixel >> m->shift) & 0xfff0) + ((pixel + (s->indirect && !s->bits8 ? 0 : pen)) & 0x0f)), &c);
				h = fnv64(h, (uint32_t)set << 16 | c);
				if (s->indirect && !s->bits8) break;
			}
		}
	}
	return h | 1;
}

#define TEXEL_BUDGET (96u * 1024u)    /* sprite texels converted a frame, beyond the first texture */
static uint32_t texels_this_frame;

/* one pen value's texel and shadow texel, as the drawlist's pixel value converts */
static bool lut_extra;    /* (the sprite's colours from the extra palette where one is set) */
static void pen_texels(const dl_frame *f, uint16_t v, uint16_t *o, uint16_t *shadow)
{
	*o = 0; *shadow = 0;
	if ((v & 0x7fff) == 0x7fff)
		return;
	const int g = (v >> f->group_shift) & f->group_mask;
	const int pixel = v & f->sprite_bits;
	if ((pixel & 0x7ffe) == f->shadow_pen) { *shadow = 0x8000; return; }
	uint16_t c = dl_color(&f->groups[g], pixel);
	if (lut_extra)
	{
		const dl_mix *m = &f->groups[g];
		s32_video_extra_at(0, (uint32_t)(m->palette_base + ((pixel >> m->shift) & 0xfff0) + (pixel & 0x0f)), &c);
	}
	if (~v & f->shadow_mask)
		c = (uint16_t)((c >> 1) & 0x3def);    /* the shadow bit: each channel halved */
	*o = argb(c);
}

/* The sprite's texels straight from its packed words: each of its 16 or 256 pens is converted once, then
   every pixel is a table lookup (dl_sprite_decode_row's rules: pen 0 inside a word, the end pen at
   either end of it, a transparent table entry are clear; a word ending in the last pen ends the row) */
static uint16_t lut_out[256], lut_sh[256];

/* each pen's texel and shadow texel for the sprite's colours */
static void sprite_luts(const dl_frame *f, const dl_sprite *s)
{
	const int mask = s->bits8 ? 255 : 15;
	lut_extra = s->extra;
	for (int pen = 0; pen <= mask; pen++)
	{
		uint16_t v;
		if (!s->indirect)
			v = pen == 0 ? 0xffff : (uint16_t)(s->color | pen);
		else
		{
			v = s->bits8 ? (uint16_t)(s->table[pen >> 4] | (pen & 0x0f)) : s->table[pen];
			if ((v & s->transparent_bits) == s->transparent_bits) v = 0xffff;
		}
		pen_texels(f, v, &lut_out[pen], &lut_sh[pen]);
	}
}

static bool over_time(void)
{
#ifdef RT_BUILD_US
	if (build_keep_going && !build_keep_going()) return true;
	return be->clock_us && be->clock_us() - build_start > build_limit;
#else
	return false;
#endif
}

/* Frees the least recently used sprite texture, false when none may go: not one the last four frames
   used (the PowerVR may still be drawing them. While rt_prepare runs, before this frame has a list set,
   the scene four frames back can still be rendering: the two sets queued, the one before them in the
   TA waiting for it. Three was one short: on the console, unlike the emulators, a render takes long
   enough for a new sprite's texels to land in a texture still on screen, drawing pieces of the wrong
   sprite). The entry being built was used this frame, so it is never taken. */
static bool evict_lru(void)
{
	int victim = -1;
	for (int i = 0; i < SP_SLOTS; i++)
		if (cache[i].key && cache[i].last + 4 < frame_no && (victim < 0 || cache[i].last < cache[victim].last))
			victim = i;
	if (victim < 0) return false;
	be->tex_free(cache[victim].tex);
	if (cache[victim].shadow_tex >= 0) be->tex_free(cache[victim].shadow_tex);
	cache[victim].key = 0;
	cache[victim].dead = true;
	st->sprite_evictions++;
	return true;
}

/* Rows of the entry's texture from rows_done on, CHUNK at a time, until the sprite is finished (1), its
   graphics are still loading (-1), or the frame's time for builds is spent (0: it goes on next frame) */
static int build_rows(const dl_frame *f, const dl_sprite *s, sp_entry *e)
{
	const int bits = s->bits8 ? 8 : 4, per = 32 / bits, mask = (1 << bits) - 1;
	const int pens = s->words * per, w = e->w;
	uint32_t words[64];
	sprite_luts(f, s);
	while (e->rows_done < s->rows)
	{
		const int r0 = e->rows_done;
		int n = 0;
		uint16_t any_shadow = 0;
		bool loading = false;
		for (; n < CHUNK && r0 + n < s->rows; n++)
		{
			if (!dl_sprite_row_words(s, r0 + n, words)) { loading = true; break; }
			uint16_t *out = &spbuf[n * w];
			uint16_t *sh = &shbuf[n * w];
			uint8_t *ix = &idxbuf[n * w];    /* (paletted: the pen, 0 where clear; palette entry 0 is clear) */
			int i = 0;
			for (int k = 0; k < s->words; k++)
			{
				const uint32_t word = words[k];
				int pen = 0;
				for (int index = 0, shift = 32 - bits; index < per; index++, shift -= bits, i++)
				{
					pen = (int)(word >> shift) & mask;
					if (pen == ((index == 0 || index == per - 1) ? s->end_pen : 0)) { out[i] = 0; sh[i] = 0; ix[i] = 0; continue; }
					out[i] = lut_out[pen];
					sh[i] = lut_sh[pen];
					ix[i] = (uint8_t)pen;
					any_shadow |= sh[i];
				}
				if (s->end_pen != 0 && pen == mask) { k++; for (; k < s->words; k++) for (int m = 0; m < per; m++, i++) out[i] = 0, sh[i] = 0, ix[i] = 0; break; }
			}
			for (i = pens; i < w; i++) out[i] = 0, sh[i] = 0, ix[i] = 0;
		}
		/* (paletted textures take rows in pairs: an odd row waits for its partner, or at the sprite's
		   end is paired with a clear row, inside the texture as its height is a power of two) */
		int pal_rows = n;
		if (e->pal4 && (n & 1))
		{
			if (r0 + n < s->rows) n--, pal_rows = n;
			else { memset(&idxbuf[n * w], 0, (size_t)w); pal_rows = n + 1; }
		}
		if (n)
		{
			if (e->pal4) be->tex_write_pal4(e->tex, r0, pal_rows, idxbuf, w);
			else be->tex_write(e->tex, 0, r0, w, n, spbuf, w);
			if (any_shadow && e->shadow_tex < 0)
			{
				/* the first shadow pens: the shadow texture, clear above them (without room, left out) */
				e->shadow_tex = be->tex_alloc(w, e->h);
				while (e->shadow_tex < 0 && evict_lru()) e->shadow_tex = be->tex_alloc(w, e->h);
				if (e->shadow_tex < 0) st->texture_memory_failures++;
				else for (int y = 0; y < r0; y += CHUNK)
					be->tex_write(e->shadow_tex, 0, y, w, r0 - y < CHUNK ? r0 - y : CHUNK, zero_rows, w);
			}
			if (e->shadow_tex >= 0)
				be->tex_write(e->shadow_tex, 0, r0, w, n, shbuf, w);
			e->rows_done = (uint16_t)(r0 + n);
		}
		if (loading) return -1;    /* (tried again next frame) */
		if (e->rows_done < s->rows && over_time()) return 0;
	}
	return 1;
}

static int sprite_texture_built(const dl_frame *f, const dl_sprite *s, int *tw, int *th, int *shadow);

/* Palette banks for paletted sprite textures: one for each set of 16 colours in use, found by content;
   a bank is rewritten only once the last four frames have not used it (the PowerVR may still be drawing
   with it) */
typedef struct { uint64_t hash; uint32_t last; bool used; uint16_t col[16]; } pal_bank_t;
static pal_bank_t pal_banks[RT_PAL_BANKS];
static int sprite_bank;    /* the bank for the sprite sprite_texture last looked at, or -1 */

static int bank_for(const uint16_t col[16])
{
	uint64_t h = 1469598103934665603ull;
	for (int i = 0; i < 16; i += 2) h = fnv64(h, (uint32_t)col[i] | (uint32_t)col[i + 1] << 16);
	int victim = -1;
	for (int b = 0; b < RT_PAL_BANKS; b++)
	{
		pal_bank_t *k = &pal_banks[b];
		if (k->used && k->hash == h && memcmp(k->col, col, sizeof(k->col)) == 0) { k->last = frame_no; return b; }
		if (!k->used) { if (victim < 0 || pal_banks[victim].used) victim = b; }
		else if (k->last + 4 < frame_no && (victim < 0 || (pal_banks[victim].used && k->last < pal_banks[victim].last)))
			victim = b;
	}
	if (victim < 0) return -1;
	pal_bank_t *k = &pal_banks[victim];
	k->hash = h; k->last = frame_no; k->used = true;
	memcpy(k->col, col, sizeof(k->col));
	be->pal_set(victim, col);
	return victim;
}

/* a paletted texture's key: the graphic and which of its pens are shadow pens, not its colours */
static uint64_t pal_key(const dl_sprite *s, uint32_t shadow_pens)
{
	return fnv64(fnv64(graphic_key(s), shadow_pens), 0x9a14u) | 1;
}

static int last_slot;    /* the cache entry sprite_texture's texture is (for the object records) */

/* Previews. A large sprite's texture can take several frames to build on the console (in busy stretches
   the frame has about 1 ms for builds, against 4 ms or more in an emulator), and a new drawing (scenery
   switching to its next, larger drawing as it comes closer) was missing for those frames: the flicker of
   large distant scenery. While its texture is not ready, such a sprite is drawn from a preview: every
   2nd, 4th or 8th pen of every 2nd, 4th or 8th row, at most PREVIEW_TEXELS, built at once (a sixteenth of
   the work or less) and stretched over the sprite's place. Far scenery is mostly shrunk on screen, so the
   preview is close to the real thing for the frames it shows. */
#define PREVIEW_TEXELS 16384    /* (at most: half the pens and rows, never coarser) */
#define PREVIEW_MIN 1024           /* (texels: smaller sprites have none) */
#define PREVIEW_TEXELS_A_FRAME 16384
static int previews_this_frame;    /* (texels) */
static int preview_pens, preview_rows;    /* (set when sprite_texture returns a preview: its size in pens) */

bool rt_low_2p = true;            /* (test builds: X + D-pad up turns it off and on) */
bool rt_previews = true;          /* (test builds: X + A turns previews off and on) */
static int sprite_preview(const dl_frame *f, const dl_sprite *s, int *tw, int *th)
{
	if (!rt_previews)
		return -1;
	const int bits = s->bits8 ? 8 : 4, per = 32 / bits, mask = (1 << bits) - 1;
	const int pens = s->words * per;
	if (pens * s->rows <= PREVIEW_MIN || pens > 512)
		return -1;    /* (small: built whole within a frame) */
	const int r = 2;
	const int pw = (pens + r - 1) / r, ph = (s->rows + r - 1) / r;
	if (pw * ph > PREVIEW_TEXELS)
		return -1;
	const uint64_t key = (sprite_key(f, s) ^ (0x5052455649455700ull | (uint64_t)r)) | 1;
	unsigned slot = (unsigned)(key % SP_SLOTS);
	for (int probe = 0; probe < SP_SLOTS; probe++, slot = (slot + 1) % SP_SLOTS)
	{
		if (cache[slot].key == key)
		{
			cache[slot].last = frame_no;
			preview_pens = pw; preview_rows = ph;
			*tw = cache[slot].w; *th = cache[slot].h;
			last_slot = (int)slot;
			return cache[slot].tex;
		}
		if (cache[slot].key == 0 && !cache[slot].dead)
			break;
	}
	if (previews_this_frame + pw * ph > (rt_screens_now > 1 && rt_low_2p ? 2 * PREVIEW_TEXELS_A_FRAME : PREVIEW_TEXELS_A_FRAME))
		return -1;
	/* the rows it takes, all there? (graphics still loading: no preview either) */
	uint32_t words[64];
	for (int j = 0; j < ph; j++)
		if (!dl_sprite_row_words(s, j * r, words))
			return -1;
	const int w = pow2(pw), h = pow2(ph);
	int tex = be->tex_alloc(w, h);
	while (tex < 0)
	{
		if (!evict_lru()) { st->texture_memory_failures++; return -1; }
		tex = be->tex_alloc(w, h);
	}
	slot = (unsigned)(key % SP_SLOTS);
	int probe;
	for (probe = 0; probe < SP_SLOTS && cache[slot].key; probe++)
		slot = (slot + 1) % SP_SLOTS;
	if (probe == SP_SLOTS) { be->tex_free(tex); return -1; }
	previews_this_frame += pw * ph;
	sprite_luts(f, s);
	static uint16_t row_out[512];
	for (int j0 = 0; j0 < ph; j0 += CHUNK)
	{
		const int n = ph - j0 < CHUNK ? ph - j0 : CHUNK;
		for (int j = 0; j < n; j++)
		{
			(void)dl_sprite_row_words(s, (j0 + j) * r, words);
			int i = 0;
			for (int k = 0; k < s->words; k++)
			{
				const uint32_t word = words[k];
				int pen = 0;
				for (int index = 0, shift = 32 - bits; index < per; index++, shift -= bits, i++)
				{
					pen = (int)(word >> shift) & mask;
					row_out[i] = pen == ((index == 0 || index == per - 1) ? s->end_pen : 0) ? 0 : lut_out[pen];
				}
				if (s->end_pen != 0 && pen == mask) { for (i = (k + 1) * per; i < pens; i++) row_out[i] = 0; break; }
			}
			uint16_t *out = &spbuf[j * w];
			for (int x = 0; x < w; x++)
				out[x] = x < pw ? row_out[x * r] : 0;
		}
		be->tex_write(tex, 0, j0, w, n, spbuf, w);
	}
	sp_entry *e = &cache[slot];
	e->key = key; e->dead = false; e->tex = tex; e->w = (uint16_t)w; e->h = (uint16_t)h;
	e->shadow_tex = -1; e->rows_done = (uint16_t)ph; e->partial = false; e->last = frame_no; e->pal4 = false;
	st->sprites_preview++;
	preview_pens = pw; preview_rows = ph;
	*tw = w; *th = h;
	last_slot = (int)slot;
	return tex;
}

/* 2 PLAYERS: the work of two races in one frame. Large scenery is drawn from its half-resolution copy (the
   preview) and its full texture never built: a quarter of the texels to convert, upload and keep in video
   memory, with two screens' scenery sharing it. */
#define LOW_MIN 4096                /* (texels: smaller sprites, the cars among them, are built whole) */
static bool low_res(const dl_sprite *s)
{
	if (!rt_low_2p || rt_screens_now < 2 || s->ghost || s->shadow) return false;
	const int pens = s->words * (s->bits8 ? 4 : 8);
	return pens * s->rows > LOW_MIN && pens <= 512 && ((pens + 1) / 2) * ((s->rows + 1) / 2) <= PREVIEW_TEXELS;
}

static int sprite_texture(const dl_frame *f, const dl_sprite *s, int *tw, int *th, int *shadow)
{
	if (low_res(s))
	{
		const int t = sprite_preview(f, s, tw, th);
		if (t >= 0) { *shadow = -1; return t; }
	}
	const int tex = sprite_texture_built(f, s, tw, th, shadow);
	if (tex >= 0)
		return tex;
	const uint64_t g = graphic_key(s);
	const unsigned i = (unsigned)(g % FALLBACKS);
	if (fallback[i].graphic == g)
	{
		sp_entry *e = &cache[fallback[i].slot];
		if (e->key == fallback[i].key && !e->partial && (!e->pal4 || sprite_bank >= 0))
		{
			e->last = frame_no;
			*tw = e->w; *th = e->h;
			*shadow = e->shadow_tex;
			st->sprites_fallback++;
			last_slot = (int)(e - cache);
			return e->tex;
		}
	}
	{
		const int t = sprite_preview(f, s, tw, th);
		if (t >= 0) { *shadow = -1; return t; }
	}
	st->sprites_missing++;
	return -1;
}

static int sprite_texture_built(const dl_frame *f, const dl_sprite *s, int *tw, int *th, int *shadow)
{
	/* 4-bit sprites whose pen 0 is clear get a paletted texture, when a bank is free for their colours
	   (else, and for 8-bit sprites, a texture in their colours) */
	uint64_t key = 0;
	int bank = -1;
	if (be->tex_alloc_pal4 && rt_pal_enabled && !s->bits8)
	{
		sprite_luts(f, s);
		if (lut_out[0] == 0 && lut_sh[0] == 0)
		{
			uint32_t shadow_pens = 0;
			for (int p = 0; p < 16; p++) if (lut_sh[p]) shadow_pens |= 1u << p;
			bank = bank_for(lut_out);
			if (bank >= 0) key = pal_key(s, shadow_pens);
			else st->pal_banks_full++;
		}
	}
	sprite_bank = bank;
	if (bank < 0) key = sprite_key(f, s);
	const int w = pow2(s->words * (s->bits8 ? 4 : 8)), h = pow2(s->rows);
	const uint32_t texels = (uint32_t)(s->words * (s->bits8 ? 4 : 8)) * s->rows;
	sp_entry *e = NULL;
	unsigned slot = (unsigned)(key % SP_SLOTS);
	for (int probe = 0; probe < SP_SLOTS; probe++, slot = (slot + 1) % SP_SLOTS)
	{
		if (cache[slot].key == key)
		{
			cache[slot].last = frame_no;
			if (cache[slot].partial) { e = &cache[slot]; break; }
			*tw = cache[slot].w; *th = cache[slot].h;
			*shadow = cache[slot].shadow_tex;
			st->sprite_cache_hits++;
			const uint64_t g = graphic_key(s);
			fallback[g % FALLBACKS].graphic = g; fallback[g % FALLBACKS].key = key; fallback[g % FALLBACKS].slot = (uint16_t)slot;
			last_slot = (int)slot;
			return cache[slot].tex;
		}
		if (cache[slot].key == 0 && !cache[slot].dead)
			break;
	}
	/* a texture built this frame within the budgets (scene changes spread over a few frames; on the
	   console a large one over a few frames too), from graphics already loaded (else asked for, and the
	   sprite waits) */
#ifdef RT_BUILD_US
	if (texels_this_frame > 0 && over_time()) { st->sprites_deferred++; return -1; }    /* (the console: time alone) */
#else
	if (texels_this_frame > 0 && texels_this_frame + texels > TEXEL_BUDGET) { st->sprites_deferred++; return -1; }
#endif
	if (!e)
	{
		/* a free slot for the new texture (evicting the least recently used when full) */
		int tex = bank >= 0 ? be->tex_alloc_pal4(w, h) : be->tex_alloc(w, h);
		while (tex < 0)
		{
			if (!evict_lru()) { st->texture_memory_failures++; return -1; }
			tex = bank >= 0 ? be->tex_alloc_pal4(w, h) : be->tex_alloc(w, h);
		}
		/* the first free or evicted slot on the key's chain */
		slot = (unsigned)(key % SP_SLOTS);
		int probe;
		for (probe = 0; probe < SP_SLOTS && cache[slot].key; probe++)
			slot = (slot + 1) % SP_SLOTS;
		if (probe == SP_SLOTS) { be->tex_free(tex); st->texture_memory_failures++; return -1; }
		e = &cache[slot];
		{
			const uint64_t g = graphic_key(s);
			if (fallback[g % FALLBACKS].graphic == g) st->built_recolour++; else st->built_new++;
		}
		e->key = key; e->dead = false; e->tex = tex; e->w = (uint16_t)w; e->h = (uint16_t)h;
		e->shadow_tex = -1; e->rows_done = 0; e->partial = true; e->last = frame_no;
#ifdef DL_CHECK
		if (getenv("RT_LOG_BUILDS") && (int)texels >= atoi(getenv("RT_LOG_BUILDS")))
			fprintf(stderr, "B %u place %06x src %dx%d bits8 %d ind %d pal %d key %016llx\n", frame_no, s->place,
				s->words * (s->bits8 ? 4 : 8), s->rows, s->bits8, s->indirect, bank >= 0, (unsigned long long)key);
#endif
		e->pal4 = bank >= 0;
	}
	const uint64_t b0 = be->clock_us ? be->clock_us() : 0;
	const int done = build_rows(f, s, e);
	if (be->clock_us) st->build_us_total += (uint32_t)(be->clock_us() - b0);
	if (done >= 0) texels_this_frame += texels;
	if (done < 0) { st->sprites_pending++; return -1; }
	if (done == 0) { st->sprites_deferred++; return -1; }
	e->partial = false;
	st->sprite_textures_built++;
	{
		const uint64_t g = graphic_key(s);
		fallback[g % FALLBACKS].graphic = g; fallback[g % FALLBACKS].key = key;
		fallback[g % FALLBACKS].slot = (uint16_t)(e - cache);
	}
	last_slot = (int)(e - cache);
	if (be->clock_us)
	{
		const uint32_t us = (uint32_t)(be->clock_us() - b0);
		if (us > st->build_us_max) { st->build_us_max = us; st->build_texels_max = texels; }
	}
	*tw = w; *th = h;
	*shadow = e->shadow_tex;
	return e->tex;
}


/* the visible part [x0,x1) x [y0,y1) of a sprite, as a quad with its texture coordinates */
typedef struct { const dl_sprite *s; int tex; int x0, y0, x1, y1; float z; } shadow_quad;
static shadow_quad shadows[1024];
static int shadow_count;
static uint32_t sprite_mod, sprite_add;

static int quad_bank = -1;    /* the palette bank of the sprite being emitted, when its texture is paletted */
static bool quad_ghost;      /* the sprite being emitted is half see-through */

static void sprite_quad(const dl_sprite *s, int tex, int x0, int y0, int x1, int y1, float z, int shadow_tex)
{
	if (x0 >= x1 || y0 >= y1) return;
	const float xz = (float)s->x_zoom / 65536.0f, yz = (float)s->y_zoom / 65536.0f;
	/* a pixel centre (x + 0.5) samples pen floor(k * zoom), k its distance from the first pixel, as the
	   board's sprite drawing picks it; clamping covers the half pixel before the first centre */
	const float u0 = s->flip_x ? ((float)s->x + 0.5f - (float)x0) * xz : ((float)x0 - 0.5f - (float)s->x) * xz;
	const float u1 = s->flip_x ? ((float)s->x + 0.5f - (float)x1) * xz : ((float)x1 - 0.5f - (float)s->x) * xz;
	const float v0 = s->flip_y ? ((float)s->y + 0.5f - (float)y0) * yz : ((float)y0 - 0.5f - (float)s->y) * yz;
	const float v1 = s->flip_y ? ((float)s->y + 0.5f - (float)y1) * yz : ((float)y1 - 0.5f - (float)s->y) * yz;
	/* Samples are moved off the texel edges, where the pen floor(k * zoom) puts them (every sample of an
	   unzoomed sprite lands exactly on one), towards the texel's middle: half a texel unzoomed or
	   shrunk, half a step when enlarged. The first bias was 1/256 texel, enough for an emulator's exact
	   arithmetic; the console's PowerVR interpolates with less precision, and its error, which changes
	   with the quad's place and depth, pushed samples over the edge to the neighbouring texel in bands:
	   the sprites' "warping", only on the console. (RT_EDGE_BIAS: the old bias, for the host check) */
#ifdef RT_EDGE_BIAS
	const float bias_u = 1.0f / 256.0f, bias_v = 1.0f / 256.0f;
#else
	const float bias_u = 0.5f * (xz < 1.0f ? xz : 1.0f), bias_v = 0.5f * (yz < 1.0f ? yz : 1.0f);
#endif
#ifdef DL_CHECK
	{
		static int dx = -2, dy;
		if (dx == -2) { dx = -1; if (getenv("RT_DEBUG_PIXEL")) sscanf(getenv("RT_DEBUG_PIXEL"), "%d,%d", &dx, &dy); }
		if (tex >= 0 && dx >= x0 && dx < x1 && dy >= y0 && dy < y1)
			fprintf(stderr, "sprite x %d y %d w %d h %d xz %d yz %d flip %d%d words %d rows %d bits8 %d -> expected pen col %lld row %lld\n",
				s->x, s->y, s->width, s->height, s->x_zoom, s->y_zoom, s->flip_x, s->flip_y, s->words, s->rows, s->bits8,
				(long long)(s->flip_x ? s->x - dx : dx - s->x) * s->x_zoom >> 16, (long long)(s->flip_y ? s->y - dy : dy - s->y) * s->y_zoom >> 16);
	}
#endif
	if (tex >= 0 && quad_ghost && be->quad_alpha)
		be->quad_alpha(tex, quad_bank, (float)x0, (float)y0, (float)x1, (float)y1, u0 + bias_u, v0 + bias_v, u1 + bias_u, v1 + bias_v, z,
			sprite_mod, sprite_add, 0x80);
	else if (tex >= 0 && quad_bank >= 0)
		be->quad_pal(tex, quad_bank, (float)x0, (float)y0, (float)x1, (float)y1, u0 + bias_u, v0 + bias_v, u1 + bias_u, v1 + bias_v, z,
			sprite_mod, sprite_add);
	else if (tex >= 0)
		be->quad(tex, false, (float)x0, (float)y0, (float)x1, (float)y1, u0 + bias_u, v0 + bias_v, u1 + bias_u, v1 + bias_v, z,
			sprite_mod, sprite_add);
	else
		be->shadow(shadow_tex, (float)x0, (float)y0, (float)x1, (float)y1, u0 + bias_u, v0 + bias_v, u1 + bias_u, v1 + bias_v, z);
	if (tex >= 0 && shadow_tex >= 0 && shadow_count < 1024)
		shadows[shadow_count++] = (shadow_quad){ s, shadow_tex, x0, y0, x1, y1, z };
	st->quads++;
}

#define RT_OVERLAY_Z 400000.0f    /* (over every depth band: 127 priorities of 1024) */

/* What build_pass found for each sprite of the frame's list, so emit_sprites does not look each one up
   again (colours, palette bank, key, cache: about half the sprites' time on the console) */
static struct { uint32_t frame; int tex, shadow, bank, slot; uint16_t w, h; } memo[2][DL_MAX_SPRITES + 128];

static void emit_sprites(const dl_frame *f)
{
	shadow_count = 0;
	float top_z = 0.0f;    /* (the sprite groups' highest depth band) */
	for (int g = 0; g <= f->group_mask; g++)
		if ((float)f->groups[g].priority * 1024.0f > top_z) top_z = (float)f->groups[g].priority * 1024.0f;
	/* a port's overlay: the frame dimmed, its sprites over everything */
	if (f->overlay_from >= 0 && be->tint)
		be->tint(0.0f, 0.0f, (float)DL_WIDTH, (float)DL_HEIGHT, RT_OVERLAY_Z - 1024.0f, 0x80000000u);
	for (int i = 0; i < f->sprite_count && i < DL_MAX_SPRITES + 127; i++)
	{
		const dl_sprite *s = &f->sprites[i];
		int l = s->flip_x ? s->x - s->width + 1 : s->x, r = l + s->width;
		int t = s->flip_y ? s->y - s->height + 1 : s->y, b = t + s->height;
		if (l < s->clip_left) l = s->clip_left;
		if (r > s->clip_right + 1) r = s->clip_right + 1;
		if (t < s->clip_top) t = s->clip_top;
		if (b > s->clip_bottom + 1) b = s->clip_bottom + 1;
		if (s->shadow)
		{
			/* A shadow sprite darkens what the list drew before it. One of a single word stretched over its
			   rectangle (the console menus' darkened box) is drawn as that: the rectangle, half black, over
			   the sprites before it whatever their group. Shaped ones are not drawn. */
			if (be->tint && f->shadow_mask && s->indirect && s->words <= 2 && s->rows == 1 && l < r && t < b)
				be->tint((float)l, (float)t, (float)r, (float)b, top_z + 1.0f + (float)i, 0x80000000u);
			else
			{
#ifdef DL_CHECK
				if (getenv("RT_DEBUG_SHADOW")) fprintf(stderr, "shadow sprite skipped: mask %x indirect %d words %d rows %d box %d,%d-%d,%d\n", f->shadow_mask, s->indirect, s->words, s->rows, l, t, r, b);
#endif
				st->skipped++;
			}
			continue;
		}
		if (l >= r || t >= b) continue;
		int tw, th, sh;
		int tex;
		if (memo[scr][i].frame == frame_no && prepared_memo)
		{
			tex = memo[scr][i].tex; sh = memo[scr][i].shadow; tw = memo[scr][i].w; th = memo[scr][i].h;
			sprite_bank = memo[scr][i].bank; last_slot = memo[scr][i].slot;
			st->sprite_cache_hits++;
		}
		else
		{
			preview_pens = 0;
			tex = sprite_texture(f, s, &tw, &th, &sh);
		}
		dl_sprite stand_in;
		if (tex >= 0 && preview_pens)
		{
			/* a preview: its pens stretched over the sprite's place */
			stand_in = *s;
			stand_in.x_zoom = (int32_t)(((int64_t)preview_pens << 16) / s->width);
			stand_in.y_zoom = (int32_t)(((int64_t)preview_rows << 16) / s->height);
			stand_in.rows = (uint16_t)preview_rows;
			s = &stand_in;
			preview_pens = 0;
		}
		if (tex < 0)
		{
			/* (no stand-in: a large sprite has a preview, sprite_preview) */
			{
				st->missing_px += (uint32_t)((r - l) * (b - t));
#ifdef DL_CHECK
				if (getenv("RT_LOG_MISSING") && (r - l) * (b - t) >= atoi(getenv("RT_LOG_MISSING")))
					fprintf(stderr, "M %u #%d place %06x %dx%d src %dx%d bits8 %d ind %d scr %d,%d-%d,%d\n", frame_no, i, s->place,
						s->width, s->height, s->words * (s->bits8 ? 4 : 8), s->rows, s->bits8, s->indirect, l, t, r, b);
#endif
				continue;
			}
		}
		quad_bank = cache[last_slot].pal4 ? sprite_bank : -1;
		quad_ghost = s->ghost;
		if (quad_bank >= 0) st->pal_sprites++;
		offset_colours(f->groups[sprite_group(f, s)].offsets, &sprite_mod, &sprite_add);
		const float z = f->overlay_from >= 0 && i >= f->overlay_from ? RT_OVERLAY_Z + (float)i
			: (float)f->groups[sprite_group(f, s)].priority * 1024.0f + 1.0f + (float)i;
		/* the exclusion window cuts the rows it covers */
		const int ol = s->out_left, orr = s->out_right + 1, ot = s->out_top, ob = s->out_bottom + 1;
		if (orr <= ol || ob <= ot || ol >= r || orr <= l || ot >= b || ob <= t)
		{
			sprite_quad(s, tex, l, t, r, b, z, sh);
			continue;
		}
		const int mt = ot > t ? ot : t, mb = ob < b ? ob : b;
		sprite_quad(s, tex, l, t, r, mt, z, sh);            /* above */
		sprite_quad(s, tex, l, mb, r, b, z, sh);            /* below */
		sprite_quad(s, tex, l, mt, ol > l ? ol : l, mb, z, sh);        /* left */
		sprite_quad(s, tex, orr < r ? orr : r, mt, r, mb, z, sh);      /* right */
	}
}

/* The frame's new sprite textures, built before the sprites are emitted, from the end of the list
   back: the last sprites are drawn in front (the player's car, its drivers, the lettering), and when
   the time runs out it is the far scenery at the start of the list that waits a frame. (In list order
   the console, which often has time for one or two builds a frame, never reached the car while new
   scenery kept arriving: it flickered.) */
static void build_pass(const dl_frame *f, const dl_sprite *list, int count)
{
	const bool shown = list == f->sprites;
	for (int i = (count < DL_MAX_SPRITES + 127 ? count : DL_MAX_SPRITES + 127) - 1; i >= 0; i--)
	{
		const dl_sprite *s = &list[i];
		if (s->shadow || s->width <= 0 || s->height <= 0) continue;
		int l = s->flip_x ? s->x - s->width + 1 : s->x, r = l + s->width;
		int t = s->flip_y ? s->y - s->height + 1 : s->y, b = t + s->height;
		if (l < s->clip_left) l = s->clip_left;
		if (r > s->clip_right + 1) r = s->clip_right + 1;
		if (t < s->clip_top) t = s->clip_top;
		if (b > s->clip_bottom + 1) b = s->clip_bottom + 1;
		if (l >= r || t >= b) continue;
		if (over_time()) break;
		if (low_res(s)) continue;    /* (2 PLAYERS: drawn from its preview, built when drawn) */
		int tw, th, sh;
		const int tex = sprite_texture_built(f, s, &tw, &th, &sh);
		if (shown && tex >= 0)
		{
			memo[scr][i].frame = frame_no; memo[scr][i].tex = tex; memo[scr][i].shadow = sh; memo[scr][i].bank = sprite_bank;
			memo[scr][i].slot = last_slot; memo[scr][i].w = (uint16_t)tw; memo[scr][i].h = (uint16_t)th;
		}
	}
}

/* ---- the frame ---- */

void rt_init(const rt_backend *backend)
{
	be = backend;
	for (int b = 0; b < PLANE_SETS; b++)
		for (int sc = 0; sc < 2; sc++)
			for (int i = 0; i < DL_SPRITES; i++) { plane_sets[b][sc][i].tex = -1; plane_sets[b][sc][i].w = 0; }
	memset(seen, 0, sizeof(seen));
	if (!tile_cache) { tile_cache = memalign(32, TILE_CACHE * sizeof(tile_entry)); if (tile_cache) memset(tile_cache, 0, TILE_CACHE * sizeof(tile_entry)); }
	memset(cache, 0, sizeof(cache));
	memset(fallback, 0, sizeof(fallback));
	memset(pal_banks, 0, sizeof(pal_banks));
	pal_primed[0] = pal_primed[1] = false;
}

static void view(int screen)
{
	scr = screen;
	if (be->view) be->view(screen);
}

void rt_prepare_n(const dl_frame *const *fs, int count, bool (*keep_going)(void), uint32_t limit_us)
{
	rt_screens_now = count;
	st = &rt_prepare_stats;    /* (counted apart from rt_frame's) */
	memset(st, 0, sizeof(*st));
	frame_no++;
	prepared = true;
	texels_this_frame = 0;
	previews_this_frame = 0;
#ifdef RT_BUILD_US
	build_start = be->clock_us ? be->clock_us() : 0;
	build_limit = limit_us;
	build_keep_going = keep_going;
#else
	(void)keep_going; (void)limit_us;
#endif
	for (int sc = 0; sc < count; sc++)
		scan_palette(sc);
	for (int sc = 0; sc < count; sc++)
	{
		const dl_frame *f = fs[sc];
		if (!f->display_on)
			continue;
		scr = sc;
		cur = f;
		build_pass(f, f->sprites, f->sprite_count);
	}
	for (int sc = 0; sc < count; sc++)
	{
		const dl_frame *f = fs[sc];
		if (!f->display_on)
			continue;
		scr = sc;
		cur = f;
		build_pass(f, f->next_sprites, f->next_sprite_count);
	}
	scr = 0;
#ifdef RT_BUILD_US
	build_keep_going = NULL;
#endif
}

void rt_prepare(const dl_frame *f, bool (*keep_going)(void), uint32_t limit_us)
{
	rt_prepare_n(&f, 1, keep_going, limit_us);
}

void rt_frame_n(const dl_frame *const *fs, int count, rt_stats *stats)
{
	rt_screens_now = count;
	st = stats;
	memset(st, 0, sizeof(*st));
	const bool was_prepared = prepared;
	prepared = false;
	if (!was_prepared) frame_no++;
#ifdef RT_BUILD_US
	build_keep_going = NULL;
	build_limit = frame_build_limit;
#endif
	texels_this_frame = 0;
	previews_this_frame = 0;
#ifdef RT_BUILD_US
	build_start = be->clock_us ? be->clock_us() : 0;
#endif
	plane_set = (int)(frame_no % PLANE_SETS);
	uint64_t t = be->clock_us ? be->clock_us() : 0, t2;
#define PHASE(n) do { if (be->clock_us) { t2 = be->clock_us(); st->phase_us[n] = (uint32_t)(t2 - t); t = t2; } } while (0)
	if (!was_prepared)
		for (int sc = 0; sc < count; sc++)
			scan_palette(sc);
	PHASE(0);
	plane_cells_left = PLANE_CELLS_A_FRAME;
	plane_new_left = PLANE_NEW_CELLS;
	plane_deadline = be->clock_us ? be->clock_us() + PLANE_US_A_FRAME : 0;
	plane_cells_late = 0;
	for (int k = 0; k < count; k++)
	{
		const int sc = (k + (int)(frame_no & 1)) % count;    /* (2 PLAYERS: each screen first every other frame) */
		const dl_frame *f = fs[sc];
		if (!f->display_on)
			continue;
		scr = sc;
		cur = f;
		for (int layer = DL_TEXT; layer <= DL_BITMAP; layer++)
		{
			if (!f->layers[layer].on)
				continue;
			/* (the bitmap layer gets texture memory only once it shows a pixel: OutRunners keeps it clipped
			   and empty) */
			if (layer == DL_BITMAP && !PLANE(layer)->w && !bitmap_shows(f))
				continue;
			if (!plane_ready(layer))
				continue;
			if (layer == DL_TEXT) update_text_plane(f);
			else if (layer == DL_BITMAP) update_bitmap_plane(f);
			else update_tile_plane(f, layer);
		}
	}
	PHASE(1);
	for (int sc = 0; sc < count; sc++)
	{
		const dl_frame *f = fs[sc];
		view(sc);
		cur = f;
		if (!f->display_on)
		{
			be->solid(0x8000, 0, 0, DL_WIDTH, DL_HEIGHT, 1.0f, 0xffffffffu, 0xff000000u);
			continue;
		}
		/* the backdrop, rows of one colour together */
		const dl_mix *m = &f->layers[DL_BACKDROP];
		const float z = (float)m->priority * 1024.0f;
		int y = 0;
		while (y < DL_HEIGHT)
		{
			int n = 1;
			while (y + n < DL_HEIGHT && f->backdrop[y + n] == f->backdrop[y]) n++;
			uint32_t mod, add;
			offset_colours(m->offsets, &mod, &add);
			be->solid(argb(dl_color(m, f->backdrop[y])), 0, (float)y, DL_WIDTH, (float)(y + n), z, mod, add);
			y += n;
		}
		for (int layer = DL_TEXT; layer <= DL_BITMAP; layer++)
			if (f->layers[layer].on && PLANE(layer)->w)
				emit_plane(f, layer);
	}
	PHASE(2);
#ifdef RT_BUILD_US
	/* (the builds' time counts from here: the planes' own work, several milliseconds on the console,
	   used to spend it before the first sprite was looked at, leaving one build a frame) */
	build_start = be->clock_us ? be->clock_us() : 0;
#endif
	for (int k = 0; k < count; k++)
	{
		const int sc = (k + (int)(frame_no & 1)) % count;
		if (fs[sc]->display_on)
		{
			scr = sc;
			cur = fs[sc];
			build_pass(fs[sc], fs[sc]->sprites, fs[sc]->sprite_count);
		}
	}
	prepared_memo = true;    /* (emit_sprites may use the memo: entries of this frame, rt_prepare's included) */
	/* then, with what time is left, the next frame's: a sprite's first frame is mostly built before it
	   is shown, instead of the sprite missing for the frame its build takes. Not when the frame is
	   already late (the build time at its least): looking the list up costs a millisecond by itself. */
#ifdef RT_BUILD_US
	if (build_limit > 1500)
#endif
	for (int sc = 0; sc < count; sc++)
		if (fs[sc]->display_on)
		{
			scr = sc;
			cur = fs[sc];
			build_pass(fs[sc], fs[sc]->next_sprites, fs[sc]->next_sprite_count);
		}
	for (int sc = 0; sc < count; sc++)
	{
		const dl_frame *f = fs[sc];
		if (!f->display_on)
			continue;
		view(sc);
		cur = f;
		emit_sprites(f);
		quad_bank = -1;
		quad_ghost = false;
		/* the shadow pens, translucent, after everything they darken */
		for (int i = 0; i < shadow_count; i++)
			sprite_quad(shadows[i].s, -1, shadows[i].x0, shadows[i].y0, shadows[i].x1, shadows[i].y1, shadows[i].z, shadows[i].tex);
	}
	prepared_memo = false;
	view(0);
	PHASE(3);
	PHASE(4);
	st->plane_cells_late = plane_cells_late;
#undef PHASE
}

void rt_frame(const dl_frame *f, rt_stats *stats)
{
	rt_frame_n(&f, 1, stats);
}
