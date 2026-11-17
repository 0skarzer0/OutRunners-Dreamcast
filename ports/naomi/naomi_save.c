/* SPDX-License-Identifier: BSD-3-Clause */
/* What the arcade board keeps with the power off (its EEPROM: the settings; the battery-backed end of the
   sound board's RAM: the course records, the bookkeeping and the credits) in the NAOMI's battery-backed SRAM,
   in place of the Dreamcast port's VMU file (../dreamcast/vmu_save.c, the same contents). The region:
   0x1000-0x1FFF (naomi-kit's table: the Daytona USA recompilation had 0x1000-0x5FFF, unused since the native port
   replaced it). A copy is written within a second of a change: the SRAM has no flash to wear out.
   An original NAOMI game that uses the whole SRAM may overwrite it: the checksum then finds no save and the
   board's defaults are used, as on a new board. */
#include "dc_port.h"
#include "vmu_save.h"
#include <kos.h>
#include <s32/machine.h>
#include <nk/naomi_kit.h>
#include <string.h>

#define BACKUP_RAM_START 0x1f00
#define BACKUP_RAM_SIZE  0xfa
#define SRAM_OFFSET      0x1000u
#define SRAM_REGION      0x1000u
#define MAGIC            "ORUN"

typedef struct
{
	uint32_t version;                      /* 1 */
	uint16_t eeprom[S32_EEPROM_WORDS];
	uint8_t backup[BACKUP_RAM_SIZE];
	uint8_t spare[2];
} naomi_save;

uint8_t dc_options[DC_OPTIONS];
bool dc_options_loaded;
volatile uint32_t dc_vmu_state;    /* bits 0-1 the load's result + 1, bits 8-23 saves written, 24-31 failures */
static naomi_save saved;

static void current(naomi_save *d)
{
	memset(d, 0, sizeof(*d));
	d->version = 1;
	memcpy(d->eeprom, s32_board.eeprom.data, sizeof(d->eeprom));
	memcpy(d->backup, &s32_board.shared_ram[BACKUP_RAM_START], BACKUP_RAM_SIZE);
}

int dc_vmu_load(void)
{
	naomi_save d;
	int result = 0;

	if (nk_sram_load(SRAM_OFFSET, SRAM_REGION, MAGIC, &d, sizeof(d)) == (int)sizeof(d) && d.version == 1)
	{
		memcpy(s32_board.eeprom.data, d.eeprom, sizeof(d.eeprom));
		memcpy(&s32_board.shared_ram[BACKUP_RAM_START], d.backup, BACKUP_RAM_SIZE);
		saved = d;
		result = 1;
		printf("NAOMI: the records and settings from the SRAM\n");
	}
	else
	{
		current(&saved);    /* (nothing to compare against yet: the first change is saved) */
		printf("NAOMI: no save in the SRAM: the board's defaults\n");
	}
	dc_vmu_state = (uint32_t)(result + 2);
	return result;
}

void dc_vmu_frame(uint32_t frame)
{
	naomi_save now;

	if (frame % 60 != 0)
		return;
	current(&now);
	if (memcmp(&now, &saved, sizeof(now)) == 0)
		return;
	if (nk_sram_save(SRAM_OFFSET, SRAM_REGION, MAGIC, &now, sizeof(now)))
	{
		saved = now;
		dc_vmu_state += 1u << 8;
	}
	else
		dc_vmu_state += 1u << 24;
}

/* (the console build's TIME ATTACK ghost: not in this one) */
void dc_vmu_ghost_release(void) {}
bool dc_vmu_ghost_offer(void) { return false; }
void dc_vmu_ghost_answer(bool save) { (void)save; }
