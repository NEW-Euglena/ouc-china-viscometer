#ifndef ESP_NETWORK_H
#define ESP_NETWORK_H

#include "main.h"
#include "liquid_measurement.h"

/* 传感器状态：0 正常，1 未找到，2 数据校验失败。 */
void ESP_Network_Init(void);
void ESP_Network_SetTemperature(uint8_t status, int32_t temperature_centi);
void ESP_Network_SetPump(uint8_t enabled);
/* 测量期间保留待保存配置，结束后再擦写 Flash。 */
void ESP_Network_SetMeasurementActive(uint8_t active);
void ESP_Network_SetMeasurement(const LiquidMeasurement *measurement);
/* USB 每秒异步发送一份状态，不依赖 Wi-Fi 连接。 */
void ESP_Network_PollUsbTelemetry(void);
void ESP_Network_Poll(void);

#endif
