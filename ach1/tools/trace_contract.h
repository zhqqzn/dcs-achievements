/*
 * Контракт хостовой стороны, общий для генераторов трасс, 
 * анализаторов и тестовых обвязок.
 * Нормативная семантика и правила совместимости -- в файле
 * USART_TRACE_CONTRACT.md.
 */
#ifndef AVR_PINGPONG_TRACE_CONTRACT_H
#define AVR_PINGPONG_TRACE_CONTRACT_H

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/protocol_contract.inc"

/* Каноническая схема CSV: одно событие на строку, в порядке наблюдения */
#define TRACE_CSV_HEADER "kind,cycle,value"
#define TRACE_KIND_TIMER1_START 'S'
#define TRACE_KIND_UDR_WRITE 'U'
#define TRACE_KIND_EPOCH_MARKER 'E'
#define TRACE_KIND_PORTC_WRITE 'P'

/* Метаданные расписания, передаваемые инструментацией PORTC, используемой только в стресс-тестах */
#define TRACE_SCHEDULE_MARKER UART_TRACE_SCHEDULE_MARKER
#define TRACE_SCHEDULE_FIELD_COUNT 8U
#define TRACE_MAX_SCHEDULE_LINE_BYTES UART_OUTPUT_LINE_MAX_BYTES

typedef struct {
    char kind;
    uint64_t cycle;
    unsigned value;
} trace_record_t;

/*
 * Разбор строки трассы. При записи значение такта заключается в кавычки
 * согласно каноническому формату CSV; поддержка не заключенного в кавычки
 * десятичного значения такта делает парсеры устойчивыми к эквивалентным
 * вариантам CSV.
 * Эта вспомогательная функция намеренно разбирает только сами записи,
 * не затрагивая семантику, специфичную для режима трассировки;
 * каждый анализатор сам определяет, какие типы записей имеют для него смысл.
 */
static inline int
trace_contract_parse_record(char *line, trace_record_t *record)
{
    char *cursor;
    char *end;
    unsigned long long cycle;
    unsigned long long value;
    int quoted_cycle = 0;
    char kind;
    size_t length;

    if (line == NULL || record == NULL)
        return -1;

    length = strlen(line);
    while (length > 0U && (line[length - 1U] == '\n' || line[length - 1U] == '\r'))
        line[--length] = '\0';

    cursor = line;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (*cursor == '\0') return -1;
    kind = *cursor++;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (*cursor++ != ',') return -1;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;

    if (*cursor == '"') {
        quoted_cycle = 1;
        ++cursor;
    }
    if (!isdigit((unsigned char)*cursor)) return -1;
    errno = 0;
    cycle = strtoull(cursor, &end, 10);
    if (errno == ERANGE || end == cursor || cycle > UINT64_MAX) return -1;
    cursor = end;
    if (quoted_cycle) {
        if (*cursor++ != '"') return -1;
    }
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (*cursor++ != ',') return -1;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;

    if (!isdigit((unsigned char)*cursor)) return -1;
    errno = 0;
    value = strtoull(cursor, &end, 10);
    if (errno == ERANGE || end == cursor || value > UINT_MAX) return -1;
    cursor = end;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (*cursor != '\0') return -1;

    if (kind != TRACE_KIND_TIMER1_START && kind != TRACE_KIND_UDR_WRITE &&
        kind != TRACE_KIND_EPOCH_MARKER && kind != TRACE_KIND_PORTC_WRITE)
        return -1;
    if ((kind == TRACE_KIND_TIMER1_START || kind == TRACE_KIND_UDR_WRITE ||
         kind == TRACE_KIND_PORTC_WRITE) && value > UINT8_MAX)
        return -1;
    if (kind == TRACE_KIND_EPOCH_MARKER && value > UINT16_MAX)
        return -1;

    record->kind = kind;
    record->cycle = (uint64_t)cycle;
    record->value = (unsigned)value;
    return 0;
}

#endif
