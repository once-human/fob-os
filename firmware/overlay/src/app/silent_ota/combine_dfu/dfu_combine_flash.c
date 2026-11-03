/**
*****************************************************************************************
*     Copyright(c) 2022, Realtek Semiconductor Corporation. All rights reserved.
*****************************************************************************************
   * @file
   * @brief
   * @author Grace
   * @date    2022-10-11
   * @version v1.0
   **************************************************************************************
   * @attention
   * <h2><center>&copy; COPYRIGHT 2022 Realtek Semiconductor Corporation</center></h2>
   * *************************************************************************************
  */
#include "bt_types.h"
#include "trace.h"
#include "dfu_api.h"
#include "dfu_flash.h"
#include "dfu_service.h"
#include "dfu_combine_flash.h"
#include "board.h"

#if (COMBINED_OTA_TYPE == COMBINED_OTA_TYPE_ONCE_TRANS)
/*============================================================================*
 *                              Macros
 *============================================================================*/
/* max image number in one merged OTA bin */
#define DFU_MAX_IMG_NUM         5

/*dfu error code*/
typedef enum
{
    DFU_RSP_SUCCESS                        = 0x01,
    DFU_RSP_FAIL_INVALID_PARAMETER         = 0x02,
    DFU_RSP_FAIL_OPERATION                 = 0x03,
    DFU_RSP_FAIL_DATA_SIZE_EXCEEDS_LIMIT   = 0x04,
    DFU_RSP_FAIL_CRC_ERROR                 = 0x05,
    DFU_RSP_FAIL_LENGTH_ERROR              = 0x06,
    DFU_RSP_FAIL_PROG_ERROR                = 0x07,
    DFU_RSP_FAIL_ERASE_ERROR               = 0x08,
    DFU_RSP_FAIL_SYS_VERSION_ERROR         = 0x09,
    DFU_RSP_FAIL_FLASH_OPERATION           = 0x0A,
} T_DFU_RSP_CODE;

#if OTA_DUAL_BANK_FILTER
typedef enum
{
    DFU_OTA_BANK_0                         = 0x00,
    DFU_OTA_BANK_1                         = 0x01
} T_DFU_OTA_BANK;

typedef enum
{
    DFU_ACTION_SKIP_DATA                   = 0x00,
    DFU_ACTION_ADOPT_DATA                  = 0x01,
} T_DFU_ACTION;
#endif

/* dfu status code */
typedef enum
{
    DFU_STS_UNKNOWN  = 0,
    DFU_STS_IDLE,
    DFU_STS_WAIT_HEADER,
    DFU_STS_START,
    DFU_STS_RX_FW,
    DFU_STS_VALID_FW,
    DFU_STS_ACT_RST,
    DFU_STS_END,
    DFU_STS_MAX
} T_DFU_STS;

/* dfu app db */
typedef struct
{
    T_IMG_CTRL_HEADER_FORMAT    ctrl_header;

#if OTA_DUAL_BANK_FILTER
    /* receiving which bank data */
    /* note that we assume OTA always starts from Bank 0 */
    T_DFU_OTA_BANK              current_bank;
    /* which bank should be updated */
    T_DFU_OTA_BANK              update_bank;
    /* OTA module currently is adopting data to flash or skipping */
    T_DFU_ACTION                action;
    /* left sub-images to be skipped */
    uint8_t                     num_images_skip;
#endif

    T_DFU_STS                   state;
    uint32_t                    dest_addr;
    uint32_t                    image_total_length;
    uint32_t                    cur_offset;
} T_DFU_PARAM;

/* image info */
typedef struct
{
    T_IMG_ID image_id;
    uint32_t image_size;
    uint32_t image_offset;
    uint8_t valid;
} T_TEMP_IMG_INFO;

/* dfu app db */
T_DFU_PARAM app_dfu_param;

/* rx buffer and related index */

//uint8_t  dfu_temp_buf[DFU_TEMP_BUFFER_SIZE];

#if OTA_DUAL_BANK_FILTER
/* start offset of currently handling bank */
uint32_t dfu_img_bank_offset;
#endif

/* image header buffer */
uint8_t   temp_img_hdr[DFU_HEADER_SIZE];
uint16_t  temp_img_hdr_used_size = 0;

/* image info list for record current rx state */
uint8_t dfu_image_num = 0;
T_TEMP_IMG_INFO dfu_image_info[DFU_MAX_IMG_NUM];



/*============================================================================*
 *                             Local Functions
 *============================================================================*/
uint32_t get_cur_dfu_img_header_offset(void)
{
    uint32_t header_offset = 0;
    if (dfu_image_num == 0)
    {
        header_offset = 0;
    }
    else
    {
        header_offset = dfu_image_info[dfu_image_num - 1].image_size +
                        dfu_image_info[dfu_image_num - 1].image_offset;
    }
#if OTA_DUAL_BANK_FILTER
    return header_offset + dfu_img_bank_offset;
#else
    return header_offset;
#endif
}

uint32_t get_cur_dfu_img_tail(void)
{
    uint32_t tail_addr = 0;
    tail_addr = dfu_image_info[dfu_image_num].image_offset +
                dfu_image_info[dfu_image_num].image_size +
#if OTA_DUAL_BANK_FILTER
                dfu_img_bank_offset;
#else
                0;
#endif
    return tail_addr;
}

uint32_t get_cur_dfu_img_total_len(void)
{
    uint32_t total_length = 0;
    total_length = app_dfu_param.image_total_length;
    return total_length;
}

uint8_t dfu_receive_data(uint16_t length, uint8_t *p_value)
{
    uint32_t result = 0;
#if OTA_DUAL_BANK_FILTER
    if (app_dfu_param.action == DFU_ACTION_SKIP_DATA)
    {
        DFU_PRINT_INFO2("dfu_receive_data, skip image 0x%x, #images to skip: %d",
                        app_dfu_param.ctrl_header.image_id, app_dfu_param.num_images_skip);
    }
    else
#endif
    {
        // write to flash
        result = dfu_update(app_dfu_param.ctrl_header.image_id,
#if OTA_DUAL_BANK_FILTER
                            app_dfu_param.cur_offset,
#else
                            app_dfu_param.cur_offset + dfu_image_info[dfu_image_num].image_offset,
#endif
                            length,
                            (uint32_t *)p_value, false);
    }
    if (result)
    {
        DBG_DIRECT("dfu_receive_data dfu_update failed, result=%d", result);
        //dfu_fw_reboot(false);
        return DFU_RSP_FAIL_FLASH_OPERATION;
    }
    else
    {
        /*update varibals value*/
        app_dfu_param.cur_offset += length;
    }
    return DFU_RSP_SUCCESS;
}

uint8_t dfu_validate_img(void)
{
    bool check_result = false;
    check_result = dfu_check_checksum(dfu_image_info[dfu_image_num].image_id,
                                      dfu_image_info[dfu_image_num].image_offset);

    DFU_PRINT_INFO1("DFU_OPCODE_VALID_FW: check_result=%d (1: Success, 0: Fail)", check_result);

    if (check_result)
    {
        dfu_image_info[dfu_image_num].valid = true;
        dfu_image_num ++;
        DFU_PRINT_INFO1("DFU_OPCODE_VALID_FW: dfu_image_num=%d ", dfu_image_num);
        return true;
    }
    else
    {
        return false;
    }
}

uint8_t dfu_update_write_param(uint32_t *p_offset, uint32_t *p_data_len, uint8_t **pp_data,
                               uint32_t proc_len)
{
    if (*p_data_len < proc_len)
    {
        return false;
    }
    *p_offset += proc_len;
    *p_data_len -= proc_len;
    *pp_data += proc_len;
    APP_PRINT_INFO4("dfu_update_write_param: offset = 0x%x, len = %d, data @ 0x%x, sts=%d",
                    *p_offset, *p_data_len, *pp_data, app_dfu_param.state);
    return true;
}

uint8_t ota_handle_reset(void)
{
    dfu_image_num = 0;
    memset(dfu_image_info, 0x00, sizeof(dfu_image_info));
    g_ota_tmp_buf_used_size = 0;
    g_dfu_para.cur_offset = 0;

    app_dfu_param.cur_offset = 0;
    app_dfu_param.state = DFU_STS_IDLE;

    temp_img_hdr_used_size = 0;
    memset(temp_img_hdr, 0x00, sizeof(temp_img_hdr));

#if OTA_DUAL_BANK_FILTER
    // we use the start address of OTAHeader as criterion since it's unchanged after MP download.
    app_dfu_param.num_images_skip = 0;
    app_dfu_param.action = DFU_ACTION_ADOPT_DATA;

    DBG_DIRECT("ota_handle_reset 0x%x, 0x%x", BANK0_OTA_HEADER_ADDR, BANK1_OTA_HEADER_ADDR);
    if ((BANK1_OTA_HEADER_ADDR | FLASH_OFFSET_TO_NO_CACHE) == get_active_ota_bank_addr())
    {
        app_dfu_param.update_bank = DFU_OTA_BANK_0;
    }
    else
    {
        app_dfu_param.update_bank = DFU_OTA_BANK_1;
    }
#endif
    return DFU_RSP_SUCCESS;
}

uint8_t ota_handle_start(uint8_t *p_img)
{
    uint8_t *p = p_img;
    uint32_t result = 0;
    T_IMG_CTRL_HEADER_FORMAT dfu_control_point;

    dfu_control_point.ic_type = (*p);
    p += 1;
    dfu_control_point.secure_version = (*p);
    p += 1;
    LE_ARRAY_TO_UINT16(dfu_control_point.ctrl_flag.value, p);
    p += 2;
    LE_ARRAY_TO_UINT16(dfu_control_point.image_id, p);
    p += 2;
    LE_ARRAY_TO_UINT16(dfu_control_point.crc16, p);
    p += 2;
    LE_ARRAY_TO_UINT32(dfu_control_point.payload_len, p);

    DFU_PRINT_INFO6("DFU_OPCODE_START_DFU: ic_type=0x%x, secure_version=0x%x, ctrl_flag.value=0x%x, image_id=0x%x, crc16=0x%x, payload_len=%d",
                    dfu_control_point.ic_type,
                    dfu_control_point.secure_version,
                    dfu_control_point.ctrl_flag.value,
                    dfu_control_point.image_id,
                    dfu_control_point.crc16,
                    dfu_control_point.payload_len
                   );

    app_dfu_param.ctrl_header.ic_type = dfu_control_point.ic_type;
    app_dfu_param.ctrl_header.ctrl_flag.value = dfu_control_point.ctrl_flag.value;
    app_dfu_param.ctrl_header.image_id = dfu_control_point.image_id;
    app_dfu_param.ctrl_header.crc16 = dfu_control_point.crc16;
    app_dfu_param.ctrl_header.payload_len = dfu_control_point.payload_len;
    app_dfu_param.image_total_length = app_dfu_param.ctrl_header.payload_len + IMG_HEADER_SIZE;

    /*check if start dfu fields are vaild*/
    if (app_dfu_param.ctrl_header.ic_type == DEFINED_IC_TYPE)
    {
        if ((app_dfu_param.ctrl_header.image_id >= OTA) && (app_dfu_param.ctrl_header.image_id < IMAGE_MAX))
        {
            /*disable bank switch, need record temp image info*/
            {
                dfu_image_info[dfu_image_num].image_id = (T_IMG_ID)app_dfu_param.ctrl_header.image_id;
                dfu_image_info[dfu_image_num].image_size = app_dfu_param.image_total_length;
                if (dfu_image_num == 0)
                {
                    dfu_image_info[dfu_image_num].image_offset = 0;
                }
                else
                {
                    dfu_image_info[dfu_image_num].image_offset = dfu_image_info[dfu_image_num - 1].image_offset +
                                                                 dfu_image_info[dfu_image_num - 1].image_size;
                }
            }
            DFU_PRINT_INFO4("DFU_OPCODE_START_DFU: image_num=%d, image_id=0x%x, image_size=0x%x, image_offset=0x%x",
                            dfu_image_num, dfu_image_info[dfu_image_num].image_id,
                            dfu_image_info[dfu_image_num].image_size, dfu_image_info[dfu_image_num].image_offset);


#if OTA_DUAL_BANK_FILTER
            if (dfu_image_num == 0)
            {
                T_OTA_HEADER_FORMAT     *dfu_ota_header = (T_OTA_HEADER_FORMAT *)(p - 8);
                app_dfu_param.current_bank = (T_DFU_OTA_BANK)((get_header_addr_by_img_id(RomPatch) ==
                                                               (dfu_ota_header->rom_patch_addr | FLASH_OFFSET_TO_NO_CACHE)) \
                                                              ? (!app_dfu_param.update_bank) : app_dfu_param.update_bank);
                if ((app_dfu_param.current_bank != app_dfu_param.update_bank))
                {
                    DFU_PRINT_INFO1("Received wrong bank data, processing bank start offset: %x", dfu_img_bank_offset);
                    DFU_PRINT_INFO2("current bank: %d, update_bank: %d", app_dfu_param.current_bank,
                                    app_dfu_param.update_bank);
                    if (app_dfu_param.action == DFU_ACTION_ADOPT_DATA)
                    {
                        DFU_PRINT_INFO1("dfu_fsbl_address: %x", dfu_ota_header->secure_boot_addr);
                        app_dfu_param.action = DFU_ACTION_SKIP_DATA;
                        app_dfu_param.num_images_skip = DFU_MAX_IMG_NUM;
                    }
                }
            }

            if (app_dfu_param.action != DFU_ACTION_SKIP_DATA)
#endif
            {
                DFU_PRINT_INFO2("ota_handle_start, img_header_size: %d, dfu_image_num: %d", DFU_HEADER_SIZE,
                                dfu_image_num);
                result = dfu_update(app_dfu_param.ctrl_header.image_id,
#if OTA_DUAL_BANK_FILTER
                                    0,
#else
                                    0 + dfu_image_info[dfu_image_num].image_offset,
#endif
                                    DFU_HEADER_SIZE,
                                    (uint32_t *)p_img, true);
            }
            if (result)
            {
                DBG_DIRECT("DFU_OPCODE_START_DFU: dfu_update fail=%d", result);

                //dfu_fw_reboot(false);
                return DFU_RSP_FAIL_FLASH_OPERATION;
            }
            else
            {
                app_dfu_param.cur_offset = DFU_HEADER_SIZE;
                DFU_PRINT_INFO0("DFU_OPCODE_START_DFU: start success!");
                return DFU_RSP_SUCCESS;
            }
        }
        else
        {
            DFU_PRINT_ERROR1("DFU_OPCODE_START_DFU: image id=0x%x Error!", app_dfu_param.ctrl_header.image_id);
            return DFU_RSP_FAIL_INVALID_PARAMETER;
        }
    }
    else
    {
        DFU_PRINT_ERROR1("DFU_OPCODE_START_DFU: ic type=0x%x Error!", app_dfu_param.ctrl_header.ic_type);
        return DFU_RSP_FAIL_INVALID_PARAMETER;
    }
}

uint8_t dfu_handle_state_idle(uint32_t *p_offset, uint32_t *p_data_len, uint8_t **pp_data)
{
    uint8_t ret_val = DFU_RSP_SUCCESS;
    uint32_t proc_len = 0;
    DFU_PRINT_INFO3("dfu_handle_state_idle, DFU_HEADER_SIZE: %d, get_cur_dfu_img_header_offset: %x, dfu_image_num: %d",
                    DFU_HEADER_SIZE, get_cur_dfu_img_header_offset(), dfu_image_num);
    // idle -> wait header
    if (*p_offset == get_cur_dfu_img_header_offset()
        && *p_data_len < DFU_HEADER_SIZE)
    {
        APP_PRINT_INFO2("[DFU_STATE_CHG] I->W, data_len = %d, offset = 0x%x",
                        *p_data_len, *p_offset);

        temp_img_hdr_used_size = *p_data_len;
        memcpy(temp_img_hdr, *pp_data, *p_data_len);
        APP_PRINT_INFO2("copy data to temp_img_hdr, used_size: %d, first 16 byte = [%b]",
                        temp_img_hdr_used_size, TRACE_BINARY(16, temp_img_hdr));
        proc_len = *p_data_len;
        app_dfu_param.state = DFU_STS_WAIT_HEADER;
    }
    // idle -> start
    else if (*p_offset == get_cur_dfu_img_header_offset()
             && *p_data_len >= DFU_HEADER_SIZE)
    {
        APP_PRINT_INFO2("[DFU_STATE_CHG] I->S, data_len = %d, offset = 0x%x",
                        *p_data_len, *p_offset);
        APP_PRINT_INFO2("dfu_handle_state_idle, new image starts, p_offset: 0x%x, data: [%b]", *p_offset,
                        TRACE_BINARY(16, *pp_data));
        ret_val = ota_handle_start((uint8_t *)*pp_data);
        if (ret_val == DFU_RSP_SUCCESS)
        {
            proc_len = DFU_HEADER_SIZE;
            app_dfu_param.state = DFU_STS_START;
        }
        else
        {
            app_dfu_param.state = DFU_STS_IDLE;
        }
    }
    else
    {
        return DFU_RSP_FAIL_INVALID_PARAMETER;
    }

    dfu_update_write_param(p_offset, p_data_len, pp_data, proc_len);
    return ret_val;
}

uint8_t dfu_handle_state_wait_header(uint32_t *p_offset, uint32_t *p_data_len, uint8_t **pp_data)
{
    uint8_t ret_val = DFU_RSP_SUCCESS;
    uint32_t proc_len = 0;
    if (temp_img_hdr_used_size != 0)
    {
        APP_PRINT_INFO3("[DFU_STATE_CHG] W->W/S, temp_img_hdr_used_size = %d, data_len = %d, offset = 0x%x",
                        temp_img_hdr_used_size, *p_data_len, *p_offset);

        uint32_t empty_len = DFU_HEADER_SIZE - temp_img_hdr_used_size;
        if (*p_data_len >= empty_len)
        {
            memcpy(temp_img_hdr + temp_img_hdr_used_size, *pp_data, DFU_HEADER_SIZE - temp_img_hdr_used_size);
            APP_PRINT_INFO2("copy data to temp_img_hdr, used_size: %d, first 16 byte = [%b]",
                            temp_img_hdr_used_size, TRACE_BINARY(16, temp_img_hdr));
            proc_len = empty_len;
            temp_img_hdr_used_size = 0;
            ret_val = ota_handle_start((uint8_t *)temp_img_hdr);
            if (ret_val == DFU_RSP_SUCCESS)
            {
                app_dfu_param.state = DFU_STS_START;
            }
            else
            {
                app_dfu_param.state = DFU_STS_IDLE;
            }
        }
        else
        {
            memcpy(temp_img_hdr + temp_img_hdr_used_size, *pp_data, *p_data_len);
            proc_len = *p_data_len;
            temp_img_hdr_used_size += *p_data_len;
        }
    }
    else
    {
        return DFU_RSP_FAIL_OPERATION;
    }

    dfu_update_write_param(p_offset, p_data_len, pp_data, proc_len);
    return ret_val;
}

uint8_t dfu_handle_state_rx_data(uint32_t *p_offset, uint32_t *p_data_len, uint8_t **pp_data)
{
    uint8_t ret_val = DFU_RSP_SUCCESS;
    uint32_t proc_len = 0;
    APP_PRINT_INFO0("dfu_handle_state_rx_data");
    uint32_t tail_addr = get_cur_dfu_img_tail();
    if (tail_addr > *p_offset + *p_data_len)
    {
        APP_PRINT_INFO2("[DFU_STATE_CHG] Rx cont, data_len = %d, offset = 0x%x",
                        *p_data_len, *p_offset);

        // not reach next image, write all
        ret_val = dfu_receive_data(*p_data_len, *pp_data);
        if (ret_val  != DFU_RSP_SUCCESS)
        {
            return ret_val;
        }
        proc_len = *p_data_len;
        app_dfu_param.state = DFU_STS_RX_FW;
    }
    else
    {
        APP_PRINT_INFO2("[DFU_STATE_CHG] Rx -> END, data_len = %d, offset = 0x%x",
                        *p_data_len, *p_offset);

        // 1. write current image first
        uint32_t remained_len = tail_addr - *p_offset;
        ret_val = dfu_receive_data(remained_len, *pp_data);
        if (ret_val  != DFU_RSP_SUCCESS)
        {
            return ret_val;
        }
        proc_len = remained_len;
        app_dfu_param.state = DFU_STS_VALID_FW;
#if OTA_DUAL_BANK_FILTER
        if (app_dfu_param.action == DFU_ACTION_SKIP_DATA)
        {
            app_dfu_param.num_images_skip -= 1;
            dfu_image_num ++;
            DFU_PRINT_INFO1("dfu_handle_state_rx_data, one image finished, left to skip: %d",
                            app_dfu_param.num_images_skip);
            if (app_dfu_param.num_images_skip == 0)
            {
                app_dfu_param.action = DFU_ACTION_ADOPT_DATA;
                app_dfu_param.current_bank = app_dfu_param.update_bank;
                // record bank start address
                dfu_img_bank_offset = (*p_offset) + proc_len;
                dfu_image_num = 0;
            }
        }
        else
#endif
        {
            // validate
            if (dfu_validate_img() != true)
            {
                DBG_DIRECT("dfu_validate_img return false");
                //dfu_fw_reboot(false);
                return DFU_RSP_FAIL_CRC_ERROR;
            }
            if (dfu_image_num == DFU_MAX_IMG_NUM)
            {
                APP_PRINT_INFO0("[DFU_STATE_CHG] END -> FINISHED");
#if OTA_DUAL_BANK_FILTER
                app_dfu_param.action = DFU_ACTION_SKIP_DATA;
#endif
                app_dfu_param.state = DFU_STS_END;
            }
        }
        // 2. next image header will handle by while loop
        if (app_dfu_param.state != DFU_STS_END)
        {
            DBG_DIRECT("dfu_handle_state_rx_data dfu_image_num: %d", dfu_image_num);
            app_dfu_param.state = DFU_STS_IDLE;
        }
    }

    dfu_update_write_param(p_offset, p_data_len, pp_data, proc_len);
    return ret_val;
}

uint8_t ota_handle_write(uint32_t offset, uint32_t data_len, uint8_t *data)
{
    APP_PRINT_INFO3("ota_handle_write: offset = 0x%x, len = %d, data @ 0x%x",
                    offset, data_len, data);

    // offset == 0
    // start for new OTA proc
    uint8_t ret_val = DFU_RSP_FAIL_OPERATION;

    uint8_t prot_count = 0;
    uint32_t temp_data_len = 0;

    while (data_len)
    {
        // protection
        if (temp_data_len == data_len)
        {
            if (++prot_count > DFU_MAX_IMG_NUM)
            {
                return ret_val;
            }
        }

        temp_data_len = data_len;
        if (app_dfu_param.state == DFU_STS_END)
        {
            return DFU_RSP_SUCCESS;
        }

        // wait header loop -> start
        if (app_dfu_param.state == DFU_STS_WAIT_HEADER)
        {
            if (dfu_handle_state_wait_header(&offset, &data_len, &data) != DFU_RSP_SUCCESS)
            {
                return ret_val;
            }
        }

        // idle -> wait header | idle -> start
        if (app_dfu_param.state == DFU_STS_IDLE)
        {
            if (dfu_handle_state_idle(&offset, &data_len, &data) != DFU_RSP_SUCCESS)
            {
                return ret_val;
            }
        }

        // start -> rx loop -> valid
        if (app_dfu_param.state == DFU_STS_START
            || app_dfu_param.state == DFU_STS_RX_FW)
        {
            if (dfu_handle_state_rx_data(&offset, &data_len, &data) != DFU_RSP_SUCCESS)
            {
                return ret_val;
            }
        }
    }

    return DFU_RSP_SUCCESS;
}

/**
 * @brief read OTA data section
 * @param[in] start_addr.
 * @param[in] data_len.
 * @param[in] data pointer.
 * @retval result true: operation success, false: operation fail
 */
uint8_t rtk_ota_init(void)
{
    app_dfu_param.dest_addr =  flash_get_bank_addr(FLASH_OTA_TMP);
    app_dfu_param.state = DFU_STS_IDLE;
    app_dfu_param.cur_offset = 0;
#if OTA_DUAL_BANK_FILTER
    app_dfu_param.current_bank = DFU_OTA_BANK_0;
    dfu_img_bank_offset = 0;
#endif

    ota_handle_reset();

    return true;
}

/**
 * @brief read OTA data section
 * @param[in] start_addr.
 * @param[in] data_len.
 * @param[in] data pointer.
 * @retval result true: operation success, false: operation fail
 */
bool rtk_ota_flash_read(uint32_t start_addr, uint32_t data_len, uint8_t *data)
{
    uint32_t counter = 0;
    return flash_split_read_locked(start_addr, data_len, data, &counter);
}

/**
 * @brief write OTA image into OTA data section
 * @param[in] offset
 * @param[in] data_len
 * @param[in] data pointer.
 * @retval result true: operation success, false: operation fail
 */
bool rtk_ota_flash_write(uint32_t offset, uint32_t data_len, uint8_t *data)
{
    uint8_t ret_val = DFU_RSP_SUCCESS;
    APP_PRINT_INFO3("[RTK_OTA] write: offset = 0x%x, data_len = %d, data @ 0x%x",
                    offset, data_len, data);

//    if (offset == 0)
//    {
//        ota_handle_reset();
//    }

//    if (g_ota_tmp_buf_used_size + data_len > DFU_TEMP_BUFFER_SIZE)
//    {
//        ret_val = ota_handle_write(g_dfu_para.cur_offset,
//                                   g_ota_tmp_buf_used_size,
//                                   p_ota_temp_buffer_head);
//        g_dfu_para.cur_offset += g_ota_tmp_buf_used_size;

//        memcpy(p_ota_temp_buffer_head, data, data_len);
//        g_ota_tmp_buf_used_size = data_len;
//    }
//    else
//    {
//        memcpy(p_ota_temp_buffer_head + g_ota_tmp_buf_used_size, data, data_len);
//        g_ota_tmp_buf_used_size += data_len;
//    }

//    ret_val = ota_handle_write(g_dfu_para.cur_offset,
//                                   g_ota_tmp_buf_used_size,
//                                   p_ota_temp_buffer_head);

    ret_val = ota_handle_write(offset, data_len, data);


    if (ret_val == DFU_RSP_SUCCESS)
    {
        return true;
    }
    else
    {
        return false;
    }
}

/**
 * @brief erase OTA data section
 * @param[in] start_addr   flash addr in sector (4kb) to be erase
 * @param[in] data_len
 * @retval result true: operation success, false: operation fail
 */
bool rtk_ota_flash_erase(uint32_t start_addr, uint32_t data_len)
{
    uint32_t offset = start_addr;
    uint32_t remained_len = data_len;

    uint32_t total_block_cnt = remained_len / FMC_BLK_SECTION_LEN;
    uint32_t block_cnt = 0;

    DFU_PRINT_INFO3("[RTK_OTA] erase: addr = 0x%x, len = %d, total_block =%d",
                    offset, remained_len, total_block_cnt);

    if (0 != (offset & (FMC_SEC_SECTION_LEN - 1)))      // not 4k align
    {
        return false;
    }

    while (1)
    {
        if (0 == (offset & (FMC_BLK_SECTION_LEN - 1))         // new Block starts
            && (block_cnt < total_block_cnt))                 // still have block for erase
        {
            flash_erase_locked(FLASH_ERASE_BLOCK, offset);
            if (remained_len > FMC_BLK_SECTION_LEN)
            {
                offset += FMC_BLK_SECTION_LEN;
                remained_len -= FMC_BLK_SECTION_LEN;
                block_cnt ++;
            }
            else
            {
                break;
            }
        }
        else if (0 == (offset & (FMC_SEC_SECTION_LEN - 1)))   //new Sector starts
        {
            flash_erase_locked(FLASH_ERASE_SECTOR, offset);
            if (remained_len > FMC_SEC_SECTION_LEN)
            {
                offset += FMC_SEC_SECTION_LEN;
                remained_len -= FMC_SEC_SECTION_LEN;
            }
            else
            {
                break;
            }
        }
    }

    return true;
}

/**
 * @brief validate the FW image in OTA data section
 * @param none
 * @retval result true: operation success, false: operation fail
 */
uint8_t rtk_ota_validate_fw(void)
{
    bool check_result = false;
    uint8_t idx = 0;
    if (g_ota_tmp_buf_used_size != 0)
    {
        DFU_PRINT_INFO2("[RTK_OTA] validate: remaind buff offset = 0x%x, len = %d)",
                        g_dfu_para.cur_offset, g_ota_tmp_buf_used_size);

        ota_handle_write(g_dfu_para.cur_offset,
                         g_ota_tmp_buf_used_size,
                         p_ota_temp_buffer_head);
        g_dfu_para.cur_offset = 0;
        g_ota_tmp_buf_used_size = 0;
    }

    if (dfu_image_num == 0
#if TEST_SS_LOOP_OTA
        || dfu_image_num != 5
#endif
       )
    {
        return false;
    }

    for (idx = 0; idx < dfu_image_num; idx++)
    {
        if (dfu_image_info[idx].valid != true)
        {
            DFU_PRINT_INFO2("[RTK_OTA] validate: dfu_image_num=%d, check_result=%d (1: Success, 0: Fail)",
                            dfu_image_num, check_result);
            return false;
        }
    }
    return true;
}

/**
 * @brief reset to activate the FW image
 * @param none
 * @retval result true: operation success, false: operation fail
 */
uint8_t rtk_ota_activate_image(void)
{
    uint32_t base_addr = 0;
    uint8_t have_valid_fw = 0;

    T_IMG_CTRL_HEADER_FORMAT *p_header = NULL;
    if (IMAGE_USER_DATA == app_dfu_param.ctrl_header.image_id)
    {
        /* because flash_get_bank_addr(FLASH_BKP_DATA1) is not located flash block protect range, needn't unlock bp */
        base_addr = flash_get_bank_addr(FLASH_BKP_DATA1) | FLASH_OFFSET_TO_NO_CACHE;
    }
    else
    {
        /* check OTA temp or running bank to see if received image is OK.*/
        base_addr = get_temp_ota_bank_addr_by_img_id((T_IMG_ID)app_dfu_param.ctrl_header.image_id);
    }
    for (uint8_t i = 0; i < dfu_image_num; i++)
    {
        if (dfu_image_info[i].valid == true)
        {
#if OTA_DUAL_BANK_FILTER
            p_header = (T_IMG_CTRL_HEADER_FORMAT *)(base_addr);
            base_addr = get_temp_ota_bank_addr_by_img_id((T_IMG_ID)app_dfu_param.ctrl_header.image_id);
#else
            p_header = (T_IMG_CTRL_HEADER_FORMAT *)(base_addr + dfu_image_info[i].image_offset);
#endif
            dfu_set_image_ready(p_header);
            have_valid_fw++;
        }
    }

    if (have_valid_fw != dfu_image_num
        || have_valid_fw == 0)
    {
        return false;
    }
    // if check ok
    /*note: must unlock flash bp for ota copy before reset when not support bank switch*/
    unlock_flash_bp_all();

    //os_delay(20);  // debug, for printing debug message
    //dfu_switch_to_ota_mode();
    return true;
}

#endif // (COMBINED_OTA_TYPE == COMBINED_OTA_TYPE_ONCE_TRANS)


