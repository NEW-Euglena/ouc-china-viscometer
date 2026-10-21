#include "hmi_serial_bridge.h"
#include <stdio.h>
#include <string.h>

/* 两个接收队列独立保存原始字节，下载通道不输出任何调试文字。 */
typedef struct {
  uint8_t data[4096], byte;
  volatile uint16_t head, tail;
  volatile uint8_t fault;
} BridgeReceiver;
static BridgeReceiver host, screen;
static char command[80];
static uint8_t command_length, end_count, command_overflow, downloading;
/* 可通过调试器查看；只记录故障，不伪造屏幕应答。 */
volatile uint32_t hmi_bridge_rx_errors, hmi_bridge_overflows, hmi_bridge_tx_errors;

static void Recover(UART_HandleTypeDef *uart, BridgeReceiver *receiver) {
  HAL_UART_AbortReceive(uart);
  __HAL_UART_CLEAR_FLAG(uart, UART_CLEAR_OREF | UART_CLEAR_NEF |
                               UART_CLEAR_FEF | UART_CLEAR_PEF);
  __HAL_UART_SEND_REQ(uart, UART_RXDATA_FLUSH_REQUEST);
  receiver->fault = 0U;
  if (HAL_UART_Receive_IT(uart, &receiver->byte, 1U) != HAL_OK)
    receiver->fault = 1U;
}

void HMI_Bridge_Init(void) {
  memset(&host, 0, sizeof(host));
  memset(&screen, 0, sizeof(screen));
  command_length = end_count = command_overflow = downloading = 0U;
  hmi_bridge_rx_errors = hmi_bridge_overflows = hmi_bridge_tx_errors = 0U;
  Recover(&huart2, &host);
  Recover(&huart3, &screen);
}

void HMI_Bridge_RxCpltCallback(UART_HandleTypeDef *uart) {
  BridgeReceiver *receiver;
  if (uart->Instance == USART2) receiver = &host;
  else if (uart->Instance == USART3) receiver = &screen;
  else return;
  const uint16_t next = (uint16_t)((receiver->head + 1U) % sizeof(receiver->data));
  if (next == receiver->tail) ++hmi_bridge_overflows;
  else {
    receiver->data[receiver->head] = receiver->byte;
    receiver->head = next;
  }
  if (HAL_UART_Receive_IT(uart, &receiver->byte, 1U) != HAL_OK)
    receiver->fault = 1U;
}

void HMI_Bridge_ErrorCallback(UART_HandleTypeDef *uart) {
  if (uart->Instance == USART2) host.fault = 1U;
  else if (uart->Instance == USART3) screen.fault = 1U;
  else return;
  ++hmi_bridge_rx_errors;
}

static void TrackCommand(uint8_t value) {
  if (downloading != 0U) return;
  if (value == 0xffU) {
    if (++end_count < 3U) return;
    command[command_length] = '\0';
    unsigned long file_size, baud;
    char reserved;
    if (command_overflow == 0U &&
        sscanf(command, "whmi-wri %lu,%lu,%c", &file_size, &baud, &reserved) == 3 &&
        file_size != 0U && baud == 115200U) {
      /* 最后一个结束符已经阻塞发送至 TC，随后才切换屏幕端波特率。 */
      HAL_UART_AbortReceive(&huart3);
      huart3.Init.BaudRate = 115200U;
      if (HAL_UART_Init(&huart3) != HAL_OK) ++hmi_bridge_tx_errors;
      Recover(&huart3, &screen);
      downloading = 1U;
    }
    command_length = end_count = command_overflow = 0U;
  } else {
    if (end_count != 0U) command_overflow = 1U;
    end_count = 0U;
    if (value == 0U || value > 0x7fU) command_overflow = 1U;
    if (command_length < sizeof(command) - 1U)
      command[command_length++] = (char)value;
    else command_overflow = 1U;
  }
}

void HMI_Bridge_Poll(void) {
  if (host.fault != 0U) {
    command_length = end_count = command_overflow = 0U;
    Recover(&huart2, &host);
  }
  if (screen.fault != 0U) Recover(&huart3, &screen);
  /* 每轮交替处理两端，接收中断可在阻塞发送期间继续填充队列。 */
  for (unsigned i = 0U; i < 16U; ++i) {
    if (host.tail != host.head) {
      const uint8_t value = host.data[host.tail];
      host.tail = (uint16_t)((host.tail + 1U) % sizeof(host.data));
      if (HAL_UART_Transmit(&huart3, &value, 1U, 20U) == HAL_OK)
        TrackCommand(value);
      else {
        ++hmi_bridge_tx_errors;
        command_overflow = 1U;
      }
    }
    if (screen.tail != screen.head) {
      const uint8_t value = screen.data[screen.tail];
      screen.tail = (uint16_t)((screen.tail + 1U) % sizeof(screen.data));
      if (HAL_UART_Transmit(&huart2, &value, 1U, 20U) != HAL_OK)
        ++hmi_bridge_tx_errors;
    }
  }
}
