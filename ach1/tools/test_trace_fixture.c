/*
 * Генератор синтетических CSV-трасс для регрессионных тестов анализаторов
 * Режимы bad намеренно сдвигают начало первой строки на шесть тактов:
 * корректный анализатор должен принять обычный файл и отвергнуть испорченный
 */

#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trace_contract.h"

typedef struct { char kind; uint64_t cycle; unsigned value; } row_t;
static row_t *rows;
static size_t row_count, row_capacity;

/* Накапливаем синтетическую запись до сортировки; динамический массив нужен для трасс разной длины */
static int add_row(char kind, uint64_t cycle, unsigned value)
{
    if (row_count == row_capacity) {
        size_t next = row_capacity == 0U ? 1024U : row_capacity * 2U;
        row_t *expanded = realloc(rows, next * sizeof(*rows));
        if (expanded == NULL) return -1;
        rows = expanded; row_capacity = next;
    }
    rows[row_count++] = (row_t){kind, cycle, value};
    return 0;
}

/* Упорядочиваем события по такту и задаёт стабильный приоритет метаданных в совпадающий такт */
static int compare_rows(const void *lhs, const void *rhs)
{
    const row_t *a = lhs, *b = rhs;
    if (a->cycle < b->cycle) return -1;
    if (a->cycle > b->cycle) return 1;
    /* Метаданные эпохи располагаются раньше UART-событий с тем же циклом */
    int pa = a->kind == TRACE_KIND_TIMER1_START ? 0 : a->kind == TRACE_KIND_EPOCH_MARKER ? 1 : a->kind == TRACE_KIND_PORTC_WRITE ? 2 : 3;
    int pb = b->kind == TRACE_KIND_TIMER1_START ? 0 : b->kind == TRACE_KIND_EPOCH_MARKER ? 1 : b->kind == TRACE_KIND_PORTC_WRITE ? 2 : 3;
    return pa - pb;
}

/* Моделируем разнесённые по UART байты: каждый следующий байт начинается через 8N1-интервал */
static int add_uart_line(uint64_t start, const char *line, size_t length, bool shift_first)
{
    for (size_t i = 0U; i < length; ++i) {
        uint64_t cycle = start + (uint64_t)i * 1104U;
        /* Негативная фикстура сдвигает только первый байт: анализатор обязан заметить нарушение допуска */
        if (shift_first) cycle += 6U;
        if (add_row(TRACE_KIND_UDR_WRITE, cycle, (unsigned char)line[i]) != 0) return -1;
    }
    return 0;
}

/* Формируем положительную трассу периодического задания и её вариант с ошибкой +6 тактов */
static int generate_task2(bool bad_timing)
{
    const uint64_t epoch = 1000U;
    const uint32_t t1 = 16384U, t2 = 16384U, phi = 8192U;
    if (add_row(TRACE_KIND_TIMER1_START, epoch, 1U) != 0) return -1;
    for (unsigned i = 0U; i < 12U; ++i) {
        uint64_t ping = epoch + t1 + (uint64_t)i * t1;
        uint64_t pong = epoch + phi + t2 + (uint64_t)i * t2;
        if (add_uart_line(ping, UART_DEFAULT_STREAM1_FRAME, UART_DEFAULT_STREAM1_FRAME_BYTES, bad_timing && i == 0U) != 0 ||
            add_uart_line(pong, UART_DEFAULT_STREAM2_FRAME, UART_DEFAULT_STREAM2_FRAME_BYTES, false) != 0) return -1;
    }
    return 0;
}

/* Кодируем снимок счётчика, периоды, длины и активные строки тем же форматом, что диагностическая прошивка */
static int add_stress_epoch_marker(uint64_t sample)
{
    const uint64_t start = 1000U;
    uint64_t marker = sample + 5U;
    uint16_t snap = (uint16_t)(sample - start);
    uint16_t live = (uint16_t)(snap + 5U);
    const unsigned values[TRACE_SCHEDULE_FIELD_COUNT] = {
        snap & 0xffU, snap >> 8U,
        24064U & 0xffU, 24064U >> 8U,
        24064U & 0xffU, 24064U >> 8U,
        6U, 6U
    };
    if (add_row(TRACE_KIND_EPOCH_MARKER, marker, live) != 0 || add_row(TRACE_KIND_PORTC_WRITE, marker, TRACE_SCHEDULE_MARKER) != 0) return -1;
    for (size_t i = 0U; i < TRACE_SCHEDULE_FIELD_COUNT; ++i)
        if (add_row(TRACE_KIND_PORTC_WRITE, marker + 1U + i, values[i]) != 0) return -1;
    const char strings[] = UART_DEFAULT_STREAM1_FRAME UART_DEFAULT_STREAM2_FRAME;
    const size_t strings_length = UART_DEFAULT_STREAM1_FRAME_BYTES + UART_DEFAULT_STREAM2_FRAME_BYTES;
    for (size_t i = 0U; i < strings_length; ++i)
        if (add_row(TRACE_KIND_PORTC_WRITE, marker + 9U + i, (unsigned char)strings[i]) != 0) return -1;
    return 0;
}

/* Создаём много эпох и строк под нагрузкой, чтобы проверить анализ пропусков и временных ошибок */
static int generate_stress(bool bad_timing)
{
    const uint64_t start = 1000U;
    const uint64_t epoch_span = 500000U;
    const uint64_t phi = UART_DEFAULT_STREAM1_FRAME_BYTES * UART_FRAME_CYCLES + UART_STATUS_TX_CYCLES + UART_CONTROL_SLOT_SETUP_CYCLES;
    const unsigned epoch_count = 20U;
    if (add_row(TRACE_KIND_TIMER1_START, start, 1U) != 0) return -1;
    for (unsigned epoch = 0U; epoch < epoch_count; ++epoch) {
        uint64_t sample = start + (uint64_t)epoch * epoch_span;
        uint64_t end = sample + epoch_span;
        if (epoch > 0U && add_stress_epoch_marker(sample) != 0) return -1;
        uint64_t a = sample + 24064U;
        uint64_t b = sample + phi + 24064U;
        unsigned slot = 0U;
        while (a + 5U * 1104U < end || b + 5U * 1104U < end) {
            if (a + 5U * 1104U < end) {
                if (add_uart_line(a, UART_DEFAULT_STREAM1_FRAME, UART_DEFAULT_STREAM1_FRAME_BYTES, bad_timing && epoch == 0U && slot == 0U) != 0) return -1;
                a += 24064U;
                ++slot;
            }
            if (b + 5U * 1104U < end) {
                if (add_uart_line(b, UART_DEFAULT_STREAM2_FRAME, UART_DEFAULT_STREAM2_FRAME_BYTES, false) != 0) return -1;
                b += 24064U;
            }
        }
    }
    return 0;
}

/* Выбираем тип фикстуры, сортируем события и записываем CSV для интеграционных тестов */
int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s task2|task2-bad|stress|stress-bad output.csv\n", argv[0]);
        return 2;
    }
    int status;
    if (strcmp(argv[1], "task2") == 0) status = generate_task2(false);
    else if (strcmp(argv[1], "task2-bad") == 0) status = generate_task2(true);
    else if (strcmp(argv[1], "stress") == 0) status = generate_stress(false);
    else if (strcmp(argv[1], "stress-bad") == 0) status = generate_stress(true);
    else { fprintf(stderr, "Unknown fixture '%s'\n", argv[1]); return 2; }
    if (status != 0) { fprintf(stderr, "Fixture generation failed\n"); free(rows); return 1; }
    /* Сортировка нужна потому, что генератор добавляет строки потоками, а не по хронологии */
    qsort(rows, row_count, sizeof(*rows), compare_rows);
    FILE *output = fopen(argv[2], "w");
    if (output == NULL) { perror(argv[2]); free(rows); return 1; }
    fprintf(output, "%s\n", TRACE_CSV_HEADER);
    for (size_t i = 0U; i < row_count; ++i)
        fprintf(output, "%c,\"%" PRIu64 "\",%u\n", rows[i].kind, rows[i].cycle, rows[i].value);
    if (fclose(output) != 0) { perror("fclose"); free(rows); return 1; }
    printf("Generated %zu trace rows in %s\n", row_count, argv[2]);
    free(rows);
    return 0;
}
