/* hal_sfc_norflash.h (host mock): page/sector API over a RAM NOR flash. */
#ifndef HAL_SFC_NORFLASH_H_MOCK
#define HAL_SFC_NORFLASH_H_MOCK
#include "ak_mock_types.h"
#define SPIFLASH_PAGE_SIZE       256
#define SPIFLASH_SECTOR_SIZE     (4 * 1024)
#define SPIFLASH_PAGE_PER_SECTOR 16
T_BOOL spi_flash_sector_erase(T_U32 sector);
T_BOOL spi_flash_write(T_U32 page, T_U8 *buf, T_U32 page_cnt);
T_BOOL spi_flash_read(T_U32 page, T_U8 *buf, T_U32 page_cnt);
#endif
