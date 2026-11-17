/* SPDX-License-Identifier: BSD-3-Clause */
/* The NAOMI's side of the Dreamcast port: naomi-kit's start (the console into a ring of lines the Raspberry
   reads, the NetDIMM's files at /dimm, its peeks), the live block (the port's own timings and the PowerVR's
   figures, copied once a second: live.py ADDR words), START as the coin button, and the BIOS font the NAOMI has
   not (its texts are left out: the console's log has them). */
#include "dc_port.h"
#include <kos.h>
#include <s32/machine.h>
#include <nk/naomi_kit.h>
#include <stdint.h>
#include <stdio.h>
#include <dc/biosfont.h>

/* ---- the live block ---- */
#define LIVE_NAMES \
	"frames_shown board_frames frame_avg_us worst_sound_us worst_video_us worst_frame_us worst_wait_us " \
	"worst_board_us pvr_wait_avg_us pvr_work_avg_us quads cells_converted sprites_built fallbacks missing " \
	"texture_failures overflows vram_free phase0_max_us phase1_max_us phase2_max_us phase3_max_us stat12 " \
	"unpack_max_us stat14 unaligned heap_free frames_skipped sound_us"
#define LIVE_COUNT 29

typedef struct
{
	nk_live_head head;
	uint32_t words[LIVE_COUNT];
} naomi_live;

volatile naomi_live dc_naomi_live;

extern volatile uint32_t dc_frame_count, dc_time_avg_us[8], dc_pvr_stats[16], dc_frames_skipped;
extern volatile uint32_t dc_sound_us_total;

static void every_second(void)
{
	static uint32_t last_frames, last_board, last_sound;
	volatile uint32_t *w = dc_naomi_live.words;
	const uint32_t frames = dc_frame_count, board = (uint32_t)s32_board.frame, sound = dc_sound_us_total;

	w[0] = frames - last_frames;
	w[1] = board - last_board;
	for (int i = 0; i < 8; i++)
		w[2 + i] = dc_time_avg_us[i];
	for (int i = 0; i < 16; i++)
		w[10 + i] = dc_pvr_stats[i];
	w[26] = nk_heap_left();
	w[27] = dc_frames_skipped;
	w[28] = w[0] ? (sound - last_sound) / w[0] : 0;
	last_frames = frames;
	last_board = board;
	last_sound = sound;
	dc_naomi_live.head.frames_alive = frames;
}

void dc_naomi_boot(void)
{
	static const nk_config config = {
		.title = "OUTRUNNERS (NAOMI, DREAMCAST PORT)",
		.live = &dc_naomi_live.head,
		.game = "ORDC",
		.names = LIVE_NAMES,
		.count = LIVE_COUNT,
		.need_dimm = true,
		.hang_first_s = 0, .hang_s = 0,    /* (the port has its own watchdog: diag_screen.c) */
		.misaligned = false,               /* (and its own misaligned accesses: fault_record.c) */
		.every_second = every_second };

	nk_boot(&config);
	nk_cabinet_start_coin(6);
}

/* ---- no BIOS font on a NAOMI: the texts go to the console's log instead ---- */
uint32_t bfont_set_foreground_color(uint32_t colour) { (void)colour; return 0; }
uint32_t bfont_set_background_color(uint32_t colour) { (void)colour; return 0; }
size_t bfont_draw(void *buffer, uint32_t width, bool opaque, uint32_t c) { (void)buffer; (void)width; (void)opaque; (void)c; return 0; }
void bfont_draw_str(void *buffer, uint32_t width, bool opaque, const char *text)
{
	(void)buffer; (void)width; (void)opaque;
	printf("NAOMI: [screen] %s\n", text);
}
