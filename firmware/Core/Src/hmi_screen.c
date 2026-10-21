#include "hmi_screen.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>

typedef enum {
  HMI_BOOT_WAIT, HMI_REPLY_WAIT, HMI_CHECK_DONE, HMI_UI_IDLE, HMI_UI_WAIT
} HMI_State;
static volatile HMI_State state;
static uint32_t state_time;
/* 发送缓冲区保留到 UART 中断发送完成。 */
static uint8_t tx_buffer[128];
static uint8_t rx_byte, rx_frame[16], rx_length, rx_end_count;
static volatile uint8_t rx_fault, page_received, page_id;
static volatile uint8_t reply_received, reply_code, restarted;
static uint8_t sensor_status, pump_enabled, dirty;
static int32_t temperature_centi;
static uint32_t sample_time;
static uint8_t ui_index, batch_active, ui_initialized, batch_sensor, batch_pump;
static char batch_temperature[16];
static uint8_t measurement_set, result_valid, batch_measurement;
static LiquidStage measurement_stage;
static LiquidError measurement_error;
static uint32_t result_ms;
static char batch_stage[40], batch_result[24];
static unsigned batch_stage_color;
static uint8_t levels_set, upper_wet, lower_wet, upper_triggered, lower_triggered;
static uint8_t batch_upper_wet, batch_lower_wet;
static char batch_upper[32], batch_lower[32];
/* 可通过调试器检查已获屏幕确认的完整刷新次数。 */
volatile uint32_t hmi_ui_updates;

/* 英文显示使用 ASCII 字符，与屏幕 gb2312 编码及原串口协议兼容。 */
static const char *const sensor_text[] = {
  "Temperature OK",                 /* 温度正常 */
  "No probe",         /* 未发现探头 */
  "Data error", /* 温度数据异常 */
  "Waiting",                 /* 等待采样 */
  "Stale data"                  /* 数据过期 */
};


static const char *const measurement_stage_text[] = {
  "Idle", /* 待机 */
  "Suction", /* 吸液 */
  "Extra suction", /* 续吸 */
  "Draining", /* 放流 */
  "Timing", /* 计时 */
  "Complete", /* 完成 */
  "Aborted", /* 中止 */
  "Fault", /* 故障 */
};

static const char *const measurement_error_text[] = {
  "Fault", /* 故障 */
  "Upper mark wet", /* 上方有液 */
  "Invalid sequence", /* 液位顺序异常 */
  "Event overflow", /* 液位事件溢出 */
  "Suction timeout", /* 吸液超时 */
  "Upper timeout", /* 上方下降超时 */
  "Lower timeout", /* 下方下降超时 */
  "Unstable level", /* 液位未稳定 */
};

static void Debug(const char *message) {
  HAL_UART_Transmit(&huart2, (const uint8_t *)message,
                    (uint16_t)strlen(message), 20U);
}

uint8_t HMI_Screen_SendCommand(const char *command) {
  if (command == NULL || command[0] == '\0' ||
      huart3.gState != HAL_UART_STATE_READY) return 0U;
  const size_t length = strlen(command);
  if (length > sizeof(tx_buffer) - 3U) return 0U;
  memcpy(tx_buffer, command, length);
  memset(tx_buffer + length, 0xff, 3U);
  return HAL_UART_Transmit_IT(&huart3, tx_buffer,
                            (uint16_t)(length + 3U)) == HAL_OK;
}

void HMI_Screen_Init(void) {
  state = HMI_BOOT_WAIT;
  state_time = HAL_GetTick();
  rx_length = rx_end_count = rx_fault = page_received = 0U;
  reply_received = restarted = 0U;
  sensor_status = 3U;
  pump_enabled = 0U;
  measurement_set = result_valid = levels_set = 0U;
  temperature_centi = 0;
  dirty = 1U;
  ui_index = batch_active = ui_initialized = 0U;
  hmi_ui_updates = 0U;
  if (HAL_UART_Receive_IT(&huart3, &rx_byte, 1U) != HAL_OK) rx_fault = 1U;
  Debug("HMI USART3 initialized: PC10 TX, PC11 RX, 9600 8N1.\r\n");
}

void HMI_Screen_SetTemperature(uint8_t status, int32_t centi) {
  sensor_status = status <= 2U ? status : 2U;
  temperature_centi = centi;
  sample_time = HAL_GetTick();
  dirty = 1U;
}

void HMI_Screen_SetPump(uint8_t enabled) {
  pump_enabled = enabled != 0U;
  dirty = 1U;
}

void HMI_Screen_SetLiquidStatus(uint8_t upper, uint8_t lower,
                                uint8_t started, uint8_t ended) {
  levels_set = 1U;
  upper_wet = upper != 0U;
  lower_wet = lower != 0U;
  upper_triggered = started != 0U;
  lower_triggered = ended != 0U;
  dirty = 1U;
}

void HMI_Screen_SetMeasurement(LiquidStage stage, LiquidError error,
                               uint8_t valid, uint32_t milliseconds) {
  measurement_set = 1U;
  measurement_stage = stage;
  measurement_error = error;
  result_valid = valid;
  result_ms = milliseconds;
  dirty = 1U;
}

void HMI_Screen_RxCpltCallback(void) {
  if (rx_length < sizeof(rx_frame)) rx_frame[rx_length++] = rx_byte;
  rx_end_count = rx_byte == 0xffU ? (uint8_t)(rx_end_count + 1U) : 0U;
  if (rx_end_count == 3U) {
    /* 只用完整的页面回复确认通信，启动通知和错误回复不能冒充成功。 */
    if (rx_length == 5U && rx_frame[0] == 0x66U &&
        state == HMI_REPLY_WAIT && rx_fault == 0U) {
      page_id = rx_frame[1];
      page_received = 1U;
    }
    if (rx_length == 4U && state == HMI_UI_WAIT && rx_fault == 0U) {
      reply_code = rx_frame[0];
      reply_received = 1U;
    }
    if (rx_length == 4U && rx_frame[0] == 0x88U && state != HMI_REPLY_WAIT)
      restarted = 1U;
    rx_length = rx_end_count = 0U;
  }
  if (HAL_UART_Receive_IT(&huart3, &rx_byte, 1U) != HAL_OK) rx_fault = 1U;
}

void HMI_Screen_ErrorCallback(void) {
  rx_fault = 1U;
}

static uint8_t CurrentSensorStatus(void) {
  if (sensor_status != 3U && HAL_GetTick() - sample_time >= 5000U) return 4U;
  return sensor_status;
}

static void TakeSnapshot(void) {
  batch_sensor = CurrentSensorStatus();
  batch_pump = pump_enabled;
  batch_measurement = measurement_set;
  if (batch_measurement) {
    strcpy(batch_stage, measurement_stage == LIQUID_FAULT ?
        measurement_error_text[measurement_error] : measurement_stage_text[measurement_stage]);
    batch_stage_color = measurement_stage == LIQUID_FAULT ? 62826U :
                        measurement_stage == LIQUID_COMPLETE ? 15954U : 61343U;
    if (result_valid || measurement_stage == LIQUID_TIMING)
      snprintf(batch_result, sizeof(batch_result), "%lu.%03lu s",
               (unsigned long)(result_ms / 1000U), (unsigned long)(result_ms % 1000U));
    else strcpy(batch_result, "--.--- s");
  }
  if (!batch_measurement) strcpy(batch_result, "--.--- s");
  batch_upper_wet = levels_set && upper_wet;
  batch_lower_wet = levels_set && lower_wet;
  if (levels_set) {
    snprintf(batch_upper, sizeof(batch_upper), "%s %s", upper_wet ? "Wet" : "Dry",
             upper_triggered ? "Started" : "Pending");
    snprintf(batch_lower, sizeof(batch_lower), "%s %s", lower_wet ? "Wet" : "Dry",
             lower_triggered ? "Finished" : "Pending");
  } else {
    strcpy(batch_upper, "Waiting");
    strcpy(batch_lower, "Waiting");
  }
  if (batch_sensor == 0U) {
    const int32_t absolute = temperature_centi < 0 ? -temperature_centi : temperature_centi;
    snprintf(batch_temperature, sizeof(batch_temperature), "%s%ld.%02ld",
             temperature_centi < 0 ? "-" : "", (long)(absolute / 100),
             (long)(absolute % 100));
  } else {
    strcpy(batch_temperature, "--.--");
  }
  dirty = 0U;
  batch_active = 1U;
  ui_index = ui_initialized != 0U ? 1U : 0U;
}

static void SendUiCommand(void) {
  char command[80];
  const char *running = batch_pump != 0U ? "On" : "Off";
  switch (ui_index) {
    case 0: strcpy(command, "bkcmd=3"); break;
    case 1: snprintf(command, sizeof(command), "tTemp.txt=\"%s\"", batch_temperature); break;
    case 2: snprintf(command, sizeof(command), "tStage.txt=\"%s\"", batch_measurement ? batch_stage :
        batch_pump != 0U ? "Pump running" : "Idle"); break;
    case 3: snprintf(command, sizeof(command), "tRun.txt=\"%s\"", running); break;
    case 4: snprintf(command, sizeof(command), "tUpper.txt=\"%s\"", batch_upper); break;
    case 5: snprintf(command, sizeof(command), "tLower.txt=\"%s\"", batch_lower); break;
    case 6: snprintf(command, sizeof(command), "tSensor.txt=\"%s\"", sensor_text[batch_sensor]); break;
    case 7: snprintf(command, sizeof(command), "tTime.txt=\"%s\"", batch_result); break;
    case 8: snprintf(command, sizeof(command), "tStage.pco=%u", batch_measurement ? batch_stage_color :
        batch_pump != 0U ? 16028U : 61343U); break;
    case 9: snprintf(command, sizeof(command), "tRun.pco=%u", batch_pump != 0U ? 16028U : 38167U); break;
    case 10: snprintf(command, sizeof(command), "tUpper.pco=%u", batch_upper_wet ? 15954U : 61343U); break;
    case 11: snprintf(command, sizeof(command), "tLower.pco=%u", batch_lower_wet ? 15954U : 61343U); break;
    default: snprintf(command, sizeof(command), "tSensor.pco=%u",
        batch_sensor == 0U ? 15954U : batch_sensor == 3U ? 38167U : 62826U); break;
  }
  reply_received = 0U;
  state = HMI_UI_WAIT;
  state_time = HAL_GetTick();
  if (HMI_Screen_SendCommand(command) == 0U) state = HMI_UI_IDLE;
}

void HMI_Screen_Poll(void) {
  if (rx_fault != 0U) {
    HAL_UART_AbortReceive(&huart3);
    __HAL_UART_CLEAR_FLAG(&huart3, UART_CLEAR_OREF | UART_CLEAR_NEF |
                                  UART_CLEAR_FEF | UART_CLEAR_PEF);
    __HAL_UART_SEND_REQ(&huart3, UART_RXDATA_FLUSH_REQUEST);
    rx_length = rx_end_count = page_received = reply_received = 0U;
    rx_fault = 0U;
    if (HAL_UART_Receive_IT(&huart3, &rx_byte, 1U) != HAL_OK) rx_fault = 1U;
    Debug("HMI receive error; check wiring and baud rate.\r\n");
  }
  if (restarted != 0U) {
    restarted = 0U;
    state = HMI_BOOT_WAIT;
    state_time = HAL_GetTick();
    ui_initialized = batch_active = 0U;
    dirty = 1U;
  }
  if (state == HMI_BOOT_WAIT && HAL_GetTick() - state_time >= 2000U) {
    page_received = 0U;
    state = HMI_REPLY_WAIT;
    state_time = HAL_GetTick();
    /* 复位 MCU 可能打断上一条指令，先结束屏幕内的残留指令，再查询页面。 */
    if (HMI_Screen_SendCommand("\xff\xff\xff" "sendme") == 0U) {
      state = HMI_BOOT_WAIT;
      Debug("HMI query could not start.\r\n");
    }
  } else if (state == HMI_REPLY_WAIT) {
    if (page_received != 0U) {
      char message[48];
      snprintf(message, sizeof(message), "HMI communication OK; page=%u.\r\n",
               (unsigned)page_id);
      Debug(message);
      state = page_id == 0U ? HMI_UI_IDLE : HMI_CHECK_DONE;
    } else if (HAL_GetTick() - state_time >= 2000U) {
      Debug("HMI no valid reply; check power, TX/RX and 9600 baud.\r\n");
      state = HMI_BOOT_WAIT;
      state_time = HAL_GetTick();
    }
  } else if (state == HMI_UI_IDLE) {
    if (batch_active == 0U) {
      if (dirty == 0U && CurrentSensorStatus() == batch_sensor) return;
      TakeSnapshot();
    }
    SendUiCommand();
  } else if (state == HMI_UI_WAIT) {
    /* 中断可能在两次判断之间到达；本轮只读取一次完成标记。 */
    const uint8_t completed = reply_received;
    if (completed != 0U && reply_code == 1U) {
      state = HMI_UI_IDLE;
      if (++ui_index == 13U) {
        batch_active = 0U;
        if (ui_initialized == 0U) Debug("HMI status UI ready.\r\n");
        ui_initialized = 1U;
        ++hmi_ui_updates;
      }
    } else if (completed != 0U || HAL_GetTick() - state_time >= 1500U) {
      char message[48];
      if (completed != 0U)
        snprintf(message, sizeof(message), "HMI UI command error: 0x%02X.\r\n", reply_code);
      else
        strcpy(message, "HMI UI command timed out.\r\n");
      Debug(message);
      state = HMI_BOOT_WAIT;
      state_time = HAL_GetTick();
      ui_initialized = batch_active = 0U;
      dirty = 1U;
    }
  }
}
