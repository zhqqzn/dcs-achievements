#ifndef AVR_PINGPONG_SCHEDULE_MODEL_H
#define AVR_PINGPONG_SCHEDULE_MODEL_H

#include <stdint.h>

/* Константы модели должны совпадать с параметрами прошивки и настройками USART */
#define SCHED_F_CPU 16000000UL
#define SCHED_UBRR 16UL
#define SCHED_CPU_CYCLES_PER_BIT (8UL * (SCHED_UBRR + 1UL))
#define SCHED_UART_FRAME_CYCLES (10UL * SCHED_CPU_CYCLES_PER_BIT)
/* Наибольший ответ OK/ERR вместе с CR/LF занимает пять передаваемых байтов */
#define SCHED_STATUS_MAX_LEN 5UL
#define SCHED_STATUS_TX_CYCLES (SCHED_STATUS_MAX_LEN * SCHED_UART_FRAME_CYCLES)
/* Запас на прохождение управляющего слота от планирования до начала ответа */
#define SCHED_CONTROL_SLOT_SETUP 512UL
/* Защитный промежуток не позволяет считать соседние передачи соприкасающимися */
#define SCHED_OUTPUT_GUARD_CYCLES 32UL
/* Timer2 использует делитель 128; его периоды допустимы только с этим шагом */
#define SCHED_TIMER2_PRESCALER 128UL
#define SCHED_TIMER2_MAX_CYCLES 32768UL

/* Результат проверки содержит ответ и величины, объясняющие отказ */
typedef struct {
    int feasible;
    uint32_t gcd_cycles;
    uint32_t phase_cycles;
    uint32_t required_gcd_cycles;
    const char *reason;
} schedule_validation_t;

/* Ниже объявлены функции модели, общей для тестов и проверки параметров */
uint32_t schedule_gcd(uint32_t a, uint32_t b);
uint32_t schedule_line_cycles(uint32_t length_including_crlf);
schedule_validation_t schedule_validate(uint32_t t1, uint32_t t2,
                                         uint32_t len1, uint32_t len2);
void schedule_periodic_only_minimum(uint32_t len1, uint32_t len2,
                                    uint32_t guard, uint32_t *g_min,
                                    uint32_t *phase, uint32_t *sum_min);
void schedule_integer_ms_minimum(uint32_t len1, uint32_t len2,
                                 uint32_t guard, uint32_t *periods_ms,
                                 uint32_t *sum_ms);

#endif
