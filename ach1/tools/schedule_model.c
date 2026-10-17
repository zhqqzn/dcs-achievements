/*
 * Расчётная модель временного расписания для ATmega8
 * Модель используется тестами и для независимой проверки критериев допуска
 */

#include "schedule_model.h"

/* Наибольший общий делитель задаёт общую временную сетку, на которой должны помещаться оба потока */
uint32_t schedule_gcd(uint32_t a, uint32_t b)
{
    /* Алгоритм Евклида находит наибольший общий делитель за логарифмическое время */
    while (b != 0U) {
        uint32_t remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

/* Переводим число байтов строки вместе с CR/LF во время передачи в тактах CPU */
uint32_t schedule_line_cycles(uint32_t length_including_crlf)
{
    /* Каждый байт 8N1 занимает десять битовых интервалов. */
    if (length_including_crlf < 2U || length_including_crlf > 24U)
        return 0U;
    return length_including_crlf * SCHED_UART_FRAME_CYCLES;
}

/* Проверяем аппаратные ограничения и наличие непересекающихся окон для обеих строк и ответа статуса */
schedule_validation_t schedule_validate(uint32_t t1, uint32_t t2,
                                         uint32_t len1, uint32_t len2)
{
    schedule_validation_t result;
    uint32_t tx1;
    uint32_t tx2;
    uint32_t required;
    uint32_t phase;

    result.feasible = 0;
    result.gcd_cycles = schedule_gcd(t1, t2);
    result.phase_cycles = 0U;
    result.required_gcd_cycles = 0U;
    result.reason = "uninitialized";

    /* Интервалы прошивки имеют ширину 16 бит; модель проверяет тот же диапазон */
    if (t1 < 1U || t1 > 0xffffU || t2 < 1U || t2 > 0xffffU) {
        result.reason = "interval outside uint16";
        return result;
    }
    if (t2 % SCHED_TIMER2_PRESCALER != 0U || t2 > SCHED_TIMER2_MAX_CYCLES) {
        result.reason = "Timer2 period must be a multiple of 128 cycles and <=32768";
        return result;
    }
    if (len1 < 2U || len1 > 24U || len2 < 2U || len2 > 24U) {
        result.reason = "line length outside supported range";
        return result;
    }

    tx1 = schedule_line_cycles(len1);
    tx2 = schedule_line_cycles(len2);
    /* Резервируем время обеих строк, ответа OK/ERR, настройки управляющего слота и защитного промежутка */
    required = tx1 + tx2 + SCHED_STATUS_TX_CYCLES +
               SCHED_CONTROL_SLOT_SETUP + SCHED_OUTPUT_GUARD_CYCLES;
    phase = tx1 + SCHED_STATUS_TX_CYCLES + SCHED_CONTROL_SLOT_SETUP;
    result.phase_cycles = phase;
    result.required_gcd_cycles = required;

    /* Если требуемое окно не помещается в 16-разрядный счётчик, конфигурация невозможна */
    if (required > 0xffffU) {
        result.reason = "required separation exceeds 16-bit range";
        return result;
    }
    if (t1 < required || t2 < required || result.gcd_cycles < required) {
        result.reason = "period/gcd too small for outputs and status slot";
        return result;
    }
    /* Проверяем, что фазовый сдвиг и gcd общей сетки оставляют окно для второй строки и защитного промежутка */
    if (phase >= result.gcd_cycles ||
        result.gcd_cycles - phase < tx2 + SCHED_OUTPUT_GUARD_CYCLES) {
        result.reason = "phase leaves insufficient gap";
        return result;
    }

    result.feasible = 1;
    result.reason = "schedulable under the model";
    return result;
}

/* Находим минимальную одинаковую сетку периодов для режима без командного канала */
void schedule_periodic_only_minimum(uint32_t len1, uint32_t len2,
                                    uint32_t guard, uint32_t *g_min,
                                    uint32_t *phase, uint32_t *sum_min)
{
    uint32_t tx1 = schedule_line_cycles(len1);
    uint32_t tx2 = schedule_line_cycles(len2);
    uint32_t gap = tx1 + tx2 + 2U * guard;

    /* Округлить интервал вверх до шага Timer2 с делителем 128 */
    gap = ((gap + SCHED_TIMER2_PRESCALER - 1U) / SCHED_TIMER2_PRESCALER) * SCHED_TIMER2_PRESCALER;
    /* Без ответов команд два потока разделяются временем передачи и двумя защитными промежутками */
    if (g_min != 0)
        *g_min = gap;
    if (phase != 0)
        *phase = tx1 + guard;
    if (sum_min != 0)
        *sum_min = 2U * gap;
}

/* Округляем найденный период вверх до целого числа миллисекунд и считает сумму двух периодов */
void schedule_integer_ms_minimum(uint32_t len1, uint32_t len2,
                                 uint32_t guard, uint32_t *periods_ms,
                                 uint32_t *sum_ms)
{
    uint32_t minimum = 0U;
    uint32_t one_ms = SCHED_F_CPU / 1000U;
    uint32_t periods;

    schedule_periodic_only_minimum(len1, len2, guard, &minimum, 0, 0);
    /* Округляем число миллисекунд вверх, чтобы период не оказался меньше минимума */
    periods = (minimum + one_ms - 1U) / one_ms;
    if (periods_ms != 0)
        *periods_ms = periods;
    if (sum_ms != 0)
        *sum_ms = 2U * periods;
}
