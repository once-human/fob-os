#ifndef _EXTERNAL_FLASH_CFG_H_
#define _EXTERNAL_FLASH_CFG_H_
#include "board.h"

#ifdef __cplusplus
extern "C" {
#endif

#if (SUPPORT_EXTERNAL_SPI_FLASH_OTA == 1)
#define SPI0_SCK_PIN                    P4_0
#define SPI0_MISO_PIN                   P4_1
#define SPI0_MOSI_PIN                   P4_2
#define SPI0_CS_PIN                     P4_3
#define FLASH_SPI                       SPI0
#endif

#ifdef __cplusplus
}
#endif

#endif /* _EXTERNAL_FLASH_CFG_H_ */
