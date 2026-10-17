/*
 * Регрессионные тесты расчётной модели расписания
 * Проверяются формулы USART, минимальные периоды и отклонение недопустимых пар
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "schedule_model.h"

static unsigned tests_run;
static unsigned tests_failed;

#define CHECK(condition, label) do { \
    ++tests_run; \
    if (condition) { printf("PASS: %s\n", label); } \
    else { printf("FAIL: %s (line %d)\n", label, __LINE__); ++tests_failed; } \
} while (0)

/* Каждая проверка фиксирует формулу или граничный случай. Первый FAIL влияет на код возврата */
int main(void)
{
    schedule_validation_t result;
    uint32_t g, phase, sum, periods_ms, sum_ms;

    /* Проверяем вычисляемую скорость USART и перевод кадра 8N1 в такты процессора. */
    CHECK(fabs(((double)SCHED_F_CPU / (double)(8UL * (SCHED_UBRR + 1UL))) - 117647.0588235) < 0.001,
          "actual USART baud rate calculation");
    CHECK(SCHED_CPU_CYCLES_PER_BIT == 136UL, "CPU cycles per bit");
    CHECK(SCHED_UART_FRAME_CYCLES == 1360UL, "8N1 frame cycles");
    CHECK(schedule_line_cycles(6U) == 8160UL && SCHED_STATUS_TX_CYCLES == 6800UL,
          "default line and status transmission lengths");

    /* Отдельная формула относится к режиму без командных ответов и не подменяет полную проверку расписания */
    schedule_periodic_only_minimum(6U, 6U, SCHED_OUTPUT_GUARD_CYCLES,
                                   &g, &phase, &sum);
    CHECK(g == 16384UL && phase == 8192UL && sum == 32768UL,
          "periodic-only theoretical minimum with guards");
    schedule_integer_ms_minimum(6U, 6U, SCHED_OUTPUT_GUARD_CYCLES,
                                &periods_ms, &sum_ms);
    CHECK(periods_ms == 2UL && sum_ms == 4UL,
          "minimum equal integer-millisecond intervals");
    CHECK(!schedule_validate(16000U, 16000U, 6U, 6U).feasible,
          "one-millisecond intervals rejected");

    /* Проверки ниже закрепляют аппаратные ограничения Timer2 и минимальную допустимую пару периодов */
    result = schedule_validate(23680U, 23680U, 6U, 6U);
    CHECK(result.feasible && result.gcd_cycles == 23680U &&
          result.phase_cycles == 15472U && result.required_gcd_cycles == 23664U,
          "minimum hardware-representable command-channel schedule");
    CHECK(!schedule_validate(23663U, 23663U, 6U, 6U).feasible,
          "period below command-channel minimum rejected");
    CHECK(!schedule_validate(23664U, 23664U, 6U, 6U).feasible,
          "Timer2 interval not aligned to prescaler rejected");
    CHECK(!schedule_validate(32896U, 32896U, 6U, 6U).feasible,
          "Timer2 interval beyond 32768-cycle hardware range rejected");
    CHECK(schedule_validate(32768U, 32768U, 6U, 6U).feasible,
          "maximum representable Timer2 interval accepted");
    CHECK(schedule_validate(24064U, 24064U, 6U, 6U).feasible,
          "aligned safe command-channel period accepted");
    CHECK(schedule_gcd(24064U, 24192U) == 128U &&
          !schedule_validate(24064U, 24192U, 6U, 6U).feasible,
          "gcd admission rule rejects incompatible aligned period pair");
    CHECK(!schedule_validate(23680U, 24064U, 6U, 6U).feasible &&
          schedule_validate(23680U, 23680U, 6U, 6U).feasible,
          "paired period update can be feasible when single update is not");
    result = schedule_validate(65535U, 32768U, 24U, 24U);
    CHECK(!result.feasible && result.required_gcd_cycles > 0xffffU,
          "two maximum strings exceed 16-bit scheduling range");
    CHECK(!schedule_validate(49000U, 32768U, 24U, 6U).feasible,
          "long line rejected when no Timer2 period can fit its schedule");

    printf("\nTests: %u; failures: %u\n", tests_run, tests_failed);
    return tests_failed == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
