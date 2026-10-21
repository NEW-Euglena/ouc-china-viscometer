#ifndef HMI_SCREEN_H
#define HMI_SCREEN_H

#include <stdint.h>
#include "liquid_measurement.h"

void HMI_Screen_Init(void);
void HMI_Screen_Poll(void);
void HMI_Screen_SetTemperature(uint8_t sensor_status, int32_t temperature_centi);
void HMI_Screen_SetPump(uint8_t enabled);
void HMI_Screen_SetLiquidStatus(uint8_t upper_wet, uint8_t lower_wet,
                                uint8_t upper_triggered, uint8_t lower_triggered);
void HMI_Screen_SetMeasurement(LiquidStage stage, LiquidError error,
                               uint8_t result_valid, uint32_t result_ms);
/* 仅从主循环调用；返回 1 表示已开始发送，不代表屏幕已执行成功。 */
uint8_t HMI_Screen_SendCommand(const char *command);
void HMI_Screen_RxCpltCallback(void);
void HMI_Screen_ErrorCallback(void);

#endif
