#ifndef LIQUID_MEASUREMENT_H
#define LIQUID_MEASUREMENT_H

#include <stdint.h>

#define LIQUID_DEBOUNCE_MS 5U
#define LIQUID_EXTRA_SUCTION_MS 300U
#define LIQUID_SUCTION_TIMEOUT_MS 30000U
#define LIQUID_DRAIN_TIMEOUT_MS 120000U
#define LIQUID_EDGE_CAPACITY 32U

typedef enum {
  LIQUID_IDLE, LIQUID_SUCTION, LIQUID_EXTRA_SUCTION, LIQUID_WAIT_UPPER,
  LIQUID_TIMING, LIQUID_COMPLETE, LIQUID_ABORTED, LIQUID_FAULT
} LiquidStage;
typedef enum {
  LIQUID_ERROR_NONE, LIQUID_ERROR_UPPER_WET, LIQUID_ERROR_ORDER,
  LIQUID_ERROR_OVERFLOW, LIQUID_ERROR_SUCTION_TIMEOUT,
  LIQUID_ERROR_UPPER_TIMEOUT, LIQUID_ERROR_LOWER_TIMEOUT,
  LIQUID_ERROR_UNSTABLE
} LiquidError;
typedef enum { LIQUID_UPPER, LIQUID_LOWER } LiquidInput;

typedef struct { uint32_t time; uint8_t input, wet; } LiquidEdge;
typedef struct {
  uint32_t time;
  uint8_t raw, stable;
} LiquidLevel;
typedef struct {
  LiquidStage stage;
  LiquidError error;
  uint32_t phase_time, start_time, result_ms;
  uint8_t pump, result_valid, upper_triggered, lower_triggered;
  LiquidLevel levels[2];
  LiquidEdge edges[LIQUID_EDGE_CAPACITY];
  volatile uint8_t head, tail, overflow;
  uint32_t button_time;
  uint8_t button_raw, button_stable, button_armed;
} LiquidMeasurement;

void LiquidMeasurement_Init(LiquidMeasurement *measurement, uint32_t now,
                            uint8_t upper_wet, uint8_t lower_wet, uint8_t pressed);
/* 单一 EXTI 生产者调用；主循环消费，不在中断内控制泵阀或打印。 */
void LiquidMeasurement_RecordEdge(LiquidMeasurement *measurement,
                                  LiquidInput input, uint8_t wet, uint32_t now);
void LiquidMeasurement_Poll(LiquidMeasurement *measurement, uint32_t now);
uint8_t LiquidMeasurement_Start(LiquidMeasurement *measurement, uint32_t now);
void LiquidMeasurement_Abort(LiquidMeasurement *measurement);
void LiquidMeasurement_Button(LiquidMeasurement *measurement,
                              uint8_t pressed, uint32_t now);
uint8_t LiquidMeasurement_IsActive(const LiquidMeasurement *measurement);

#endif
