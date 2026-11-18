/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   This file provides code for the configuration
  *          of all used GPIO pins.
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
#include "gpio.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/** Configure pins as
        * Analog
        * Input
        * Output
        * EVENT_OUT
        * EXTI
*/
void MX_GPIO_Init(void)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* 三路执行器先保持断电，诊断和下载模式不进入业务待机状态。 */
  HAL_GPIO_WritePin(GPIOA, PUMP_PWM2_Pin | VALVE_CONTROL_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(C_VENT_VALVE_GPIO_Port, C_VENT_VALVE_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /* 配置真空泵和电磁阀 GPIO 输出。 */
  GPIO_InitStruct.Pin = PUMP_PWM2_Pin | VALVE_CONTROL_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* D9/PC7 控制 C 管泄压阀，低电平通气，高电平封住排气口。 */
  GPIO_InitStruct.Pin = C_VENT_VALVE_Pin;
  HAL_GPIO_Init(C_VENT_VALVE_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(TEMP_DATA_GPIO_Port, TEMP_DATA_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin : TEMP_DATA_Pin */
  GPIO_InitStruct.Pin = TEMP_DATA_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(TEMP_DATA_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : LED_GREEN_Pin */
  GPIO_InitStruct.Pin = LED_GREEN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(LED_GREEN_GPIO_Port, &GPIO_InitStruct);

  /* NPN 有液低电平；外部各接 4.7kΩ 到 3V3，内部上拉作为辅助。 */
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Pin = LIQUID_UPPER_Pin;
  HAL_GPIO_Init(LIQUID_UPPER_GPIO_Port, &GPIO_InitStruct);
  GPIO_InitStruct.Pin = LIQUID_LOWER_Pin;
  HAL_GPIO_Init(LIQUID_LOWER_GPIO_Port, &GPIO_InitStruct);
  HAL_NVIC_SetPriority(EXTI2_3_IRQn, 1U, 0U);
  HAL_NVIC_EnableIRQ(EXTI2_3_IRQn);
  HAL_NVIC_SetPriority(EXTI4_15_IRQn, 1U, 0U);
  HAL_NVIC_EnableIRQ(EXTI4_15_IRQn);

}

/* USER CODE BEGIN 2 */

void PumpValve_SetRunning(uint8_t running)
{
  if (running != 0U) {
    /* 抽气前先关闭 C 管通气路径。 */
    HAL_GPIO_WritePin(C_VENT_VALVE_GPIO_Port, C_VENT_VALVE_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, PUMP_PWM2_Pin | VALVE_CONTROL_Pin, GPIO_PIN_SET);
  } else {
    /* 先停泵并切回 B 管，再打开 C 管通气路径。 */
    HAL_GPIO_WritePin(GPIOA, PUMP_PWM2_Pin | VALVE_CONTROL_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(C_VENT_VALVE_GPIO_Port, C_VENT_VALVE_Pin, GPIO_PIN_RESET);
  }
}

/* USER CODE END 2 */
