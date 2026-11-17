/* SPDX-License-Identifier: BSD-3-Clause */
/* The cabinet's controls (a Crazy Taxi upright's JVS, read by naomi-kit) as the arcade board's, in place of the
   Dreamcast port's pad (../dreamcast/input.c): the wheel, the two pedals, the shifter (forward: HIGH, back: LOW),
   START, SERVICE, TEST and the coins. START is the coin button too (nk_cabinet_start_coin, naomi_glue.c): a
   press puts a coin in, the release is START. */
#include "dc_port.h"
#include <s32/machine.h>
#include <nk/naomi_kit.h>
#include <string.h>

/* the board's wheel as MAME has it: 0x10 full left, 0x80 centre, 0xf0 full right */
static uint8_t board_wheel(int steer)
{
	return (uint8_t)(steer < 0 ? 0x80 + steer * (0x80 - 0x10) / 128 : 0x80 + steer * (0xf0 - 0x80) / 127);
}

static int coin_frames;

void dc_controls_update(void)
{
	s32_inputs *input = &s32_board.input;
	nk_cabinet cabinet;
	bool coin = false;

	nk_cabinet_read(&cabinet);
	if (cabinet.coins > 0)
		coin_frames = 4;    /* (the board counts a coin from a pulse of a few frames) */
	if (coin_frames > 0)
	{
		coin = true;
		coin_frames--;
	}
	memset(input, 0xff, sizeof(*input));    /* (the 315-5296s' ports are active low) */
	memset(input->analog, 0, sizeof(input->analog));
	input->analog[0] = board_wheel(cabinet.steer);
	input->analog[1] = cabinet.gas;
	input->analog[2] = cabinet.brake;
	input->analog[3] = 0x80;
	if (cabinet.forward) input->port_a[0] &= ~0x01;
	if (cabinet.back) input->port_a[0] &= ~0x02;
	if (cabinet.service) input->port_e[0] &= ~0x01;
	if (cabinet.test) input->port_e[0] &= ~0x02;
	if (coin) input->port_e[0] &= ~0x04;
	if (cabinet.start && !coin) input->port_e[0] &= ~0x10;
}

bool dc_exit_requested(void)
{
	return false;
}

void dc_exit_clear(void)
{
}

/* (the console build's; not in this one) */
uint32_t dc_menu_actions(void)
{
	return 0;
}

bool dc_pause_wanted(void)
{
	return false;
}
