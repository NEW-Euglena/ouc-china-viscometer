#include "hmi_screen.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>

/* 查询页面并验证指令执行，不改动屏幕波特率、页面或持久设置。 */
static const char *const queries[] = {"sendme", "printh A5 5A 3C C3"};
static const uint8_t proof[] = {0xa5U, 0x5aU, 0x3cU, 0xc3U};
static uint8_t tx_buffer[128], rx_byte, rx_data[64];
static volatile uint8_t rx_count, rx_fault, rx_overflow;
static volatile uint16_t rx_errors;
static uint8_t phase, query_index;
static uint16_t tx_length;
static uint32_t phase_time;

static void Debug(const char *message) {
  HAL_UART_Transmit(&huart2, (const uint8_t *)message,
                    (uint16_t)strlen(message), 100U);
}

static void ArmReceiver(void) {
  __HAL_UART_CLEAR_FLAG(&huart3, UART_CLEAR_OREF | UART_CLEAR_NEF |
                                  UART_CLEAR_FEF | UART_CLEAR_PEF);
  __HAL_UART_SEND_REQ(&huart3, UART_RXDATA_FLUSH_REQUEST);
  rx_fault = 0U;
  if (HAL_UART_Receive_IT(&huart3, &rx_byte, 1U) != HAL_OK) {
    rx_fault = 1U;
    ++rx_errors;
  }
}

uint8_t HMI_Screen_SendCommand(const char *command) {
  if (command == NULL || command[0] == '\0' ||
      huart3.gState != HAL_UART_STATE_READY) return 0U;
  const size_t length = strlen(command);
  if (length > sizeof(tx_buffer) - 3U) return 0U;
  memcpy(tx_buffer, command, length);
  memset(tx_buffer + length, 0xff, 3U);
  tx_length = (uint16_t)(length + 3U);
  return HAL_UART_Transmit_IT(&huart3, tx_buffer, tx_length) == HAL_OK;
}

void HMI_Screen_Init(void) {
  phase = 0U;
  query_index = 0U;
  phase_time = HAL_GetTick();
  rx_count = rx_fault = rx_overflow = 0U;
  rx_errors = 0U;
  Debug("HMI TEST: PC10 TX -> screen RX; PC11 RX <- screen TX; 9600 8N1.\r\n"
        "HMI TEST: pumps/valves OFF; temperature/network/B1 paused.\r\n");
}

void HMI_Screen_RxCpltCallback(void) {
  if (rx_count < sizeof(rx_data)) rx_data[rx_count++] = rx_byte;
  else rx_overflow = 1U;
  if (HAL_UART_Receive_IT(&huart3, &rx_byte, 1U) != HAL_OK) {
    rx_fault = 1U;
    ++rx_errors;
  }
}

void HMI_Screen_ErrorCallback(void) {
  rx_fault = 1U;
  ++rx_errors;
}

static void ReportReply(void) {
  /* 接收已经停止，打印和判断期间中断不会修改本轮数据。 */
  char message[256];
  size_t used = (size_t)snprintf(message, sizeof(message),
      "HMI TEST RX: bytes=%u%s errors=%u HEX=", (unsigned)rx_count,
      rx_overflow != 0U ? "+" : "", (unsigned)rx_errors);
  for (uint8_t i = 0U; i < rx_count; ++i) {
    used += (size_t)snprintf(message + used, sizeof(message) - used,
                           "%02X ", rx_data[i]);
  }
  snprintf(message + used, sizeof(message) - used, "\r\n");
  Debug(message);
  if (rx_errors != 0U || rx_overflow != 0U) {
    Debug("HMI TEST: RX_ERROR; check wiring/power/baud; repeat query.\r\n");
    return;
  }
  for (uint8_t i = 0U; i < rx_count; ++i) {
    const uint8_t frame_start = i == 0U || (i >= 3U &&
        rx_data[i - 1U] == 0xffU && rx_data[i - 2U] == 0xffU &&
        rx_data[i - 3U] == 0xffU);
    if (frame_start == 0U) continue;
    if (query_index == 0U && (unsigned)i + 5U <= rx_count &&
        rx_data[i] == 0x66U && rx_data[i + 2U] == 0xffU &&
        rx_data[i + 3U] == 0xffU && rx_data[i + 4U] == 0xffU) {
      snprintf(message, sizeof(message), "HMI TEST: SCREEN_OK page=%u.\r\n",
               (unsigned)rx_data[i + 1U]);
      Debug(message);
      return;
    }
    if (query_index == 1U && (unsigned)i + sizeof(proof) <= rx_count &&
        memcmp(rx_data + i, proof, sizeof(proof)) == 0) {
      /* 返回的二进制标记不同于发出的 ASCII 指令，证明屏幕执行了指令。 */
      Debug("HMI TEST: SCREEN_OK proof=printh.\r\n");
      return;
    }
  }
  if (rx_count == tx_length && memcmp(rx_data, tx_buffer, tx_length) == 0) {
    Debug("HMI TEST: TX_ECHO; received exact query bytes; source undetermined.\r\n");
  } else if (rx_count == 0U) {
    Debug("HMI TEST: NO_REPLY; check screen TX, common GND and 9600 baud.\r\n");
  } else {
    Debug("HMI TEST: UNEXPECTED_REPLY; see HEX data above.\r\n");
  }
}

void HMI_Screen_Poll(void) {
  if (phase == 1U && rx_fault != 0U) {
    HAL_UART_AbortReceive(&huart3);
    ArmReceiver();
  }
  const uint32_t elapsed = HAL_GetTick() - phase_time;
  if (phase == 1U && elapsed >= 1000U) {
    HAL_UART_AbortReceive(&huart3);
    ReportReply();
    query_index ^= 1U;
    phase = 2U;
    phase_time = HAL_GetTick();
  } else if (phase != 1U && elapsed >= 2000U) {
    HAL_UART_AbortReceive(&huart3);
    rx_count = rx_overflow = 0U;
    rx_errors = 0U;
    ArmReceiver();
    phase = 1U;
    phase_time = HAL_GetTick();
    Debug(query_index == 0U ?
        "HMI TEST TX: 73 65 6E 64 6D 65 FF FF FF (sendme).\r\n" :
        "HMI TEST TX: printh A5 5A 3C C3 + FF FF FF; expect A5 5A 3C C3.\r\n");
    if (HMI_Screen_SendCommand(queries[query_index]) == 0U) {
      HAL_UART_AbortReceive(&huart3);
      Debug("HMI TEST: TX_FAILED; query was not sent.\r\n");
      phase = 2U;
    }
  }
}
