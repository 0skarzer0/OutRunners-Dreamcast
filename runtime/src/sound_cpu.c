/**
 *    Name:        sound_cpu.c
 *    Description: runs the decompiled sound program on the board's time: the Z80's timing model
 *    Version:     0.1.0
 *    Created:     13-10-2026
 *    Modified:    13-11-2026
 *    Author:      Sebastian Toyos
 *
 *    license:BSD-3-Clause
 *    The sound program is C (src/sound/, tools/decompile_sound.py) and keeps the Z80's time itself: every
 *    basic block of the original starts with z80_block(cycles). The rules, the same tools/core/system32/reference/sound
 *    keeps for MAME's Z80 so that the two compare sample for sample:
 *      - the Z80's time counts in its own cycles and moves on by each block's cycles (tools/z80/timing.py),
 *        the extra cycles of a condition's longer way, and 21 for every round of a block move but its
 *        last; the board's time is 2.5 board cycles a Z80 cycle (20 MHz against 8), rounded down;
 *      - when a block starts, the YM3438 timers due by then run out, and an interrupt is taken if the
 *        line is up and the Z80 lets interrupts in (not right after an EI that ended the block before):
 *        13 cycles and one count of the refresh register, then sound_irq(); after it returns the same is
 *        looked at again;
 *      - the refresh register counts the opcode fetches: every block's (tools/z80/timing.py fetches), two
 *        for every round of a block move but its last, and one for an interrupt; it starts at 0;
 *      - everything a block reads and writes happens at the time it starts;
 *      - the board runs the program whenever its time moves on (s32_board.time_passes): from its reset
 *        once the I/O chip lets the Z80 run, then the main loop again and again until the Z80's time is
 *        past the board's (the loop's top is where it waits, sound_symbols.txt's "loop" line); while the
 *        Z80 is held in reset, only the timers run.
 */
#include "sound/support.h"
#include "sound/chips.h"
#include "s32/byte_order.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INTERRUPT_CYCLES 13

/* the board's time when the Z80 has run 'cycles' */
static uint64_t board_time(uint64_t cycles)
{
#if S32_CPU_CLOCK * 2ull == S32_SOUND_CLOCK * 5ull
	/* (the same floor without a 64-bit division, which is a library call on 32-bit processors such as
	   the Dreamcast's SH4, run for every block of the sound program) */
	return (cycles * 5u) >> 1;
#else
	return cycles * S32_CPU_CLOCK / S32_SOUND_CLOCK;
#endif
}

/* the first Z80 cycle at or after the board's 'time' */
static uint64_t z80_time(uint64_t time)
{
	return (time * S32_SOUND_CLOCK + S32_CPU_CLOCK - 1) / S32_CPU_CLOCK;
}

uint64_t sound_chips_quiet_until(void);
bool sound_chips_busy_at(uint64_t time);
extern uint32_t sound_chips_timer_expiries;
extern uint64_t sound_chips_next_deadline;

/* the sound program's ways in (sound_symbols.txt) */
void sound_reset(void);
void sound_irq(void);
void sound_main_loop(void);

s32_z80_state s32_z80 = { .held = true };
#define z80 s32_z80

/* the gate (support.h): the next timer's end, or now while an interrupt may be taken */
static void set_gate(void)
{
	if (z80.line && z80.interrupts)
		z80.gate = 0;
	else if (sound_chips_next_deadline >= UINT64_MAX / 8)
		z80.gate = UINT64_MAX;
	else
	{
		/* (the deadline changes far less often than this runs, after every block: its 64-bit division,
		   a library call on the SH4, is remembered) */
		static uint64_t deadline = UINT64_MAX, gate;

		if (sound_chips_next_deadline != deadline)
		{
			deadline = sound_chips_next_deadline;
			gate = z80_time(deadline);
		}
		z80.gate = gate;
	}
}
void sound_cpu_deadline_changed(void) { set_gate(); }

/* when the block being run makes its reads and writes (board cycles) */
uint64_t sound_cpu_now(void) { return board_time(z80.block_start); }

uint64_t sound_passes_run, sound_passes_skipped;    /* (main loop passes run, and counted without running) */
#if defined(__linux__) && !defined(__DREAMCAST__)
__attribute__((destructor)) static void idle_stats(void)
{
	if (getenv("S32_IDLE_STATS")) fprintf(stderr, "sound passes run %llu stretched cycles %llu timer expiries %u\n", (unsigned long long)sound_passes_run, (unsigned long long)sound_passes_skipped, (unsigned)sound_chips_timer_expiries);
}
#endif

/* What a pass of the main loop did that the idle skip (on_time_passes) must know about */
static struct
{
	uint32_t outputs;       /* port writes, chip writes, block moves */
	uint32_t inputs;        /* a hash of what its port and chip reads returned */
	uint32_t interrupts;    /* interrupts taken */
} pass;

void z80_block_board(int cycles, int fetches)
{
	const uint64_t time = board_time(z80.clock);
	if (time >= sound_chips_next_deadline || (z80.line && z80.interrupts && !z80.after_ei))
	for (;;)
	{
		sound_chips_timers_due(board_time(z80.clock));
		if (!z80.interrupts || z80.after_ei || !z80.line)
			break;
		pass.interrupts++;
		z80.interrupts = false;
		z80.clock += INTERRUPT_CYCLES;
		z80.refresh++;
		sound_irq();
	}
	z80.after_ei = false;
	s32_board.sound_cycles = board_time(z80.clock);
	z80.block_start = z80.clock;
	z80.clock += (uint64_t)cycles;
	z80.block_refresh = z80.refresh;
	z80.refresh += (uint32_t)fetches;
	set_gate();
}

uint8_t refresh_read(int fetches)
{
	return (uint8_t)((z80.block_refresh + (uint32_t)fetches) & 0x7f);
}

void interrupts_on(void)
{
	z80.interrupts = true;
	set_gate();
}

void interrupts_on_after_block(void)
{
	z80.interrupts = true;
	z80.after_ei = true;
	set_gate();
}

void interrupts_off(void)
{
	z80.interrupts = false;
}

uint8_t z80_read8(uint16_t address)
{
	const uint8_t value = s32_z80_read(address);
	if (address >= 0xc000 && address < 0xe000)    /* (the MultiPCM) */
		pass.inputs = (pass.inputs ^ value) * 16777619u;
	return value;
}

uint16_t z80_read16(uint16_t address)
{
	const uint8_t low = s32_z80_read(address);
	return (uint16_t)(low | s32_z80_read((uint16_t)(address + 1)) << 8);
}

void z80_write8(uint16_t address, uint8_t value)
{
	if (address >= 0xc000 && address < 0xe000)
		pass.outputs++;
	s32_z80_write(address, value);
}

void z80_write16(uint16_t address, uint16_t value)
{
	s32_z80_write(address, (uint8_t)value);
	s32_z80_write((uint16_t)(address + 1), (uint8_t)(value >> 8));
}

uint8_t port_in(uint8_t port)
{
	const uint8_t value = s32_z80_in(port);
	pass.inputs = ((pass.inputs ^ port) * 16777619u ^ value) * 16777619u;
	return value;
}

void port_out(uint8_t port, uint8_t value)
{
	/* the MultiPCM's samples up to now come from the banks as they were */
	if ((port & 0xf0) == 0xb0)
		sound_chips_pcm_banks_changing();
	pass.outputs++;
	s32_z80_out(port, value);
}

void move_block(uint16_t source, uint16_t destination, uint16_t count, bool upward)
{
	const uint32_t rounds = count ? count : 0x10000;

	pass.outputs++;
	for (uint32_t round = 0; round < rounds; round++)
	{
		s32_z80_write(destination, s32_z80_read(source));
		if (source >= 0xe000 && destination >= 0xe000)    /* (a byte of the RAM to another: its place along) */
			s32_order_moved(&s32_board.shared_ram[destination & 0x1fff], &s32_board.shared_ram[source & 0x1fff], 1);
		source = (uint16_t)(upward ? source + 1 : source - 1);
		destination = (uint16_t)(upward ? destination + 1 : destination - 1);
	}
	z80.clock += 21 * (uint64_t)(rounds - 1);
	z80.refresh += 2 * (rounds - 1);
}

void sound_invalid_jump(uint32_t address)
{
	if (s32_board.log)
	{
		char message[64];
		snprintf(message, sizeof(message), "sound program: a jump to %04X, past its table", (unsigned)address);
		s32_board.log(message);
	}
}

/* ---- the board's side ---- */

static void on_time_passes(uint64_t until)
{
	if (z80.held)
	{
		sound_chips_timers_due(until);
		if (z80.clock < z80_time(until))
			z80.clock = z80_time(until);
		return;
	}
	if (!z80.running)
	{
		z80.running = true;
		sound_reset();
	}
	/* S32_SOUND_PASS_STRETCH n (the Dreamcast: 3; unset: exact, as the reference): the main loop polls,
	   about 45 passes a frame, for commands and the timers' ticks, and each pass costs the console about
	   0.1 ms. With n, after a pass the clock moves on by n - 1 more passes' worth without running them,
	   as if the Z80 were that much slower, but never past the next timer's end or 'until': a tick or a
	   command is still seen by the next pass that would have seen it, at most a pass (about 1 ms at 3)
	   later. Not the reference's exact timing, so the host's sound hashes differ with it. */
	while (board_time(z80.clock) < until)
	{
#ifdef S32_SOUND_PASS_STRETCH
		const uint64_t clock0 = z80.clock;
#endif
		pass.outputs = 0; pass.interrupts = 0;
		sound_main_loop();
		sound_passes_run++;
#ifdef S32_SOUND_PASS_STRETCH
		if (pass.interrupts == 0)
		{
			uint64_t target = sound_chips_quiet_until();
			if (until < target) target = until;
			const uint64_t stop = z80_time(target);
			uint64_t more = (z80.clock - clock0) * (uint64_t)(S32_SOUND_PASS_STRETCH - 1);
			if (stop > z80.clock)
			{
				if (more > stop - z80.clock) more = stop - z80.clock;
				sound_passes_skipped += more;
				z80.clock += more;
			}
		}
#endif
	}
}

static void on_sound_irq(bool asserted, uint8_t vector)
{
	(void)vector;    /* interrupt mode 1: always 0038 */
	z80.line = asserted;
	set_gate();
}

static void on_sound_reset(bool held)
{
	if (held)
	{
		z80.running = false;
		z80.interrupts = false;
		z80.after_ei = false;
		z80.refresh = 0;
	}
	z80.held = held;
}

/* Hooks the sound program to the board; s32_board.sound_rom and the chips have to be set */
void sound_cpu_attach(void)
{
	/* the tables the program reads by name, where the chip (or the assets) has them */
	for (size_t index = 0; index < sound_table_count; index++)
		*sound_tables[index].data = s32_sound_rom_at(sound_tables[index].address);
	z80.clock = z80_time(s32_board.cycles);
	s32_board.time_passes = on_time_passes;
	s32_board.sound_irq_changed = on_sound_irq;
	s32_board.sound_reset_changed = on_sound_reset;
}
