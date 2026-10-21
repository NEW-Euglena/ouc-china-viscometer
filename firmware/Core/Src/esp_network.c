#include "esp_network.h"
#include "usart.h"
#include "hmi_screen.h"
#ifdef HMI_SERIAL_BRIDGE
#include "hmi_serial_bridge.h"
#endif
#include "network_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_include("network_config.local.h")
#include "network_config.local.h"
#else
/* 未填写配置时只检查固件和指令，不尝试连接网络。 */
#define NETWORK_WIFI_SSID ""
#define NETWORK_WIFI_PASSWORD ""
#define NETWORK_SERVER_IP "127.0.0.1"
#define NETWORK_SERVER_PORT 8080U
#endif

typedef enum {
  BOOT_WAIT, AT_WAIT, ECHO_WAIT, VERSION_WAIT, MODE_WAIT, MUX_WAIT,
  NORMAL_MODE_WAIT, JOIN_WAIT, IP_WAIT, IDLE, OPEN_WAIT, PROMPT_WAIT,
  SEND_WAIT, HTTP_WAIT, CLOSE_WAIT, RETRY_WAIT, DISABLED
} NetworkState;

/* 中断写入、主循环读取；容量足以保存温度转换期间的短响应。 */
static uint8_t rx_ring[2048];
static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;
static volatile uint8_t rx_fault;
static uint8_t rx_byte;
static NetworkState state;
static uint32_t state_time;
static uint32_t state_timeout;
static uint32_t last_upload;
static uint8_t sensor_status = 1U;
static int32_t temperature_centi;
static uint8_t pump_enabled;
static char tx_buffer[896];
static char request[768];
static uint16_t request_length;
static char line[256];
static uint16_t line_length;
static uint32_t ipd_remaining;
static char http_line[96];
static uint16_t http_length;
static uint8_t http_finished;
static uint8_t got_ok, got_error, got_prompt, got_send_ok;
static uint8_t got_http_ok, got_http_error, disconnected, restarted;
static NetworkConfig active_config, pending_config;
static uint8_t config_pending;
static uint8_t measurement_active;
static uint8_t measurement_available;
static LiquidMeasurement measurement_snapshot;
static char usb_telemetry[544];
static uint32_t usb_telemetry_time;
static char pending_id[9], active_id[9];
/* USB 配置只接收一条有界帧，串口日志继续由 USART2 输出。 */
static uint8_t config_rx[512], config_byte;
static volatile uint16_t config_head, config_tail;
static volatile uint8_t config_fault;
static char config_line[256];
static uint16_t config_length;
static uint8_t config_discard;
static uint32_t config_started;

static void Debug(const char *message) {
  HAL_UART_Transmit(&huart2, (const uint8_t *)message,
                    (uint16_t)strlen(message), 100U);
}

static void ClearEvents(void) {
  got_ok = got_error = got_prompt = got_send_ok = 0U;
}

static void Enter(NetworkState next, uint32_t timeout) {
  state = next;
  state_time = HAL_GetTick();
  state_timeout = timeout;
}

/* 发送缓冲区保留到中断发送完成，不在栈上存放待发送数据。 */
static void Send(const char *data, NetworkState next, uint32_t timeout) {
  const size_t length = strlen(data);
  ClearEvents();
  if (length >= sizeof(tx_buffer)) {
    got_error = 1U;
  } else {
    memmove(tx_buffer, data, length + 1U);
    if (HAL_UART_Transmit_IT(&huart1, (uint8_t *)tx_buffer,
                             (uint16_t)length) != HAL_OK) {
      got_error = 1U;
    }
  }
  Enter(next, timeout);
}

static void Retry(void) {
  Debug("Network unavailable; retry in 30 seconds.\r\n");
  Enter(RETRY_WAIT, 30000U);
}

static void CloseConnection(void) {
  Send("AT+CIPCLOSE\r\n", CLOSE_WAIT, 5000U);
}

static void ParseLine(void) {
  line[line_length] = '\0';
  if (strcmp(line, "OK") == 0) got_ok = 1U;
  else if (strcmp(line, "ERROR") == 0 || strcmp(line, "FAIL") == 0 ||
           strcmp(line, "SEND FAIL") == 0 || strncmp(line, "busy", 4U) == 0)
    got_error = 1U;
  else if (strcmp(line, "SEND OK") == 0) got_send_ok = 1U;
  else if (strcmp(line, "WIFI DISCONNECT") == 0) disconnected = 1U;
  else if (strcmp(line, "ready") == 0) restarted = 1U;

  /* 仅打印已知诊断行；不能打印可能含密码的命令回显。 */
  if (state == VERSION_WAIT &&
      (strncmp(line, "AT version:", 11U) == 0 ||
       strncmp(line, "SDK version:", 12U) == 0 ||
       strncmp(line, "compile time:", 13U) == 0)) {
    Debug(line);
    Debug("\r\n");
  }
  if (state == IP_WAIT && strncmp(line, "+CIFSR:STAIP,", 13U) == 0) {
    Debug(line);
    Debug("\r\n");
  }
  line_length = 0U;
}

static void ParseByte(uint8_t byte) {
  if (ipd_remaining != 0U) {
    --ipd_remaining;
    if (http_finished == 0U) {
      if (byte == '\n') {
        http_line[http_length] = '\0';
        /* 只接受 HTTP 状态行中的 204，不搜索任意文本中的数字。 */
        got_http_ok = (strncmp(http_line, "HTTP/1.1 204 ", 13U) == 0 ||
                       strncmp(http_line, "HTTP/1.0 204 ", 13U) == 0);
        got_http_error = (got_http_ok == 0U);
        http_finished = 1U;
      } else if (byte != '\r') {
        if (http_length < sizeof(http_line) - 1U)
          http_line[http_length++] = (char)byte;
        else rx_fault = 1U;
      }
    }
    return;
  }
  if (byte == '>' && line_length == 0U) {
    got_prompt = 1U;
    return;
  }
  if (byte == '\r') return;
  if (byte == '\n') {
    if (line_length != 0U) ParseLine();
    return;
  }
  /* ESP 在提示符后可能输出空格。 */
  if (byte == ' ' && line_length == 0U) return;
  if (byte == ':' && line_length >= 6U && strncmp(line, "+IPD,", 5U) == 0) {
    char *end;
    line[line_length] = '\0';
    const unsigned long count = strtoul(line + 5, &end, 10);
    if (*end != '\0' || count == 0U || count > 1024U) rx_fault = 1U;
    else ipd_remaining = (uint32_t)count;
    line_length = 0U;
    return;
  }
  if (line_length < sizeof(line) - 1U) line[line_length++] = (char)byte;
  else rx_fault = 1U;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart) {
#ifdef HMI_SERIAL_BRIDGE
  HMI_Bridge_RxCpltCallback(uart);
#else
  if (uart->Instance == USART3) {
    HMI_Screen_RxCpltCallback();
    return;
  }
  if (uart->Instance == USART2) {
    const uint16_t next = (uint16_t)((config_head + 1U) % sizeof(config_rx));
    if (next == config_tail) config_fault = 1U;
    else { config_rx[config_head] = config_byte; config_head = next; }
    if (HAL_UART_Receive_IT(&huart2, &config_byte, 1U) != HAL_OK) config_fault = 1U;
    return;
  }
  if (uart->Instance != USART1) return;
  const uint16_t next = (uint16_t)((rx_head + 1U) % sizeof(rx_ring));
  if (next == rx_tail) rx_fault = 1U;
  else {
    rx_ring[rx_head] = rx_byte;
    rx_head = next;
  }
  if (HAL_UART_Receive_IT(&huart1, &rx_byte, 1U) != HAL_OK) rx_fault = 1U;
#endif
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart) {
#ifdef HMI_SERIAL_BRIDGE
  HMI_Bridge_ErrorCallback(uart);
#else
  if (uart->Instance == USART1) rx_fault = 1U;
  else if (uart->Instance == USART2) config_fault = 1U;
  else if (uart->Instance == USART3) HMI_Screen_ErrorCallback();
#endif
}

static void ResetReceiver(void) {
  HAL_UART_AbortReceive(&huart1);
  __HAL_UART_CLEAR_FLAG(&huart1, UART_CLEAR_OREF | UART_CLEAR_NEF |
                                UART_CLEAR_FEF | UART_CLEAR_PEF);
  __HAL_UART_SEND_REQ(&huart1, UART_RXDATA_FLUSH_REQUEST);
  rx_head = rx_tail = 0U;
  rx_fault = 0U;
  line_length = http_length = 0U;
  ipd_remaining = 0U;
  http_finished = got_http_ok = got_http_error = 0U;
  ClearEvents();
  if (HAL_UART_Receive_IT(&huart1, &rx_byte, 1U) != HAL_OK) rx_fault = 1U;
}

void ESP_Network_Init(void) {
  usb_telemetry_time = HAL_GetTick();
  measurement_active = 0U;
  measurement_available = 0U;
  memset(&active_config, 0, sizeof(active_config));
  if (!NetworkConfig_Load(&active_config)) {
    snprintf(active_config.ssid, sizeof(active_config.ssid), "%s", NETWORK_WIFI_SSID);
    snprintf(active_config.password, sizeof(active_config.password), "%s", NETWORK_WIFI_PASSWORD);
    snprintf(active_config.server_ip, sizeof(active_config.server_ip), "%s", NETWORK_SERVER_IP);
    active_config.server_port = NETWORK_SERVER_PORT;
  }
  config_head = config_tail = config_length = 0U;
  config_fault = config_pending = config_discard = 0U;
  active_id[0] = pending_id[0] = '\0';
  if (HAL_UART_Receive_IT(&huart2, &config_byte, 1U) != HAL_OK) config_fault = 1U;
  ResetReceiver();
  Enter(BOOT_WAIT, 2000U);
}

static void ConfigReply(const char *result, const char *request_id, const char *reason) {
  char reply[80];
  snprintf(reply, sizeof(reply), "NETCFG1 %s %s%s%s\r\n", result,
           request_id[0] == '\0' ? "00000000" : request_id,
           reason[0] == '\0' ? "" : " ", reason);
  Debug(reply);
}

static void PollConfig(void) {
  /* 异步遥测完成后再处理配置，保证配置应答能使用同一串口。 */
  if (huart2.gState != HAL_UART_STATE_READY) return;
  if (config_fault != 0U) {
    HAL_UART_AbortReceive(&huart2);
    __HAL_UART_CLEAR_FLAG(&huart2, UART_CLEAR_OREF | UART_CLEAR_NEF |
                                  UART_CLEAR_FEF | UART_CLEAR_PEF);
    __HAL_UART_SEND_REQ(&huart2, UART_RXDATA_FLUSH_REQUEST);
    config_tail = config_head;
    config_fault = 0U;
    config_length = 0U;
    config_discard = 1U;
    if (HAL_UART_Receive_IT(&huart2, &config_byte, 1U) != HAL_OK) config_fault = 1U;
    ConfigReply("ERROR", "", "RX");
  }
  if (config_length != 0U && (HAL_GetTick() - config_started) >= 5000U) {
    config_length = 0U;
    config_discard = 1U;
    ConfigReply("ERROR", "", "INCOMPLETE");
  }
  while (config_tail != config_head && config_fault == 0U) {
    const uint8_t byte = config_rx[config_tail];
    config_tail = (uint16_t)((config_tail + 1U) % sizeof(config_rx));
    if (byte == '\r') continue;
    if (byte == '\n') {
      if (config_discard == 0U && config_length != 0U) {
        NetworkConfig decoded;
        char request_id[9];
        config_line[config_length] = '\0';
        if (config_length == 22U && strncmp(config_line, "NETCFG1 HELLO ", 14U) == 0 &&
            strspn(config_line + 14, "0123456789abcdefABCDEF") == 8U)
          ConfigReply("READY", config_line + 14, "");
        else if (!NetworkConfig_Parse(config_line, &decoded, request_id))
          ConfigReply("ERROR", request_id, "INVALID");
        else if (config_pending) ConfigReply("ERROR", request_id, "BUSY");
        else {
          pending_config = decoded;
          strcpy(pending_id, request_id);
          config_pending = 1U;
        }
      }
      memset(config_line, 0, sizeof(config_line));
      config_discard = 0U;
      config_length = 0U;
    } else if (!config_discard) {
      if (config_length == 0U) config_started = HAL_GetTick();
      if (byte == 0U || config_length >= sizeof(config_line) - 1U) {
        config_length = 0U;
        config_discard = 1U;
        ConfigReply("ERROR", "", "INVALID");
      } else config_line[config_length++] = (char)byte;
    }
  }
}

static uint8_t ApplyPendingConfig(void) {
  /* 在事务结束且 USART1 已完成发送后擦写，避免截断 ESP 请求。 */
  if (measurement_active || !config_pending || (state != IDLE && state != RETRY_WAIT && state != DISABLED) ||
      huart1.gState != HAL_UART_STATE_READY) return 0U;
  config_pending = 0U;
  if (!NetworkConfig_Save(&pending_config)) {
    ConfigReply("ERROR", pending_id, "FLASH");
    memset(&pending_config, 0, sizeof(pending_config));
    return 0U;
  }
  active_config = pending_config;
  memset(&pending_config, 0, sizeof(pending_config));
  strcpy(active_id, pending_id);
  ConfigReply("SAVED", active_id, "");
  restarted = disconnected = 0U;
  ResetReceiver();
  Enter(BOOT_WAIT, 2000U);
  return 1U;
}

void ESP_Network_SetTemperature(uint8_t status, int32_t value) {
  sensor_status = status;
  temperature_centi = value;
}

void ESP_Network_SetPump(uint8_t enabled) {
  pump_enabled = enabled;
}

void ESP_Network_SetMeasurementActive(uint8_t active) {
  measurement_active = active != 0U;
}

void ESP_Network_SetMeasurement(const LiquidMeasurement *measurement) {
  measurement_snapshot.stage = measurement->stage;
  measurement_snapshot.error = measurement->error;
  measurement_snapshot.levels[0].stable = measurement->levels[0].stable;
  measurement_snapshot.levels[1].stable = measurement->levels[1].stable;
  measurement_snapshot.upper_triggered = measurement->upper_triggered;
  measurement_snapshot.lower_triggered = measurement->lower_triggered;
  measurement_snapshot.start_time = measurement->start_time;
  measurement_snapshot.result_valid = measurement->result_valid;
  measurement_snapshot.result_ms = measurement->result_ms;
  measurement_available = 1U;
}

/* 对 AT 参数转义，支持 SSID 和密码中的引号、逗号和反斜线。 */
static size_t EscapeAt(char *output, const char *input) {
  size_t length = 0U;
  while (*input != '\0') {
    if (*input == '"' || *input == ',' || *input == '\\') output[length++] = '\\';
    output[length++] = *input++;
  }
  output[length] = '\0';
  return length;
}

static void JoinWifi(void) {
  char ssid[65], password[129];
  EscapeAt(ssid, active_config.ssid);
  EscapeAt(password, active_config.password);
  snprintf(tx_buffer, sizeof(tx_buffer), "AT+CWJAP_CUR=\"%s\",\"%s\"\r\n", ssid, password);
  Debug("Connecting Wi-Fi...\r\n");
  disconnected = 0U;
  Send(tx_buffer, JOIN_WAIT, 20000U);
}

static int FormatTelemetry(char *body, size_t capacity) {
  char temperature[16];
  char measurement_json[300] = "";
  const char *status = sensor_status == 0U ? "ok" :
                       sensor_status == 1U ? "not_found" : "crc_error";
  if (sensor_status == 0U)
    snprintf(temperature, sizeof(temperature), "%ld", (long)temperature_centi);
  else strcpy(temperature, "null");
  const char *enabled = pump_enabled != 0U ? "true" : "false";
  if (measurement_available) {
    static const char *const stages[] = {"idle", "suction", "extra_suction", "draining",
      "timing", "complete", "aborted", "fault"};
    static const char *const errors[] = {"none", "upper_wet", "invalid_order", "edge_overflow",
      "suction_timeout", "upper_timeout", "lower_timeout", "unstable_input"};
    const LiquidMeasurement *m = &measurement_snapshot;
    char result[16];
    if (m->result_valid) snprintf(result, sizeof(result), "%lu", (unsigned long)m->result_ms);
    else strcpy(result, "null");
    snprintf(measurement_json, sizeof(measurement_json),
        ",\"measurement\":{\"stage\":\"%s\",\"error\":\"%s\",\"upper_wet\":%s,\"lower_wet\":%s,"
        "\"upper_triggered\":%s,\"lower_triggered\":%s,\"elapsed_ms\":%lu,\"result_ms\":%s}",
        stages[m->stage], errors[m->error], m->levels[0].stable ? "true" : "false",
        m->levels[1].stable ? "true" : "false", m->upper_triggered ? "true" : "false",
        m->lower_triggered ? "true" : "false", (unsigned long)(m->stage == LIQUID_TIMING ?
        HAL_GetTick() - m->start_time : m->result_valid ? m->result_ms : 0U), result);
  }
  return snprintf(body, capacity,
      "{\"device_id\":\"nucleo-g071rb\",\"uptime_ms\":%lu,"
      "\"temperature_centi\":%s,\"sensor_status\":\"%s\","
      "\"pump_enabled\":%s,\"valve_enabled\":%s%s}",
      (unsigned long)HAL_GetTick(), temperature, status, enabled, enabled, measurement_json);
}

void ESP_Network_PollUsbTelemetry(void) {
  const uint32_t now = HAL_GetTick();
  if (now - usb_telemetry_time < 1000U || huart2.gState != HAL_UART_STATE_READY ||
      config_pending || config_length || config_head != config_tail) return;
  /* 缓冲区保持到发送完成；发送期间主循环继续响应液位及中止。 */
  memcpy(usb_telemetry, "Telemetry: ", 11U);
  const int length = FormatTelemetry(usb_telemetry + 11U, sizeof(usb_telemetry) - 13U);
  if (length <= 0 || length >= (int)sizeof(usb_telemetry) - 13) return;
  memcpy(usb_telemetry + 11U + length, "\r\n", 2U);
  if (HAL_UART_Transmit_IT(&huart2, (uint8_t *)usb_telemetry, (uint16_t)(length + 13)) == HAL_OK)
    usb_telemetry_time = now;
}

static void StartUpload(void) {
  char body[512];
  const int body_length = FormatTelemetry(body, sizeof(body));
  const int length = snprintf(request, sizeof(request),
      "POST /api/telemetry HTTP/1.1\r\nHost: %s:%u\r\n"
      "Content-Type: application/json\r\nContent-Length: %d\r\n"
      "Connection: close\r\n\r\n%s", active_config.server_ip,
      (unsigned int)active_config.server_port, body_length, body);
  if (body_length <= 0 || body_length >= (int)sizeof(body) ||
      length <= 0 || length >= (int)sizeof(request)) {
    Debug("Telemetry too large; upload skipped.\r\n");
    return;
  }
  request_length = (uint16_t)length;
  got_http_ok = got_http_error = http_finished = 0U;
  http_length = 0U;
  snprintf(tx_buffer, sizeof(tx_buffer), "AT+CIPSTART=\"TCP\",\"%s\",%u\r\n",
           active_config.server_ip, (unsigned int)active_config.server_port);
  Send(tx_buffer, OPEN_WAIT, 5000U);
}

void ESP_Network_Poll(void) {
  PollConfig();
  while (rx_tail != rx_head && rx_fault == 0U) {
    const uint8_t byte = rx_ring[rx_tail];
    rx_tail = (uint16_t)((rx_tail + 1U) % sizeof(rx_ring));
    ParseByte(byte);
  }
  if (rx_fault != 0U) {
    Debug("USART1 receive error/overflow; upload not confirmed.\r\n");
    ResetReceiver();
    /* 已收到发送提示时，补齐请求再关闭，避免停留在数据输入状态。 */
    if (state == PROMPT_WAIT) {
      Send(request, SEND_WAIT, 5000U);
      got_http_error = 1U;
    } else if (state == SEND_WAIT) {
      /* 中断发送继续完成，只将本次事务标记为无法确认。 */
      got_http_error = 1U;
    } else if (state >= OPEN_WAIT && state <= CLOSE_WAIT) CloseConnection();
    else Retry();
    return;
  }
  if (restarted != 0U) {
    restarted = disconnected = 0U;
    ResetReceiver();
    Debug("ESP restarted; checking AT firmware again.\r\n");
    Enter(BOOT_WAIT, 2000U);
    return;
  }
  if (disconnected != 0U) {
    disconnected = 0U;
    /* 加入网络失败时可能同时收到 DISCONNECT，仍等待 JOIN 的最终结果。 */
    if (state >= IP_WAIT && state <= CLOSE_WAIT) {
      Debug("Wi-Fi disconnected.\r\n");
      Retry();
      return;
    }
  }
  if (ApplyPendingConfig()) return;
  const uint32_t now = HAL_GetTick();
  const uint8_t timeout = ((now - state_time) >= state_timeout);
  switch (state) {
    case BOOT_WAIT:
      if (timeout) {
        Debug("Testing ESP-01S...\r\nTX: AT\r\n");
        Send("AT\r\n", AT_WAIT, 2000U);
      }
      break;
    case AT_WAIT:
      if (got_ok) {
        Debug("ESP-01S detected successfully\r\n");
        Send("ATE0\r\n", ECHO_WAIT, 2000U);
      } else if (got_error || timeout) {
        Debug("ESP-01S no response\r\n"); Retry();
      }
      break;
    case ECHO_WAIT:
      if (got_ok) Send("AT+GMR\r\n", VERSION_WAIT, 2000U);
      else if (got_error || timeout) Retry();
      break;
    case VERSION_WAIT:
      if (got_ok) Send("AT+CWMODE_CUR=1\r\n", MODE_WAIT, 2000U);
      else if (got_error || timeout) Retry();
      break;
    case MODE_WAIT:
      if (got_ok) Send("AT+CIPMUX=0\r\n", MUX_WAIT, 2000U);
      else if (got_error || timeout) Retry();
      break;
    case MUX_WAIT:
      if (got_ok) Send("AT+CIPMODE=0\r\n", NORMAL_MODE_WAIT, 2000U);
      else if (got_error || timeout) Retry();
      break;
    case NORMAL_MODE_WAIT:
      if (got_ok) {
        if (active_config.ssid[0] == '\0') {
          Debug("Network config missing; local controls remain active.\r\n");
          Enter(DISABLED, 0U);
        } else JoinWifi();
      } else if (got_error || timeout) Retry();
      break;
    case JOIN_WAIT:
      if (got_ok) {
        Debug("Wi-Fi connected.\r\n");
        Send("AT+CIFSR\r\n", IP_WAIT, 2000U);
      } else if (got_error || timeout) Retry();
      break;
    case IP_WAIT:
      if (got_ok) {
        last_upload = now - 5000U;
        Enter(IDLE, 0U);
      } else if (got_error || timeout) Retry();
      break;
    case IDLE:
      if ((now - last_upload) >= 5000U) {
        last_upload = now;
        StartUpload();
      }
      break;
    case OPEN_WAIT:
      if (got_error) { Debug("TCP connect failed.\r\n"); CloseConnection(); }
      else if (got_ok) {
        snprintf(tx_buffer, sizeof(tx_buffer), "AT+CIPSEND=%u\r\n", request_length);
        Send(tx_buffer, PROMPT_WAIT, 5000U);
      } else if (timeout) { Debug("TCP connect timeout.\r\n"); CloseConnection(); }
      break;
    case PROMPT_WAIT:
      if (got_prompt) Send(request, SEND_WAIT, 5000U);
      else if (got_error) { Debug("Send request rejected.\r\n"); CloseConnection(); }
      else if (timeout) {
        Debug("Send prompt timeout; upload not confirmed.\r\n");
        /* 提示符丢失时仍补齐已声明的数据长度，随后关闭连接。 */
        Send(request, SEND_WAIT, 5000U);
        got_http_error = 1U;
      }
      break;
    case SEND_WAIT:
      if (got_error || timeout) { Debug("Send failed/timeout.\r\n"); CloseConnection(); }
      else if (got_send_ok) Enter(HTTP_WAIT, 5000U);
      break;
    case HTTP_WAIT:
      if (got_http_error) { Debug("HTTP response invalid; upload failed.\r\n"); CloseConnection(); }
      else if (got_http_ok) {
        Debug("Upload successful: HTTP 204\r\n");
        if (active_id[0] != '\0') {
          ConfigReply("UPLOADED", active_id, "");
          active_id[0] = '\0';
        }
        CloseConnection();
      }
      else if (timeout) { Debug("HTTP response timeout; upload not confirmed.\r\n"); CloseConnection(); }
      break;
    case CLOSE_WAIT:
      if (got_ok || got_error || timeout) Enter(IDLE, 0U);
      break;
    case RETRY_WAIT:
      if (timeout) { ResetReceiver(); Enter(BOOT_WAIT, 2000U); }
      break;
    case DISABLED:
      break;
  }
}
