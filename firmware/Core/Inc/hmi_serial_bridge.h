#ifndef HMI_SERIAL_BRIDGE_H
#define HMI_SERIAL_BRIDGE_H

#include <usart.h>

void HMI_Bridge_Init(void);
void HMI_Bridge_Poll(void);
void HMI_Bridge_RxCpltCallback(UART_HandleTypeDef *uart);
void HMI_Bridge_ErrorCallback(UART_HandleTypeDef *uart);

#endif
