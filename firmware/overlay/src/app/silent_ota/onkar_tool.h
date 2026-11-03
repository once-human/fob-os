#ifndef ONKAR_TOOL_H
#define ONKAR_TOOL_H
#include <stdint.h>
void onkar_tool_handle(uint8_t conn_id, uint8_t *p, uint16_t len);
uint16_t onkar_battery_read(uint16_t *raw_out, uint8_t *pct_out, int8_t *err_out);
void onkar_init(void);
void onkar_heartbeat(void);
void onkar_dfu_activity(void);
void onkar_on_disconnect(void);
void onkar_os_tick(void);
#endif
