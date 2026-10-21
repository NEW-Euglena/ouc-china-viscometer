/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "gpio.h"
#include "usart.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "esp_network.h"
#include "hmi_screen.h"
#include "liquid_measurement.h"
#ifdef HMI_SERIAL_BRIDGE
#include "hmi_serial_bridge.h"
#endif

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static LiquidMeasurement liquid;
static volatile uint8_t liquid_ready;

static void RecordLiquidEdge(uint16_t pin) {
  if (!liquid_ready) return;
  if (pin == LIQUID_UPPER_Pin)
    LiquidMeasurement_RecordEdge(&liquid, LIQUID_UPPER,
        HAL_GPIO_ReadPin(LIQUID_UPPER_GPIO_Port, pin) == GPIO_PIN_RESET, HAL_GetTick());
  else if (pin == LIQUID_LOWER_Pin)
    LiquidMeasurement_RecordEdge(&liquid, LIQUID_LOWER,
        HAL_GPIO_ReadPin(LIQUID_LOWER_GPIO_Port, pin) == GPIO_PIN_RESET, HAL_GetTick());
}

/* 上下沿同时挂起时读取真实电平，避免回调顺序留下错误的最终液位。 */
void HAL_GPIO_EXTI_Rising_Callback(uint16_t pin) { RecordLiquidEdge(pin); }
void HAL_GPIO_EXTI_Falling_Callback(uint16_t pin) { RecordLiquidEdge(pin); }

#if !defined(HMI_SERIAL_BRIDGE) && !defined(HMI_CONNECTION_TEST)
static void LiquidControl_Init(void) {
  const uint32_t interrupt_state = __get_PRIMASK();
  __disable_irq();
  LiquidMeasurement_Init(&liquid, HAL_GetTick(),
      HAL_GPIO_ReadPin(LIQUID_UPPER_GPIO_Port, LIQUID_UPPER_Pin) == GPIO_PIN_RESET,
      HAL_GPIO_ReadPin(LIQUID_LOWER_GPIO_Port, LIQUID_LOWER_Pin) == GPIO_PIN_RESET,
      HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin) == GPIO_PIN_RESET);
  liquid_ready = 1U;
  if (!interrupt_state) __enable_irq();
  HMI_Screen_SetMeasurement(liquid.stage, liquid.error, liquid.result_valid, liquid.result_ms);
  HMI_Screen_SetLiquidStatus(liquid.levels[0].stable, liquid.levels[1].stable,
                            liquid.upper_triggered, liquid.lower_triggered);
  ESP_Network_SetMeasurement(&liquid);
  char message[64];
  const int length = snprintf(message, sizeof(message), "Liquid: ready; upper=%u lower=%u\r\n",
                               liquid.levels[0].stable, liquid.levels[1].stable);
  if (length > 0) HAL_UART_Transmit(&huart2, (const uint8_t *)message, (uint16_t)length, 20U);
}

static void LiquidControl_Poll(void) {
  const LiquidStage previous_stage = liquid.stage;
  const LiquidError previous_error = liquid.error;
  const uint8_t previous_upper = liquid.levels[0].stable;
  const uint8_t previous_lower = liquid.levels[1].stable;
  const uint32_t interrupt_state = __get_PRIMASK();
  /* 时间快照与队列消费保持一致，防止新边沿时间晚于本轮 now。 */
  __disable_irq();
  const uint32_t now = HAL_GetTick();
  /* 先响应中止，避免按键恰好在完成时被解释为启动下一轮。 */
  LiquidMeasurement_Button(&liquid,
      HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin) == GPIO_PIN_RESET, now);
  LiquidMeasurement_Poll(&liquid, now);
  /* 先落实泵阀状态，再进行串口、屏幕及网络处理。 */
  HAL_GPIO_WritePin(GPIOA, PUMP_PWM2_Pin | VALVE_CONTROL_Pin,
                    liquid.pump ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin,
                    liquid.pump ? GPIO_PIN_SET : GPIO_PIN_RESET);
  if (!interrupt_state) __enable_irq();
  ESP_Network_SetMeasurementActive(LiquidMeasurement_IsActive(&liquid));
  static uint32_t last_timer_display;
  const uint8_t changed = previous_stage != liquid.stage || previous_error != liquid.error ||
      previous_upper != liquid.levels[0].stable || previous_lower != liquid.levels[1].stable;
  if (changed) {
    ESP_Network_SetMeasurement(&liquid);
    HMI_Screen_SetLiquidStatus(liquid.levels[0].stable, liquid.levels[1].stable,
                              liquid.upper_triggered, liquid.lower_triggered);
  }
  if (changed || (liquid.stage == LIQUID_TIMING && now - last_timer_display >= 500U)) {
    ESP_Network_SetPump(liquid.pump);
    HMI_Screen_SetPump(liquid.pump);
    HMI_Screen_SetMeasurement(liquid.stage, liquid.error, liquid.result_valid,
        liquid.stage == LIQUID_TIMING ? now - liquid.start_time : liquid.result_ms);
    last_timer_display = now;
  }
  if (previous_stage != liquid.stage || previous_error != liquid.error ||
      previous_upper != liquid.levels[0].stable || previous_lower != liquid.levels[1].stable) {
    static const char *const stages[] = {
      "idle", "suction", "extra_suction", "draining", "timing", "complete", "aborted", "fault"
    };
    static const char *const errors[] = {
      "none", "upper_wet", "invalid_order", "edge_overflow", "suction_timeout",
      "upper_timeout", "lower_timeout", "unstable_input"
    };
    char message[160];
    const int length = snprintf(message, sizeof(message),
        "Liquid: %s; upper=%u lower=%u; error=%s; result=%s%lu.%03lu s\r\n",
        stages[liquid.stage], liquid.levels[0].stable, liquid.levels[1].stable,
        errors[liquid.error], liquid.result_valid ? "" : "invalid:",
        (unsigned long)(liquid.result_ms / 1000U), (unsigned long)(liquid.result_ms % 1000U));
    if (length > 0) HAL_UART_Transmit(&huart2, (const uint8_t *)message,
                                     (uint16_t)length, 20U);
  }
}
#endif

/* 使用 TIM3 生成稳定的 1 MHz 微秒计时，不占用 GPIO 引脚。 */
static void MicrosecondTimer_Init(void) {
  __HAL_RCC_TIM3_CLK_ENABLE();
  TIM3->PSC = (HAL_RCC_GetPCLK1Freq() / 1000000U) - 1U;
  TIM3->ARR = 0xFFFFU;
  TIM3->EGR = TIM_EGR_UG;
  TIM3->CR1 = TIM_CR1_CEN;
}

static void DelayUs(uint32_t microseconds) {
  const uint16_t start = (uint16_t)TIM3->CNT;

  while ((uint16_t)((uint16_t)TIM3->CNT - start) < microseconds) {
  }
}

static uint8_t DS18B20_Reset(void) {
  const uint32_t interrupt_state = __get_PRIMASK();
  uint8_t detected;

  __disable_irq();
  TEMP_DATA_GPIO_Port->BSRR = (uint32_t)TEMP_DATA_Pin << 16U;
  DelayUs(500);
  TEMP_DATA_GPIO_Port->BSRR = TEMP_DATA_Pin;
  DelayUs(65);
  detected = ((TEMP_DATA_GPIO_Port->IDR & TEMP_DATA_Pin) == 0U);
  if (interrupt_state == 0U) {
    __enable_irq();
  }
  /* 恢复等待允许串口中断，复位采样时序仍保持不变。 */
  DelayUs(430);

  return detected;
}

static void DS18B20_WriteBit(uint8_t bit) {
  const uint32_t interrupt_state = __get_PRIMASK();

  __disable_irq();
  TEMP_DATA_GPIO_Port->BSRR = (uint32_t)TEMP_DATA_Pin << 16U;
  if (bit != 0U) {
    DelayUs(6);
    TEMP_DATA_GPIO_Port->BSRR = TEMP_DATA_Pin;
    DelayUs(64);
  } else {
    DelayUs(60);
    TEMP_DATA_GPIO_Port->BSRR = TEMP_DATA_Pin;
    DelayUs(10);
  }
  if (interrupt_state == 0U) {
    __enable_irq();
  }
}

static uint8_t DS18B20_ReadBit(void) {
  const uint32_t interrupt_state = __get_PRIMASK();
  uint8_t bit;

  __disable_irq();
  TEMP_DATA_GPIO_Port->BSRR = (uint32_t)TEMP_DATA_Pin << 16U;
  DelayUs(3);
  TEMP_DATA_GPIO_Port->BSRR = TEMP_DATA_Pin;
  DelayUs(10);
  bit = ((TEMP_DATA_GPIO_Port->IDR & TEMP_DATA_Pin) != 0U);
  DelayUs(53);
  if (interrupt_state == 0U) {
    __enable_irq();
  }

  return bit;
}

static void DS18B20_WriteByte(uint8_t data) {
  for (uint8_t bit = 0U; bit < 8U; ++bit) {
    DS18B20_WriteBit(data & 0x01U);
    data >>= 1U;
  }
}

static uint8_t DS18B20_ReadByte(void) {
  uint8_t data = 0U;

  for (uint8_t bit = 0U; bit < 8U; ++bit) {
    data |= (uint8_t)(DS18B20_ReadBit() << bit);
  }

  return data;
}

static uint8_t DS18B20_CalculateCrc(const uint8_t *data, uint8_t length) {
  uint8_t crc = 0U;

  for (uint8_t index = 0U; index < length; ++index) {
    uint8_t value = data[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint8_t mix = (crc ^ value) & 0x01U;
      crc >>= 1U;
      if (mix != 0U) {
        crc ^= 0x8CU;
      }
      value >>= 1U;
    }
  }

  return crc;
}

static uint8_t DS18B20_StartConversion(void) {
  if (DS18B20_Reset() == 0U) return 1U;
  DS18B20_WriteByte(0xCCU);
  DS18B20_WriteByte(0x44U);
  return 0U;
}

static uint8_t DS18B20_ReadTemperature(int16_t *raw_temperature) {
  uint8_t scratchpad[9];
  uint8_t all_zero = 1U;

  if (DS18B20_Reset() == 0U) {
    return 1U;
  }

  DS18B20_WriteByte(0xCCU);
  DS18B20_WriteByte(0xBEU);
  for (uint8_t index = 0U; index < sizeof(scratchpad); ++index) {
    scratchpad[index] = DS18B20_ReadByte();
    if (scratchpad[index] != 0U) {
      all_zero = 0U;
    }
  }

  if ((all_zero != 0U) ||
      (DS18B20_CalculateCrc(scratchpad, 8U) != scratchpad[8])) {
    return 2U;
  }

  *raw_temperature =
      (int16_t)(((uint16_t)scratchpad[1] << 8U) | scratchpad[0]);
  return 0U;
}

/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void) {

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick.
   */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */
#ifdef HMI_SERIAL_BRIDGE
  /* 下载期间泵阀关闭，不进入采温、联网、按键控制或日志输出。 */
  HMI_Bridge_Init();
  while (1) HMI_Bridge_Poll();
#endif
#ifdef HMI_CONNECTION_TEST
  /* 专用接线测试不进入采温、联网和 B1 控制，GPIO 初始化已关闭泵阀。 */
  HMI_Screen_Init();
  while (1) {
    HMI_Screen_Poll();
    HAL_Delay(1);
  }
#endif
  MicrosecondTimer_Init();
  static const uint8_t startup_message[] =
      "STM32G071RB ready.\r\nDS18B20 temperature test.\r\n";
  static const uint8_t sensor_missing_message[] =
      "DS18B20 not found. Check wiring or add 4.7k pull-up.\r\n";
  static const uint8_t sensor_crc_message[] =
      "DS18B20 data error. Add 4.7k pull-up.\r\n";
  uint32_t last_temperature_time = 0U;
  uint8_t conversion_pending = 0U;
  uint8_t conversion_status = 0U;
  uint32_t conversion_time = 0U;

  HAL_UART_Transmit(&huart2, startup_message, sizeof(startup_message) - 1U,
                    HAL_MAX_DELAY);
  HAL_Delay(100);
  ESP_Network_Init();
  HMI_Screen_Init();
#if !defined(HMI_SERIAL_BRIDGE) && !defined(HMI_CONNECTION_TEST)
  LiquidControl_Init();
#endif
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
#if !defined(HMI_SERIAL_BRIDGE) && !defined(HMI_CONNECTION_TEST)
    LiquidControl_Poll();
#endif
    ESP_Network_Poll();
#if !defined(HMI_SERIAL_BRIDGE) && !defined(HMI_CONNECTION_TEST)
    LiquidControl_Poll();
#endif
    HMI_Screen_Poll();
    if (!conversion_pending && HAL_GetTick() - last_temperature_time >= 1000U) {
      conversion_status = DS18B20_StartConversion();
      conversion_time = HAL_GetTick();
      last_temperature_time = conversion_time;
      conversion_pending = 1U;
    }
    if (conversion_pending && (conversion_status != 0U ||
        HAL_GetTick() - conversion_time >= 750U)) {
      int16_t raw_temperature;
      const uint8_t sensor_status =
          conversion_status != 0U ? conversion_status : DS18B20_ReadTemperature(&raw_temperature);
      conversion_pending = 0U;

      ESP_Network_SetTemperature(sensor_status,
          sensor_status == 0U ? ((int32_t)raw_temperature * 100) / 16 : 0);
      HMI_Screen_SetTemperature(sensor_status,
          sensor_status == 0U ? ((int32_t)raw_temperature * 100) / 16 : 0);
      if (sensor_status == 0U) {
        char temperature_message[40];
        const int32_t temperature_centi =
            ((int32_t)raw_temperature * 100) / 16;
        const int32_t absolute_temperature =
            (temperature_centi < 0) ? -temperature_centi : temperature_centi;
        const int length = snprintf(
            temperature_message, sizeof(temperature_message),
            "Temperature: %s%ld.%02ld C\r\n",
            (temperature_centi < 0) ? "-" : "",
            (long)(absolute_temperature / 100),
            (long)(absolute_temperature % 100));

        if (length > 0) {
          HAL_UART_Transmit(&huart2, (const uint8_t *)temperature_message,
                            (uint16_t)length, HAL_MAX_DELAY);
        }
      } else if (sensor_status == 1U) {
        HAL_UART_Transmit(&huart2, sensor_missing_message,
                          sizeof(sensor_missing_message) - 1U, HAL_MAX_DELAY);
      } else {
        HAL_UART_Transmit(&huart2, sensor_crc_message,
                          sizeof(sensor_crc_message) - 1U, HAL_MAX_DELAY);
      }
    }
#if !defined(HMI_SERIAL_BRIDGE) && !defined(HMI_CONNECTION_TEST)
    ESP_Network_PollUsbTelemetry();
#endif
  }
  /* USER CODE END 3 */
}

/**
 * @brief System Clock Configuration
 * @retval None
 */
void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
   */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
   * in the RCC_OscInitTypeDef structure.
   */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
   */
  RCC_ClkInitStruct.ClockType =
      RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK) {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void) {
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1) {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
 * @brief  Reports the name of the source file and the source line number
 *         where the assert_param error has occurred.
 * @param  file: pointer to the source file name
 * @param  line: assert_param error line source number
 * @retval None
 */
void assert_failed(uint8_t *file, uint32_t line) {
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line
     number, ex: printf("Wrong parameters value: file %s on line %d\r\n", file,
     line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
