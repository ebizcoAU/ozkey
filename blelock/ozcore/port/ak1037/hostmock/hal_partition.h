/* hal_partition.h (host mock). */
#ifndef HAL_PARTITION_H_MOCK
#define HAL_PARTITION_H_MOCK
#include "ak_mock_types.h"
#define SFLASH_FILE_NAME_LEN 6
typedef struct {
    uint8_t  file_name[SFLASH_FILE_NAME_LEN + 1];
    uint32_t file_len;
    uint32_t file_start_page;
    uint32_t file_partition_size;
    char    *xip_start_addr;
} T_SPI_FILE_INFO;
uint8_t partition_get_bin_info(T_SPI_FILE_INFO *pBinInfo);
#endif
