/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FM1_FLASH_ID_H
#define FM1_FLASH_ID_H
#include <stdint.h>
/* Both supported 1 MiB parts use the same existing plain SPI NOR operations. */
static inline int fm1_flash_jedec_supported(uint32_t id)
{
    return id == 0x856014u || id == 0xB36014u;
}
#endif
