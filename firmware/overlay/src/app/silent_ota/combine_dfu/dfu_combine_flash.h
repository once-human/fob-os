/**
************************************************************************************************************
*               Copyright(c) 2022, Realtek Semiconductor Corporation. All rights reserved.
************************************************************************************************************
* @file
* @brief
* @details
* @author
* @date
* @version
*************************************************************************************************************
*/

#ifndef _DFU_COMBINE_FLASH_H_
#define  _DFU_COMBINE_FLASH_H_

#include <stdint.h>
#include <stdbool.h>

uint8_t rtk_ota_init(void);

bool rtk_ota_flash_read(uint32_t start_addr, uint32_t data_len, uint8_t *data);
bool rtk_ota_flash_write(uint32_t offset, uint32_t data_len, uint8_t *data);

bool rtk_ota_flash_erase(uint32_t start_addr, uint32_t data_len);

uint8_t rtk_ota_validate_fw(void);
uint8_t rtk_ota_activate_image(void);

#endif
