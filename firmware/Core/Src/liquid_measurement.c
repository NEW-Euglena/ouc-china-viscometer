#include "liquid_measurement.h"
#include <string.h>
#ifndef LIQUID_HOST_TEST
#include "stm32g0xx.h"
#define EDGE_BARRIER() __DMB()
#else
#define EDGE_BARRIER() ((void)0)
#endif

uint8_t LiquidMeasurement_IsActive(const LiquidMeasurement *m) {
  return m->stage >= LIQUID_SUCTION && m->stage <= LIQUID_TIMING;
}

static void Fail(LiquidMeasurement *m, LiquidError error) {
  m->stage = LIQUID_FAULT;
  m->error = error;
  m->pump = 0U;
}

void LiquidMeasurement_Init(LiquidMeasurement *m, uint32_t now,
                            uint8_t upper_wet, uint8_t lower_wet, uint8_t pressed) {
  memset(m, 0, sizeof(*m));
  m->levels[0] = (LiquidLevel){now, upper_wet != 0U, upper_wet != 0U};
  m->levels[1] = (LiquidLevel){now, lower_wet != 0U, lower_wet != 0U};
  m->button_time = now;
  m->button_raw = m->button_stable = pressed != 0U;
  m->button_armed = pressed == 0U;
}

void LiquidMeasurement_RecordEdge(LiquidMeasurement *m,
                                  LiquidInput input, uint8_t wet, uint32_t now) {
  const uint8_t next = (uint8_t)((m->head + 1U) % LIQUID_EDGE_CAPACITY);
  if (next == m->tail) { m->overflow = 1U; return; }
  m->edges[m->head] = (LiquidEdge){now, (uint8_t)input, wet != 0U};
  EDGE_BARRIER();
  m->head = next;
}

/* 超时额外等待消抖窗口，截止前出现的边沿仍按原始时间判定。 */
static void Advance(LiquidMeasurement *m, uint32_t now, uint8_t grace) {
  const uint32_t elapsed = now - m->phase_time;
  if (m->stage == LIQUID_EXTRA_SUCTION && elapsed >= LIQUID_EXTRA_SUCTION_MS) {
    m->phase_time = now;
    m->stage = LIQUID_WAIT_UPPER;
    m->pump = 0U;
  }
  if (m->stage == LIQUID_SUCTION &&
      elapsed >= LIQUID_SUCTION_TIMEOUT_MS + grace)
    Fail(m, LIQUID_ERROR_SUCTION_TIMEOUT);
  else if (m->stage == LIQUID_WAIT_UPPER &&
           now - m->phase_time >= LIQUID_DRAIN_TIMEOUT_MS + grace)
    Fail(m, LIQUID_ERROR_UPPER_TIMEOUT);
  else if (m->stage == LIQUID_TIMING &&
           elapsed >= LIQUID_DRAIN_TIMEOUT_MS + grace)
    Fail(m, LIQUID_ERROR_LOWER_TIMEOUT);
}

static void Accept(LiquidMeasurement *m, unsigned input, uint32_t time) {
  LiquidLevel *level = &m->levels[input];
  level->stable = level->raw;
  if (!LiquidMeasurement_IsActive(m)) return;
  if ((int32_t)(time - m->phase_time) < 0) {
    Fail(m, LIQUID_ERROR_ORDER); return;
  }
  /* 续吸期间捕获的下降不能冒充实际停泵后的放流。 */
  if (m->stage != LIQUID_EXTRA_SUCTION) Advance(m, time, 0U);
  if (!LiquidMeasurement_IsActive(m)) return;
  if (m->stage == LIQUID_SUCTION) {
    if (input == LIQUID_UPPER && level->stable) {
      if (!m->levels[LIQUID_LOWER].stable) { Fail(m, LIQUID_ERROR_ORDER); return; }
      m->stage = LIQUID_EXTRA_SUCTION;
      m->phase_time = time;
    } else if (!level->stable) Fail(m, LIQUID_ERROR_ORDER);
  } else if (m->stage == LIQUID_EXTRA_SUCTION) {
    if (!level->stable) Fail(m, LIQUID_ERROR_ORDER);
  } else if (m->stage == LIQUID_WAIT_UPPER) {
    if (input != LIQUID_UPPER || level->stable || !m->levels[LIQUID_LOWER].stable) {
      Fail(m, LIQUID_ERROR_ORDER); return;
    }
    m->stage = LIQUID_TIMING;
    m->upper_triggered = 1U;
    m->phase_time = m->start_time = time;
  } else if (m->stage == LIQUID_TIMING) {
    if (input != LIQUID_LOWER || level->stable || time == m->start_time) {
      Fail(m, LIQUID_ERROR_ORDER); return;
    }
    m->result_ms = time - m->start_time;
    m->result_valid = 1U;
    m->lower_triggered = 1U;
    m->stage = LIQUID_COMPLETE;
  }
}

/* 先确认两个输入中较早的成熟边沿，避免按引脚遍历顺序改变计时。 */
static void ConfirmUntil(LiquidMeasurement *m, uint32_t now) {
  for (unsigned count = 0U; count < 2U; ++count) {
    int selected = -1;
    for (unsigned i = 0U; i < 2U; ++i) {
      const LiquidLevel *level = &m->levels[i];
      if (level->raw != level->stable && now - level->time >= LIQUID_DEBOUNCE_MS &&
          (selected < 0 || now - level->time > now - m->levels[selected].time))
        selected = (int)i;
    }
    if (selected < 0) break;
    Accept(m, (unsigned)selected, m->levels[selected].time);
  }
}

void LiquidMeasurement_Poll(LiquidMeasurement *m, uint32_t now) {
  if (m->overflow) { Fail(m, LIQUID_ERROR_OVERFLOW); return; }
  while (m->tail != m->head) {
    EDGE_BARRIER();
    const LiquidEdge edge = m->edges[m->tail];
    m->tail = (uint8_t)((m->tail + 1U) % LIQUID_EDGE_CAPACITY);
    ConfirmUntil(m, edge.time);
    LiquidLevel *level = &m->levels[edge.input];
    if (level->raw != edge.wet) {
      level->raw = edge.wet;
      level->time = edge.time;
    }
  }
  ConfirmUntil(m, now);
  Advance(m, now, LIQUID_DEBOUNCE_MS);
}

uint8_t LiquidMeasurement_Start(LiquidMeasurement *m, uint32_t now) {
  if (LiquidMeasurement_IsActive(m)) return 0U;
  if (m->overflow) { Fail(m, LIQUID_ERROR_OVERFLOW); return 0U; }
  if (m->head != m->tail || m->levels[0].raw != m->levels[0].stable ||
      m->levels[1].raw != m->levels[1].stable) {
    Fail(m, LIQUID_ERROR_UNSTABLE); return 0U;
  }
  if (m->levels[0].stable) { Fail(m, LIQUID_ERROR_UPPER_WET); return 0U; }
  m->result_valid = 0U;
  m->upper_triggered = m->lower_triggered = 0U;
  m->result_ms = 0U;
  m->error = LIQUID_ERROR_NONE;
  m->stage = LIQUID_SUCTION;
  m->phase_time = now;
  m->pump = 1U;
  return 1U;
}

void LiquidMeasurement_Abort(LiquidMeasurement *m) {
  if (!LiquidMeasurement_IsActive(m)) return;
  m->stage = LIQUID_ABORTED;
  m->pump = 0U;
}

void LiquidMeasurement_Button(LiquidMeasurement *m, uint8_t pressed, uint32_t now) {
  pressed = pressed != 0U;
  if (pressed != m->button_raw) {
    m->button_raw = pressed;
    m->button_time = now;
  }
  if (now - m->button_time < 40U || pressed == m->button_stable) return;
  m->button_stable = pressed;
  if (!pressed) m->button_armed = 1U;
  else if (m->button_armed) {
    m->button_armed = 0U;
    if (LiquidMeasurement_IsActive(m)) LiquidMeasurement_Abort(m);
    else LiquidMeasurement_Start(m, now);
  }
}
