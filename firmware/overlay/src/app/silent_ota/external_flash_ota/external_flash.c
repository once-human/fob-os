/**
*********************************************************************************************************
*               Copyright(c) 2016, Realtek Semiconductor Corporation. All rights reserved.
**********************************************************************************************************
* @file     external_flash.c
* @brief    This file provides driver for flash.
* @details
* @author
* @date     2016-05-25
* @version  v1.0
*********************************************************************************************************
*/

/* Includes ------------------------------------------------------------------*/
#include "external_flash.h"
#include "trace.h"
#include "platform_utils.h"
#include "os_sync.h"

#if (SUPPORT_EXTERNAL_SPI_FLASH_OTA == 1)
#define EXTERNAL_FLASH_REDUCE_CODE_SIZE         0

/**
  * @brief  Reset External Flash
  *         Steps:
  *         1. Send Release Power-down command to wake up Flash
  *         2. Wait WIP bit cleared
  *         3. Send Reset Enable and Reset commands
  * @param  No parameter
  * @return Flash status
  */
bool ExternalFlash_Init(void)
{
    /* Power-up Timing can be ignored for the external Flash is not boot Flash */

    /* Wake up Flash */
    ExternalFlash_DeepSleep(false);

    uint32_t flash_id = ExternalFlash_ReadID();
    if (ExternalFlash_CheckValid(flash_id) == false)
    {
        DBG_DIRECT("Ext Flash(id:0x%06x) is invalid!", flash_id);
        return false;
    }

    /* Check Program/Erase status */
    ExternalFlash_CheckWriteStatus();

    /* Send Reset Enable */
    while (SPI_GetFlagState(FLASH_SPI, SPI_FLAG_BUSY) == SET);
    SPI_SendData(FLASH_SPI, RESET_ENABLE);

    platform_delay_us(1);

    /* Send Reset */
    while (SPI_GetFlagState(FLASH_SPI, SPI_FLAG_BUSY) == SET);
    SPI_SendData(FLASH_SPI, RESET_CMD);

    /* delay 30 ms after sending Reset command */
    platform_delay_ms(30);

    return true;
}

/**
  * @brief  Read ID of flash chip.
  * @param   No parameter.
  * @return  void
  *
  */
uint32_t ExternalFlash_ReadID(void)
{
    uint8_t sendBuf[4] = {0x9f, 0, 0, 0};
    uint8_t send_len = 4;
    uint8_t recvBuf[4];
    uint8_t index = 0;
    uint8_t recv_len = 0;

    /* Check SPI communication status */
    while (SPI_GetFlagState(FLASH_SPI, SPI_FLAG_BUSY) == SET);

    /* Clear receive FIFO */
    recv_len = SPI_GetRxFIFOLen(FLASH_SPI);
    while (recv_len)
    {
        SPI_ReceiveData(FLASH_SPI);
        recv_len--;
    }
    /* Read ID of Flash */
    SPI_SendBuffer(FLASH_SPI, sendBuf, send_len);

    /* Read data */
    while (send_len)
    {
        recv_len = SPI_GetRxFIFOLen(FLASH_SPI);
        while (recv_len)
        {
            recvBuf[index++] = SPI_ReceiveData(FLASH_SPI);
            recv_len--;
            send_len--;
        }
    }

    return (recvBuf[1] << 16) + (recvBuf[2] << 8) + recvBuf[3];
}
#if (EXTERNAL_FLASH_REDUCE_CODE_SIZE == 0)
/**
  * @brief  Read ID of flash chip.
  * @param   No parameter.
  * @return  void
  *
  */
uint32_t ExternalFlash_ReadID_EEPROM(void)
{
    uint8_t sendBuf[4] = {0x9f, 0, 0, 0};
    uint8_t send_len = 4;
    uint8_t recvBuf[4];
    uint8_t index = 0;
    uint8_t recv_len = 0;

    /* Check SPI communication status */
    while (SPI_GetFlagState(FLASH_SPI, SPI_FLAG_BUSY) == SET);

    /* Clear receive FIFO */
    recv_len = SPI_GetRxFIFOLen(FLASH_SPI);
    while (recv_len)
    {
        SPI_ReceiveData(FLASH_SPI);
        recv_len--;
    }

    SPI_SetReadLen(FLASH_SPI, 4);

    /* Read ID of Flash */
    SPI_SendBuffer(FLASH_SPI, sendBuf, send_len);

    /* Read data */
    while (send_len)
    {
        recv_len = SPI_GetRxFIFOLen(FLASH_SPI);
        while (recv_len)
        {
            recvBuf[index++] = SPI_ReceiveData(FLASH_SPI);
            recv_len--;
            send_len--;
        }
    }

    return (recvBuf[1] << 16) + (recvBuf[2] << 8) + recvBuf[3];
}
#endif
/**
  * @brief Read status register of flash.
  * @param  none.
  * @return value of status register.
  *
  */
uint8_t ExternalFlash_ReadStatus(void)
{
    uint8_t sendBuf[2] = {0x05, 0};
    uint8_t send_len = 2;
    uint8_t recvBuf[2];
    uint8_t recv_len = 0;
    uint8_t index = 0;

    /* Check SPI communication status */
    while (SPI_GetFlagState(FLASH_SPI, SPI_FLAG_BUSY) == SET);

    /* Send command of write status register */
    SPI_SendBuffer(FLASH_SPI, sendBuf, send_len);

    /* Read data */
    while (send_len)
    {
        recv_len = SPI_GetRxFIFOLen(FLASH_SPI);
        //dbg_printf("%d %d",send_len,recv_len);
        while (recv_len)
        {
            recvBuf[index++] = SPI_ReceiveData(FLASH_SPI);
            recv_len--;
            send_len--;
        }
    }

    return recvBuf[1];
}

/**
  * @brief Check flash is in progress or not.
  * @param  none.
  * @return SET: Flash is always in progress. RESET: Flash is in standby mode.
  *
  */
FlagStatus ExternalFlash_CheckWriteStatus(void)
{
    uint8_t reg_value = 0;
    uint32_t time_out = 10000000; /* about 60 second(s) */

    do
    {
        /* Time out control */
        time_out--;
        if (time_out == 0)
        {
            return SET;
        }
        reg_value = ExternalFlash_ReadStatus();
    }
    while (reg_value & STATUS_BUSY); /* Check flash is communicating or not */

    return RESET;
}

/**
  * @brief  Send write enable command before every page program(PP),
  * sector earse(SE), block earse(BE), chip earse(CE) and write status register(WRSR) command.
  * @param   No parameter.
  * @return  void
  *
  */
void ExternalFlash_WriteCmd(FunctionalState NewState)
{
    uint8_t send_len = 1;
    uint8_t recv_len = 0;

    /* Check earse status */
    ExternalFlash_CheckWriteStatus();
    //dbg_printf("check status done");

    /* Check communication status */
    while (SPI_GetFlagState(FLASH_SPI, SPI_FLAG_BUSY) == SET);

    /* Send write command */
    if (NewState == ENABLE)
    {
        /* Send write enable command */
        SPI_SendData(FLASH_SPI, 0x06);
    }
    else
    {
        /* Send write disable command */
        SPI_SendData(FLASH_SPI, 0x04);
    }

    /* Read data */
    while (send_len)
    {
        recv_len = SPI_GetRxFIFOLen(FLASH_SPI);
        while (recv_len)
        {
            SPI_ReceiveData(FLASH_SPI);
            recv_len--;
            send_len--;
        }
    }

    /* Check earse status */
    ExternalFlash_CheckWriteStatus();
}

#if (EXTERNAL_FLASH_REDUCE_CODE_SIZE == 0)
/**
  * @brief  Send write status register command.
  * @param  data: data which want to write to status register.
  * @return  void
  *
  */
void ExternalFlash_WriteStatusRegister(uint8_t data)
{
    uint8_t sendBuf[2] = {0x01, 0};
    uint8_t send_len = 2;
    uint8_t recv_len = 0;

    /* Enable write */
    ExternalFlash_WriteCmd(ENABLE);

    /* Write status register */
    while (SPI_GetFlagState(FLASH_SPI, SPI_FLAG_BUSY) == SET);
    sendBuf[1] = data & 0xff;
    SPI_SendBuffer(FLASH_SPI, sendBuf, send_len);

    /* Read data */
    while (send_len)
    {
        recv_len = SPI_GetRxFIFOLen(FLASH_SPI);
        while (recv_len)
        {
            SPI_ReceiveData(FLASH_SPI);
            recv_len--;
            send_len--;
        }
    }
}
#endif

/**
  * @brief Earse flash.
  * @param  address: address which begin to earse.
  * @param  mode: select earse mode.
  * @return none.
  *
  */
void ExternalFlash_Earse(uint32_t address, Flash_erase_module_t mode)
{
    uint8_t sendBuf[4] = {0, 0, 0, 0};
    uint8_t send_len = 4;
    uint16_t recv_len = 0;

    /* Enable write */
    ExternalFlash_WriteCmd(ENABLE);

    /* Write data */
    sendBuf[0] = mode;
    sendBuf[1] = (address >> 16) & 0xff;
    sendBuf[2] = (address >> 8) & 0xff;
    sendBuf[3] = address & 0xff;

    while (SPI_GetFlagState(FLASH_SPI, SPI_FLAG_BUSY) == SET);
    SPI_SendBuffer(FLASH_SPI, sendBuf, send_len);

    /* Read data no matter it is useful or not */
    while (send_len)
    {
        recv_len = SPI_GetRxFIFOLen(FLASH_SPI);
        while (recv_len)
        {
            SPI_ReceiveData(FLASH_SPI);
            recv_len--;
            send_len--;
        }
    }
}

#if (EXTERNAL_FLASH_REDUCE_CODE_SIZE == 0)
/**
  * @brief Earse chip.
  * @param  none.
  * @return none.
  *
  */
void ExternalFlash_EarseChip(void)
{
    uint8_t sendBuf[4] = {0, 0, 0, 0};
    uint8_t send_len = 1;
    uint16_t recv_len = 0;

    /* Enable write */
    ExternalFlash_WriteCmd(ENABLE);

    /* Write data */
    sendBuf[0] = CHIP_EARSE;
    while (SPI_GetFlagState(FLASH_SPI, SPI_FLAG_BUSY) == SET);
    SPI_SendBuffer(FLASH_SPI, sendBuf, send_len);

    /* Read data no matter it is useful or not */
    while (send_len)
    {
        recv_len = SPI_GetRxFIFOLen(FLASH_SPI);
        while (recv_len)
        {
            SPI_ReceiveData(FLASH_SPI);
            recv_len--;
            send_len--;
        }
    }
    //ExternalFlash_CheckWriteStatus();
}

/**
  * @brief Write flash.
  * @param  address: address which begin to write.
  * @param  psendBuf: address of data buffer which want to write.
  * @param  len: length of data buffer which want to write.
  * @return none.
  *
  */
static void ExternalFlash_Write_Internal(uint32_t address, uint8_t *psendBuf, uint16_t len)
{
    uint8_t sendBuf[4];
    uint16_t recv_len = 0;

    /* Enable write */
    ExternalFlash_WriteCmd(ENABLE);

    SPI_ChangeDirection(FLASH_SPI, SPI_Direction_TxOnly);

    /* Check communication status */
    while (SPI_GetFlagState(FLASH_SPI, SPI_FLAG_BUSY) == SET);

    /* Switch to Tx only mode */
    /* Send write command */
    sendBuf[0] = 0x02;
    sendBuf[1] = (address >> 16) & 0xff;
    sendBuf[2] = (address >> 8) & 0xff;
    sendBuf[3] = address & 0xff;

    uint32_t s;
    /* Enter the critical section */
    s = os_lock();
    SPI_SendBuffer(FLASH_SPI, sendBuf, 4);

    /* Write data */
    SPI_SendBuffer(FLASH_SPI, psendBuf, len);

    /* Check communication status */
    while (SPI_GetFlagState(FLASH_SPI, SPI_FLAG_BUSY) == SET);

    /* Read data no matter it is useful or not */
    recv_len = SPI_GetRxFIFOLen(FLASH_SPI);
    while (recv_len)
    {
        SPI_ReceiveData(FLASH_SPI);
        recv_len--;
    }

    /* Switch to full duplex mode */
    SPI_ChangeDirection(FLASH_SPI, SPI_Direction_FullDuplex);

    /* Exit the critical section */
    os_unlock(s);
}

/**
  * @brief Write flash.
  * @param  address: address which begin to write.
  * @param  psendBuf: address of data buffer which want to write.
  * @param  len: length of data buffer which want to write.
  * @return none.
  */
void ExternalFlash_Write(uint32_t address, uint8_t *psendBuf, uint16_t len)
{
    uint32_t tmp_addr = address;
    uint8_t *tmp_data = psendBuf;
    int remain_size = len;

    while (remain_size > 0)
    {
        uint32_t write_size;
        uint32_t page_vacancy = tmp_addr % PAGE_SIZE;

        if (page_vacancy)
        {
            write_size = PAGE_SIZE - page_vacancy;
        }
        else
        {
            write_size = PAGE_SIZE;
        }

        if (write_size > remain_size)
        {
            write_size = remain_size;
        }

        ExternalFlash_Write_Internal(tmp_addr, tmp_data, write_size);

        tmp_addr += write_size;
        tmp_data += write_size;
        remain_size -= write_size;
    }
}
#endif

/**
  * @brief Read flash.
  * @param  address: address which begin to read.
  * @param  pStoreBuf: address of data buffer which want to read.
  * @param  len: length of data buffer which want to read.
  * @return none.
  *
  */
void ExternalFlash_Read(uint32_t address, uint8_t *pStoreBuf, uint16_t len)
{
    uint32_t recvlen = 0;
    uint8_t sendBuf[4] = {0x03, 0, 0, 0};

    /* Read flash status register */

    ExternalFlash_CheckWriteStatus();

    /* Check communication status */
    while (SPI_GetFlagState(FLASH_SPI, SPI_FLAG_BUSY) == SET);

    uint32_t s;
    /* Enter the critical section */
    s = os_lock();

    /* Switch to EEPROM mode */
    SPI_ChangeDirection(FLASH_SPI, SPI_Direction_EEPROM);
    /* Clear RX FIFO */
    recvlen = SPI_GetRxFIFOLen(FLASH_SPI);
    /* Read data if RX FIFO has data */
    while (recvlen)
    {
        SPI_ReceiveData(FLASH_SPI);
        recvlen--;
    }

    /* Configure length of data which you want to read */
    SPI_SetReadLen(FLASH_SPI, len);

    /* Send read command and address */
    sendBuf[0] = 0x03;
    sendBuf[1] = (address >> 16) & 0xff;
    sendBuf[2] = (address >> 8) & 0xff;
    sendBuf[3] = address & 0xff;
    SPI_SendBuffer(FLASH_SPI, sendBuf, 4);

    /* Wait RX FIFO not empty flag */
    while (SPI_GetFlagState(FLASH_SPI, SPI_FLAG_RFNE) == RESET);

    /* Read data */
    while (len)
    {
        recvlen = SPI_GetRxFIFOLen(FLASH_SPI);
        while (recvlen)
        {
            *pStoreBuf++ = (uint8_t)SPI_ReceiveData(FLASH_SPI);
            recvlen--;
            len--;
        }
    }
    /* Switch to full duplex mode */
    SPI_ChangeDirection(FLASH_SPI, SPI_Direction_FullDuplex);

    os_unlock(s);
}

/**
  * @brief  Enable deep sleep mode or disable deep sleep mode
  * @param  enable: set true to enable or flase to disable deep sleep mode
  * @return void
  */
void ExternalFlash_DeepSleep(bool enable)
{
    uint8_t len = 1;
    uint8_t cmd;
    uint32_t delay_us;

    cmd = enable ? DEEP_POWER_DOWN : RELEASE_POWER_DOWN;
    /* wait tRES after enter deep sleep mode. GD25Q127C tRES is 30 us, double value is safe */
    delay_us = enable ? 1 : 60;

    SPI_SendBuffer(FLASH_SPI, &cmd, len);

    /* Check communication status */
    while (SPI_GetFlagState(FLASH_SPI, SPI_FLAG_BUSY) == SET);

    len = SPI_GetRxFIFOLen(FLASH_SPI);
    while (len--)
    {
        SPI_ReceiveData(FLASH_SPI);
    }

    platform_delay_us(delay_us);
}

static bool is_valid_vendor_id(uint32_t flash_id)
{
    /*
    #define FLASH_VENDOR_MXIC           0xC2
    #define FLASH_VENDOR_GD             0xC8
    #define FLASH_VENDOR_MICRON         0x20
    #define FLASH_VENDOR_WINBOND        0xEF
    #define FLASH_VENDOR_ATMEL          0x1F
    #define FLASH_VENDOR_SST            0xBF
    #define FLASH_VENDOR_SPANSION       0x01
    #define FLASH_VENDOR_PARAGON        0xE0
    #define FLASH_VENDOR_FUDAN          0xA1
    #define FLASH_VENDOR_ESMT           0x1C
    #define FLASH_VENDOR_PUYA           0x85
    #define FLASH_VENDOR_XTX            0x0B
    #define FLASH_VENDOR_ZBIT           0x5E
    #define FLASH_VENDOR_NORMEM         0x52
    #define FLASH_VENDOR_BOYA           0x68
    */
    uint8_t flash_vendor_id[] = {0xC2, 0xC8, 0x20, 0xEF, 0x1F, 0xBF, 0x01, 0xE0, 0xA1, 0x1C, 0x85, 0x0B, 0x5E, 0x52, 0x68};
    bool ret = false;

    for (int i = 0; i < sizeof(flash_vendor_id) / sizeof(flash_vendor_id[0]); i++)
    {
        if ((flash_id >> 16) == flash_vendor_id[i])
        {
            ret = true;
            break;
        }
    }

    return ret;
}

static bool is_valid_capcity_id(uint32_t flash_id)
{
    uint8_t capcity_id = flash_id & 0xF;

    /* 4MB bit to 256M bit */
    if ((capcity_id >= 2) && (capcity_id <= 9))
    {
        return true;
    }
    else
    {
        return false;
    }
}

/**
  * @brief  Check whether Flash ID is invalid or not
  * @param  flash_id: Flash ID
  * @return Flash Status
  */
bool ExternalFlash_CheckValid(uint32_t flash_id)
{
    if (is_valid_vendor_id(flash_id) == false)
    {
        return false;
    }

    if (is_valid_capcity_id(flash_id) == false)
    {
        return false;
    }

    return true;
}

#if (EXTERNAL_FLASH_REDUCE_CODE_SIZE == 0)
uint8_t ExternalFlash_GetBlockProtect(void)
{
    const uint8_t bp_mask = 0xF;
    const uint8_t bp_start_offset = 2;

    uint8_t bp_lv = 0;
    uint8_t status = 0;

    status = ExternalFlash_ReadStatus();
    bp_lv = ((status >> bp_start_offset) & bp_mask);

    return bp_lv;
}

void ExternalFlash_SetBlockProtect(uint8_t bp_lv)
{
    const uint8_t bp_mask = 0xF;
    const uint8_t bp_start_offset = 2;

    uint8_t status = 0;

    status = ExternalFlash_ReadStatus();
    status &= ~(bp_mask << bp_start_offset);
    status |= ((bp_lv & bp_mask) << bp_start_offset);

    ExternalFlash_WriteStatusRegister(status);
    ExternalFlash_CheckWriteStatus();
}
#endif

#endif

/******************* (C) COPYRIGHT 2016 Realtek Semiconductor Corporation *****END OF FILE****/

