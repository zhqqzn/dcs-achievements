/*
 * Анализатор трассы во время изменений конфигурации.
 * Восстанавливает эпохи расписания из метаданных PORTC, декодирует UART-поток
 * и проверяет пропуски событий, целостность строк и четырёхтактный допуск.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trace_contract.h"

#define MAX_OUTPUT_BYTES 256U


typedef struct {
    uint64_t sample_cycle;
    uint32_t t1;
    uint32_t t2;
    unsigned char str1[TRACE_MAX_SCHEDULE_LINE_BYTES];
    unsigned char str2[TRACE_MAX_SCHEDULE_LINE_BYTES];
    size_t len1;
    size_t len2;
    uint32_t marker_delta;
} epoch_t;

typedef struct {
    uint64_t cycle;
    unsigned char bytes[MAX_OUTPUT_BYTES];
    size_t length;
} output_line_t;

typedef struct {
    char kind[4];
    int64_t index;
    int64_t expected;
    int64_t actual;
    int64_t error;
} timing_error_t;

/* Добавляем очередную сырую запись трассы, расширяя массив по мере заполнения */
static int append_row(trace_record_t **rows, size_t *count, size_t *capacity, trace_record_t row)
{
    if (*count == *capacity) {
        size_t next = *capacity == 0U ? 1024U : *capacity * 2U;
        trace_record_t *expanded = realloc(*rows, next * sizeof(**rows));
        if (expanded == NULL) return -1;
        *rows = expanded;
        *capacity = next;
    }
    (*rows)[(*count)++] = row;
    return 0;
}

/* Сохраняем снимок расписания, действовавшего после изменения параметров или при старте */
static int append_epoch(epoch_t **epochs, size_t *count, size_t *capacity, const epoch_t *epoch)
{
    if (*count == *capacity) {
        size_t next = *capacity == 0U ? 32U : *capacity * 2U;
        epoch_t *expanded = realloc(*epochs, next * sizeof(**epochs));
        if (expanded == NULL) return -1;
        *epochs = expanded;
        *capacity = next;
    }
    (*epochs)[(*count)++] = *epoch;
    return 0;
}

/* Сортировка по восстановленному моменту снимка нужна, если записи меток пришли с задержкой */
static int compare_epochs(const void *left, const void *right)
{
    const epoch_t *a = left;
    const epoch_t *b = right;
    if (a->sample_cycle < b->sample_cycle) return -1;
    if (a->sample_cycle > b->sample_cycle) return 1;
    return 0;
}

/* Добавляем полностью собранную UART-строку; её cycle — время первого байта */
static int append_line(output_line_t **lines, size_t *count, size_t *capacity,
                       const output_line_t *line)
{
    if (*count == *capacity) {
        size_t next = *capacity == 0U ? 256U : *capacity * 2U;
        output_line_t *expanded = realloc(*lines, next * sizeof(**lines));
        if (expanded == NULL) return -1;
        *lines = expanded;
        *capacity = next;
    }
    (*lines)[(*count)++] = *line;
    return 0;
}

/* Запоминаем измеренную ошибку отдельно от строки, чтобы затем вывести общую статистику */
static int append_error(timing_error_t **errors, size_t *count, size_t *capacity,
                        const char *kind, int64_t index, int64_t expected,
                        int64_t actual, int64_t error)
{
    if (*count == *capacity) {
        size_t next = *capacity == 0U ? 256U : *capacity * 2U;
        timing_error_t *expanded = realloc(*errors, next * sizeof(**errors));
        if (expanded == NULL) return -1;
        *errors = expanded;
        *capacity = next;
    }
    timing_error_t *item = &(*errors)[(*count)++];
    (void)snprintf(item->kind, sizeof(item->kind), "%s", kind);
    item->index = index;
    item->expected = expected;
    item->actual = actual;
    item->error = error;
    return 0;
}

/* Проверяем совпадение активной пользовательской строки без преобразования байтов */
static bool line_equals(const output_line_t *line, const unsigned char *bytes, size_t len)
{
    return line->length == len && memcmp(line->bytes, bytes, len) == 0;
}

/* Ответ OK/ERR распознаётся только с обязательным завершением CR/LF */
static bool line_is_status(const output_line_t *line, const char *status)
{
    size_t n = strlen(status);
    return line->length == n + 2U && memcmp(line->bytes, status, n) == 0 &&
           line->bytes[n] == '\r' && line->bytes[n + 1U] == '\n';
}

/* Читаем CSV целиком, запоминаем эпоху старта Timer1 и считаем все записи UDR */
static int read_all_rows(const char *path, trace_record_t **rows, size_t *count,
                         uint64_t *start_cycle, size_t *udr_write_count)
{
    FILE *file = fopen(path, "r");
    char *line = NULL;
    size_t capacity = 0U;
    ssize_t length;
    size_t row_capacity = 0U;
    if (file == NULL) {
        fprintf(stderr, "FAIL: cannot read trace %s: %s\n", path, strerror(errno));
        return -1;
    }
    /* Заголовок проверяется явно: пустой файл или CSV другого формата не должен приниматься молча */
    length = getline(&line, &capacity, file);
    if (length < 0) {
        fprintf(stderr, "FAIL: trace is empty or missing the CSV header\n");
        free(line); fclose(file); return -1;
    }
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
        line[--length] = '\0';
    if (strcmp(line, TRACE_CSV_HEADER) != 0) {
        fprintf(stderr, "FAIL: invalid CSV header: %s\n", line);
        free(line); fclose(file); return -1;
    }
    while ((length = getline(&line, &capacity, file)) >= 0) {
        trace_record_t row;
        (void)length;
        if (trace_contract_parse_record(line, &row) != 0) {
            fprintf(stderr, "FAIL: malformed CSV row: %s", line);
            free(line); fclose(file); return -1;
        }
        if (row.kind == TRACE_KIND_TIMER1_START && *start_cycle == UINT64_MAX)
            *start_cycle = row.cycle;
        if (row.kind == TRACE_KIND_UDR_WRITE)
            ++*udr_write_count;
        if (append_row(rows, count, &row_capacity, row) != 0) {
            fprintf(stderr, "FAIL: out of memory reading trace\n");
            free(line); fclose(file); return -1;
        }
    }
    free(line);
    fclose(file);
    return 0;
}

/* Восстанавливаем из последовательности PORTC-меток периоды, строки и момент снимка расписания */
static int decode_epochs(const trace_record_t *rows, size_t row_count,
                         uint64_t start_cycle, epoch_t **epochs,
                         size_t *epoch_count)
{
    size_t capacity = 0U;
    epoch_t startup;
    memset(&startup, 0, sizeof(startup));
    startup.sample_cycle = start_cycle;
    startup.t1 = 24064U;
    startup.t2 = 24064U;
    memcpy(startup.str1, UART_DEFAULT_STREAM1_FRAME, UART_DEFAULT_STREAM1_FRAME_BYTES); startup.len1 = UART_DEFAULT_STREAM1_FRAME_BYTES;
    memcpy(startup.str2, UART_DEFAULT_STREAM2_FRAME, UART_DEFAULT_STREAM2_FRAME_BYTES); startup.len2 = UART_DEFAULT_STREAM2_FRAME_BYTES;
    if (append_epoch(epochs, epoch_count, &capacity, &startup) != 0)
        return -1;

    /* Запись E предваряет кадр метаданных P: сигнальный байт, поля снимка, периоды и обе строки */
    for (size_t i = 0U; i < row_count; ++i) {
        if (rows[i].kind != TRACE_KIND_EPOCH_MARKER) continue;
        uint64_t marker_cycle = rows[i].cycle;
        uint32_t live_count = rows[i].value & 0xffffU;
        if (i + 1U + TRACE_SCHEDULE_FIELD_COUNT >= row_count || rows[i + 1U].kind != TRACE_KIND_PORTC_WRITE ||
            rows[i + 1U].value != TRACE_SCHEDULE_MARKER) {
            fprintf(stderr, "FAIL: epoch row at index %zu lacks marker 0x%02X\n", i, (unsigned)TRACE_SCHEDULE_MARKER);
            return -1;
        }
        unsigned field[TRACE_SCHEDULE_FIELD_COUNT];
        for (size_t j = 0U; j < TRACE_SCHEDULE_FIELD_COUNT; ++j) {
            const trace_record_t *r = &rows[i + 2U + j];
            if (r->kind != TRACE_KIND_PORTC_WRITE) {
                fprintf(stderr, "FAIL: incomplete timing metadata after cycle=%" PRIu64 "\n", marker_cycle);
                return -1;
            }
            field[j] = r->value & 0xffU;
        }
        uint16_t snap = (uint16_t)(field[0] | (field[1] << 8U));
        uint32_t t1 = field[2] | (field[3] << 8U);
        uint32_t t2 = field[4] | (field[5] << 8U);
        unsigned len1 = field[6], len2 = field[7];
        if (t1 == 0U || t2 == 0U || len1 < 2U || len1 > TRACE_MAX_SCHEDULE_LINE_BYTES ||
            len2 < 2U || len2 > TRACE_MAX_SCHEDULE_LINE_BYTES) {
            fprintf(stderr, "FAIL: invalid schedule marker lengths/periods at cycle=%" PRIu64
                    ": periods=%u,%u lengths=%u,%u\n", marker_cycle, t1, t2, len1, len2);
            return -1;
        }
        size_t payload_start = i + 2U + TRACE_SCHEDULE_FIELD_COUNT;
        size_t payload_len = (size_t)len1 + len2;
        if (payload_start + payload_len > row_count) {
            fprintf(stderr, "FAIL: truncated schedule string payload at cycle=%" PRIu64 "\n", marker_cycle);
            return -1;
        }
        epoch_t epoch;
        memset(&epoch, 0, sizeof(epoch));
        epoch.t1 = t1; epoch.t2 = t2;
        epoch.len1 = len1; epoch.len2 = len2;
        for (size_t j = 0U; j < payload_len; ++j) {
            if (rows[payload_start + j].kind != TRACE_KIND_PORTC_WRITE) {
                fprintf(stderr, "FAIL: non-marker row inside schedule string payload at cycle=%" PRIu64 "\n", marker_cycle);
                return -1;
            }
            if (j < len1) epoch.str1[j] = (unsigned char)(rows[payload_start + j].value & 0xffU);
            else epoch.str2[j - len1] = (unsigned char)(rows[payload_start + j].value & 0xffU);
        }
        if (epoch.str1[len1 - 2U] != '\r' || epoch.str1[len1 - 1U] != '\n' ||
            epoch.str2[len2 - 2U] != '\r' || epoch.str2[len2 - 1U] != '\n') {
            fprintf(stderr, "FAIL: active strings lack CRLF at cycle=%" PRIu64 "\n", marker_cycle);
            return -1;
        }
        /* Счётчик Timer1 16-разрядный: разность вычисляется по модулю 65536 */
        uint32_t delta = (live_count - snap) & 0xffffU;
        if (delta > 128U) {
            fprintf(stderr, "FAIL: implausible marker counter delta at cycle=%" PRIu64 ": %u\n", marker_cycle, delta);
            return -1;
        }
        epoch.marker_delta = delta;
        epoch.sample_cycle = marker_cycle - delta;
        if (append_epoch(epochs, epoch_count, &capacity, &epoch) != 0)
            return -1;
        i = payload_start + payload_len - 1U;
    }
    qsort(*epochs, *epoch_count, sizeof(**epochs), compare_epochs);
    return 0;
}

/* Преобразуем поток отдельных UDR-байтов в строки; неполный хвост оставляем для диагностики */
static int decode_uart_lines(const trace_record_t *rows, size_t row_count,
                             output_line_t **lines, size_t *line_count,
                             unsigned char *partial, size_t *partial_len,
                             uint64_t *partial_cycle)
{
    size_t capacity = 0U;
    unsigned char pending[MAX_OUTPUT_BYTES];
    size_t pending_len = 0U;
    uint64_t first_cycle = 0U;
    for (size_t i = 0U; i < row_count; ++i) {
        if (rows[i].kind != TRACE_KIND_UDR_WRITE) continue;
        if (pending_len == 0U) first_cycle = rows[i].cycle;
        if (pending_len >= sizeof(pending)) {
            fprintf(stderr, "FAIL: UART output line exceeds %u bytes\n", MAX_OUTPUT_BYTES);
            return -1;
        }
        pending[pending_len++] = (unsigned char)(rows[i].value & 0xffU);
        if (rows[i].value == '\n') {
            output_line_t line;
            memset(&line, 0, sizeof(line));
            line.cycle = first_cycle;
            memcpy(line.bytes, pending, pending_len);
            line.length = pending_len;
            if (append_line(lines, line_count, &capacity, &line) != 0) return -1;
            pending_len = 0U;
        }
    }
    *partial_len = pending_len;
    if (pending_len > 0U) {
        memcpy(partial, pending, pending_len);
        *partial_cycle = first_cycle;
    }
    return 0;
}

/* Находим ближайший индекс периодического слота; отрицательные значения округляет вниз корректно для C */
static int64_t nearest_grid_index(int64_t actual, int64_t base, int64_t period)
{
    int64_t delta = actual - base;
    int64_t shifted = delta + period / 2;
    if (shifted >= 0)
        return shifted / period;
    /* Деление целых в C округляет к нулю; здесь вручную получаем округление вниз */
    return -((-shifted + period - 1) / period);
}

/* Выводим допустимые аргументы анализатора командной трассы */
static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s TRACE.csv [--tolerance N] [--minimum-epochs N] [--minimum-events N]\n", program);
}

/* Сверяем каждую полученную строку с последним известным расписанием и проверяем допуск ±tolerance */
int main(int argc, char **argv)
{
    const char *path = NULL;
    int64_t tolerance = 4;
    size_t minimum_epochs = 20U, minimum_events = 10U;
    trace_record_t *rows = NULL;
    size_t row_count = 0U, udr_count = 0U;
    uint64_t start_cycle = UINT64_MAX;
    epoch_t *epochs = NULL;
    size_t epoch_count = 0U;
    output_line_t *lines = NULL;
    size_t line_count = 0U;
    unsigned char partial[MAX_OUTPUT_BYTES];
    size_t partial_len = 0U;
    uint64_t partial_cycle = 0U;
    timing_error_t *errors = NULL;
    size_t error_count = 0U, error_capacity = 0U;
    char malformed[20][200];
    size_t malformed_count = 0U;
    char sequence_errors[20][240];
    size_t sequence_error_count = 0U;
    size_t counts_a = 0U, counts_b = 0U, count_ok = 0U, count_err = 0U;
    size_t *per_epoch_a = NULL, *per_epoch_b = NULL;
    int result = 2;

    for (int arg = 1; arg < argc; ++arg) {
        if (strcmp(argv[arg], "--tolerance") == 0 && arg + 1 < argc) tolerance = strtoll(argv[++arg], NULL, 10);
        else if (strcmp(argv[arg], "--minimum-epochs") == 0 && arg + 1 < argc) minimum_epochs = (size_t)strtoul(argv[++arg], NULL, 10);
        else if (strcmp(argv[arg], "--minimum-events") == 0 && arg + 1 < argc) minimum_events = (size_t)strtoul(argv[++arg], NULL, 10);
        else if (argv[arg][0] == '-' || path != NULL) { usage(argv[0]); return 2; }
        else path = argv[arg];
    }
    if (path == NULL || tolerance < 0 || minimum_epochs < 1U || minimum_events < 1U) {
        usage(argv[0]); return 2;
    }
    if (read_all_rows(path, &rows, &row_count, &start_cycle, &udr_count) != 0)
        goto cleanup;
    if (start_cycle == UINT64_MAX) {
        fprintf(stderr, "FAIL: trace has no Timer1 start row (S)\n");
        goto cleanup;
    }
    if (decode_epochs(rows, row_count, start_cycle, &epochs, &epoch_count) != 0)
        goto cleanup;
    if (decode_uart_lines(rows, row_count, &lines, &line_count, partial, &partial_len, &partial_cycle) != 0)
        goto cleanup;

    per_epoch_a = calloc(epoch_count, sizeof(*per_epoch_a));
    per_epoch_b = calloc(epoch_count, sizeof(*per_epoch_b));
    if (per_epoch_a == NULL || per_epoch_b == NULL) goto cleanup;

    /* Для каждой полной строки выбираем актуальную эпоху, проверяем текст и ожидаемый номер слота. */
    for (size_t i = 0U; i < line_count; ++i) {
        const output_line_t *line = &lines[i];
        if (line->length < 2U || line->bytes[line->length - 2U] != '\r' ||
            line->bytes[line->length - 1U] != '\n') {
            if (malformed_count < 20U)
                (void)snprintf(malformed[malformed_count], sizeof(malformed[malformed_count]),
                               "cycle=%" PRIu64 ": line is not CRLF-terminated", line->cycle);
            ++malformed_count; continue;
        }
        if (line_is_status(line, UART_STATUS_OK_TEXT)) { ++count_ok; continue; }
        if (line_is_status(line, UART_STATUS_ERR_TEXT)) { ++count_err; continue; }

        size_t ei = epoch_count;
        for (size_t j = 0U; j < epoch_count; ++j) {
            if (epochs[j].sample_cycle <= line->cycle) ei = j;
            else break;
        }
        if (ei == epoch_count) {
            if (malformed_count < 20U)
                (void)snprintf(malformed[malformed_count], sizeof(malformed[malformed_count]),
                               "cycle=%" PRIu64 ": line predates any schedule epoch", line->cycle);
            ++malformed_count; continue;
        }
        epoch_t *epoch = &epochs[ei];
        const char *stream = NULL;
        uint32_t period = 0U;
        int64_t base = 0;
        size_t *epoch_count_for_stream = NULL;
        if (line_equals(line, epoch->str1, epoch->len1)) {
            stream = "A"; period = epoch->t1;
            base = (int64_t)(epoch->sample_cycle + epoch->t1);
            epoch_count_for_stream = &per_epoch_a[ei];
        } else if (line_equals(line, epoch->str2, epoch->len2)) {
            stream = "B"; period = epoch->t2;
            base = (int64_t)(epoch->sample_cycle + epoch->len1 * UART_FRAME_CYCLES +
                             UART_STATUS_TX_CYCLES + UART_CONTROL_SLOT_SETUP_CYCLES + epoch->t2);
            epoch_count_for_stream = &per_epoch_b[ei];
        } else {
            if (malformed_count < 20U)
                (void)snprintf(malformed[malformed_count], sizeof(malformed[malformed_count]),
                               "cycle=%" PRIu64 ": does not match active string in latest epoch", line->cycle);
            ++malformed_count; continue;
        }
        /* Ближайший слот показывает временную ошибку; отдельно ниже проверяется, не пропущен ли номер слота */
        int64_t index = nearest_grid_index((int64_t)line->cycle, base, period);
        int64_t expected = base + index * (int64_t)period;
        int64_t error = (int64_t)line->cycle - expected;
        if (index != (int64_t)*epoch_count_for_stream && sequence_error_count < 20U) {
            (void)snprintf(sequence_errors[sequence_error_count], sizeof(sequence_errors[sequence_error_count]),
                           "epoch=%zu stream=%s: observed slot index %" PRId64 ", expected %zu (cycle=%" PRIu64 ")",
                           ei, stream, index, *epoch_count_for_stream, line->cycle);
            ++sequence_error_count;
        }
        if (index >= 0 && (size_t)(index + 1) > *epoch_count_for_stream)
            *epoch_count_for_stream = (size_t)(index + 1);
        else
            ++*epoch_count_for_stream;
        if (append_error(&errors, &error_count, &error_capacity, stream, index,
                         expected, (int64_t)line->cycle, error) != 0) goto cleanup;
        if (strcmp(stream, "A") == 0) ++counts_a; else ++counts_b;
    }

    printf("Timer1 start cycle: %" PRIu64 "\n", start_cycle);
    printf("Schedule epochs (including startup): %zu\n", epoch_count);
    printf("UDR writes: %zu; complete lines: %zu\n", udr_count, line_count);
    uint32_t delta_min = 0U, delta_max = 0U;
    if (epoch_count > 1U) {
        delta_min = epochs[1].marker_delta; delta_max = epochs[1].marker_delta;
        for (size_t i = 2U; i < epoch_count; ++i) {
            if (epochs[i].marker_delta < delta_min) delta_min = epochs[i].marker_delta;
            if (epochs[i].marker_delta > delta_max) delta_max = epochs[i].marker_delta;
        }
    }
    printf("Rebase marker counter delta range: %u..%u cycles\n", delta_min, delta_max);
    printf("Timed lines: A/string1=%zu; B/string2=%zu; status OK=%zu ERR=%zu\n",
           counts_a, counts_b, count_ok, count_err);
    if (partial_len > 0U) {
        printf("Partial final UART bytes ignored (trace stopped mid-line) at cycle=%" PRIu64 ": ", partial_cycle);
        for (size_t i = 0U; i < partial_len; ++i) printf("%02x ", partial[i]);
        putchar('\n');
    }
    if (error_count > 0U) {
        int64_t min_error = errors[0].error, max_error = errors[0].error;
        for (size_t i = 1U; i < error_count; ++i) {
            if (errors[i].error < min_error) min_error = errors[i].error;
            if (errors[i].error > max_error) max_error = errors[i].error;
        }
        printf("UDR start error relative to each active schedule: min=%" PRId64 " max=%" PRId64
               " cycles; tolerance=±%" PRId64 "\n", min_error, max_error, tolerance);
        puts("First 12 timed events (stream, index, expected, actual, error):");
        for (size_t i = 0U; i < error_count && i < 12U; ++i)
            printf("  %-4s %3" PRId64 " expected=%" PRId64 " actual=%" PRId64 " error=%+" PRId64 "\n",
                   errors[i].kind, errors[i].index, errors[i].expected, errors[i].actual, errors[i].error);
    }
    if (sequence_error_count > 0U) {
        puts("Missing/duplicated timed schedule slots:");
        for (size_t i = 0U; i < sequence_error_count; ++i) printf("  %s\n", sequence_errors[i]);
    }
    if (malformed_count > 0U) {
        puts("Malformed/stale output lines:");
        size_t shown = malformed_count < 20U ? malformed_count : 20U;
        for (size_t i = 0U; i < shown; ++i) printf("  %s\n", malformed[i]);
        if (malformed_count > shown) printf("  ... %zu more\n", malformed_count - shown);
    }

    bool enough = counts_a >= minimum_events && counts_b >= minimum_events;
    bool enough_epochs = epoch_count >= minimum_epochs;
    bool timings_good = error_count > 0U;
    for (size_t i = 0U; i < error_count; ++i)
        if (errors[i].error < -tolerance || errors[i].error > tolerance) timings_good = false;
    bool integrity_good = malformed_count == 0U && sequence_error_count == 0U;
    if (enough && enough_epochs && timings_good && integrity_good) {
        puts("PASS: command-load trace has intact lines and all measured first-byte writes within the cycle bound");
        puts("Scope: this is evidence for this linked firmware and the installed simavr model only.");
        result = 0;
    } else {
        printf("Epoch requirement: %zu observed, %zu required\n", epoch_count, minimum_epochs);
        puts("FAIL: insufficient epochs/events, missing schedule slots, corrupted lines, or timing error beyond the bound");
        puts("Scope: simulator trace is meaningful only if simavr emits one record per UDR write and marker byte.");
        result = 1;
    }

cleanup:
    free(rows); free(epochs); free(lines); free(errors); free(per_epoch_a); free(per_epoch_b);
    return result;
}
