/*
 * Анализатор периодической трассы записей UDR.
 * Собирает байты в строки, проверяет CR/LF и сравнивает начало каждой строки
 * с идеальным расписанием в тактах CPU.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char kind;
    uint64_t cycle;
    unsigned value;
} trace_row_t;

typedef struct {
    uint64_t cycle;
    unsigned char bytes[256];
    size_t length;
} output_line_t;

typedef struct {
    uint64_t cycle;
    unsigned char value;
} udr_event_t;

/* Разбираем одну запись CSV. Поддерживаются оба варианта поля cycle: в кавычках и без них */
static int parse_row(char *line, trace_row_t *row)
{
    unsigned long long cycle;
    unsigned value;
    char kind;
    int fields = sscanf(line, " %c , \"%llu\" , %u", &kind, &cycle, &value);
    if (fields != 3)
        fields = sscanf(line, " %c , %llu , %u", &kind, &cycle, &value);
    if (fields != 3)
        return -1;
    row->kind = kind;
    row->cycle = (uint64_t)cycle;
    row->value = value;
    return 0;
}

/* Добавляем байт UDR в растущий массив событий; при нехватке места увеличиваем ёмкость вдвое */
static int append_event(udr_event_t **events, size_t *count, size_t *capacity,
                        uint64_t cycle, unsigned value)
{
    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0U ? 256U : *capacity * 2U;
        udr_event_t *new_events = realloc(*events, new_capacity * sizeof(**events));
        if (new_events == NULL)
            return -1;
        *events = new_events;
        *capacity = new_capacity;
    }
    (*events)[*count].cycle = cycle;
    (*events)[*count].value = (unsigned char)(value & 0xffU);
    ++*count;
    return 0;
}

/* Сохраняем собранную строку и такт её первого байта, а не завершающего LF */
static int append_line(output_line_t **lines, size_t *count, size_t *capacity,
                       uint64_t cycle, const unsigned char *bytes, size_t length)
{
    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0U ? 128U : *capacity * 2U;
        output_line_t *new_lines = realloc(*lines, new_capacity * sizeof(**lines));
        if (new_lines == NULL)
            return -1;
        *lines = new_lines;
        *capacity = new_capacity;
    }
    output_line_t *line = &(*lines)[(*count)++];
    line->cycle = cycle;
    if (length > sizeof(line->bytes))
        length = sizeof(line->bytes);
    memcpy(line->bytes, bytes, length);
    line->length = length;
    return 0;
}

/* Сравниваем строку побайтно, включая CR/LF: визуального совпадения текста недостаточно */
static bool bytes_equal(const output_line_t *line, const char *text)
{
    size_t len = strlen(text);
    return line->length == len && memcmp(line->bytes, text, len) == 0;
}

/* Печатаем единую форму вызова, если аргументы не прошли проверку */
static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s TRACE.csv [--t1 cycles] [--t2 cycles] [--phi cycles] [--tolerance cycles] [--minimum-lines N]\n",
            program);
}

/* Полный проход: прочитать CSV, восстановить строки, сопоставить их с расписанием и вывести отклонения */
int main(int argc, char **argv)
{
    const char *path = NULL;
    uint64_t t1 = 16384U, t2 = 16384U, phi = 8192U;
    int64_t tolerance = 4;
    size_t minimum_lines = 10U;
    FILE *input = NULL;
    char *buffer = NULL;
    size_t buffer_size = 0U;
    ssize_t nread;
    uint64_t epoch = UINT64_MAX;
    udr_event_t *events = NULL;
    size_t event_count = 0U, event_capacity = 0U;
    output_line_t *lines = NULL;
    size_t line_count = 0U, line_capacity = 0U;
    unsigned char pending[256];
    size_t pending_len = 0U;
    uint64_t pending_cycle = 0U;
    uint64_t *ping_cycles = NULL, *pong_cycles = NULL;
    size_t ping_count = 0U, pong_count = 0U, i;
    typedef struct { const char *kind; size_t index; uint64_t expected; int64_t error; } timing_error_t;
    timing_error_t *errors = NULL;
    size_t errors_count = 0U, errors_capacity = 0U;
    size_t malformed_count = 0U;
    int result = 1;

    for (int arg = 1; arg < argc; ++arg) {
        if (strcmp(argv[arg], "--t1") == 0 && arg + 1 < argc) t1 = strtoull(argv[++arg], NULL, 10);
        else if (strcmp(argv[arg], "--t2") == 0 && arg + 1 < argc) t2 = strtoull(argv[++arg], NULL, 10);
        else if (strcmp(argv[arg], "--phi") == 0 && arg + 1 < argc) phi = strtoull(argv[++arg], NULL, 10);
        else if (strcmp(argv[arg], "--tolerance") == 0 && arg + 1 < argc) tolerance = strtoll(argv[++arg], NULL, 10);
        else if (strcmp(argv[arg], "--minimum-lines") == 0 && arg + 1 < argc) minimum_lines = (size_t)strtoul(argv[++arg], NULL, 10);
        else if (argv[arg][0] == '-' || path != NULL) { usage(argv[0]); return 2; }
        else path = argv[arg];
    }
    if (path == NULL || t1 == 0U || t2 == 0U || tolerance < 0 || minimum_lines == 0U) {
        usage(argv[0]);
        return 2;
    }
    input = fopen(path, "r");
    if (input == NULL) {
        fprintf(stderr, "FAIL: cannot read trace %s: %s\n", path, strerror(errno));
        return 2;
    }
    /* Заголовок проверяется явно: пустой файл или CSV другого формата не должен приниматься молча. */
    nread = getline(&buffer, &buffer_size, input);
    if (nread < 0) {
        fprintf(stderr, "FAIL: trace is empty or missing the CSV header\n");
        goto cleanup;
    }
    while (nread > 0 && (buffer[nread - 1] == '\n' || buffer[nread - 1] == '\r'))
        buffer[--nread] = '\0';
    if (strcmp(buffer, "kind,cycle,value") != 0) {
        fprintf(stderr, "FAIL: invalid CSV header: %s\n", buffer);
        goto cleanup;
    }

    while ((nread = getline(&buffer, &buffer_size, input)) >= 0) {
        trace_row_t row;
        if (parse_row(buffer, &row) != 0) {
            fprintf(stderr, "FAIL: malformed trace CSV row: %s", buffer);
            goto cleanup;
        }
        if (row.kind == 'S' && epoch == UINT64_MAX)
            epoch = row.cycle;
        if (row.kind == 'U' && append_event(&events, &event_count, &event_capacity, row.cycle, row.value) != 0) {
            fprintf(stderr, "FAIL: out of memory reading UDR events\n");
            goto cleanup;
        }
    }
    fclose(input); input = NULL;
    free(buffer); buffer = NULL;
    if (epoch == UINT64_MAX) {
        fprintf(stderr, "FAIL: no Timer1 epoch record in trace\n");
        goto cleanup;
    }

    /* Собираем байты до LF. Метка строки берётся с первого UDR-записывания */
    for (i = 0U; i < event_count; ++i) {
        if (pending_len == 0U)
            pending_cycle = events[i].cycle;
        if (pending_len < sizeof(pending))
            pending[pending_len++] = events[i].value;
        if (events[i].value == '\n') {
            if (append_line(&lines, &line_count, &line_capacity, pending_cycle, pending, pending_len) != 0) {
                fprintf(stderr, "FAIL: out of memory decoding output lines\n");
                goto cleanup;
            }
            pending_len = 0U;
        }
    }

    /* Отдельные массивы позволяют независимо сопоставить строки двух потоков */
    ping_cycles = calloc(line_count + 1U, sizeof(*ping_cycles));
    pong_cycles = calloc(line_count + 1U, sizeof(*pong_cycles));
    errors_capacity = line_count + 1U;
    errors = calloc(errors_capacity, sizeof(*errors));
    if (ping_cycles == NULL || pong_cycles == NULL || errors == NULL) {
        fprintf(stderr, "FAIL: out of memory analyzing output\n");
        goto cleanup;
    }
    /* Строки классифицируются по точному содержимому, затем их первые байты проверяются по сетке периодов */
    for (i = 0U; i < line_count; ++i) {
        if (bytes_equal(&lines[i], "ping\r\n")) ping_cycles[ping_count++] = lines[i].cycle;
        else if (bytes_equal(&lines[i], "pong\r\n")) pong_cycles[pong_count++] = lines[i].cycle;
        else {
            ++malformed_count;
            if (malformed_count <= 10U) {
                fprintf(stderr, "Malformed/unexpected complete output line at cycle=%" PRIu64 ": ", lines[i].cycle);
                for (size_t j = 0U; j < lines[i].length; ++j)
                    fprintf(stderr, "%02x ", lines[i].bytes[j]);
                fputc('\n', stderr);
            }
        }
    }

    for (size_t stream_index = 0U; stream_index < 2U; ++stream_index) {
        const char *name = stream_index == 0U ? "ping" : "pong";
        uint64_t period = stream_index == 0U ? t1 : t2;
        uint64_t offset = stream_index == 0U ? t1 : phi + t2;
        uint64_t *cycles = stream_index == 0U ? ping_cycles : pong_cycles;
        size_t count = stream_index == 0U ? ping_count : pong_count;
        for (i = 0U; i < count; ++i) {
            uint64_t expected = epoch + offset + (uint64_t)i * period;
            int64_t error = (int64_t)cycles[i] - (int64_t)expected;
            if (errors_count < errors_capacity) {
                errors[errors_count].kind = name;
                errors[errors_count].index = i;
                errors[errors_count].expected = expected;
                errors[errors_count].error = error;
            } else {
                /* Ёмкость массива рассчитана по числу строк; эта ветвь защищает от ошибки границ */
                break;
            }
            ++errors_count;
        }
    }

    printf("Epoch (TCCR1B start write): %" PRIu64 " cycles\n", epoch);
    printf("UDR writes: %zu; complete lines: %zu\n", event_count, line_count);
    printf("ping lines: %zu; pong lines: %zu\n", ping_count, pong_count);
    if (malformed_count > 10U)
        printf("... and %zu more malformed/unexpected line(s)\n", malformed_count - 10U);
    if (pending_len > 0U) {
        printf("Partial trailing bytes at cycle %" PRIu64 " ignored (trace ended mid-line): ", pending_cycle);
        for (i = 0U; i < pending_len; ++i) printf("%02x ", pending[i]);
        putchar('\n');
    }
    if (errors_count > 0U) {
        int64_t minimum = errors[0].error, maximum = errors[0].error;
        for (i = 1U; i < errors_count; ++i) {
            if (errors[i].error < minimum) minimum = errors[i].error;
            if (errors[i].error > maximum) maximum = errors[i].error;
        }
        printf("UDR-write error: min=%" PRId64 " max=%" PRId64 " cycles; tolerance=±%" PRId64 "\n",
               minimum, maximum, tolerance);
        puts("First 12 events (type, index, expected, error):");
        for (i = 0U; i < errors_count && i < 12U; ++i)
            printf("  %-4s %3zu expected=%" PRIu64 " error=%+" PRId64 "\n",
                   errors[i].kind, errors[i].index, errors[i].expected, errors[i].error);
    } else {
        puts("FAIL: no recognizable ping/pong starts");
        goto cleanup;
    }

    bool timing_ok = true;
    for (i = 0U; i < errors_count; ++i) {
        if (errors[i].error < -tolerance || errors[i].error > tolerance) {
            timing_ok = false;
            break;
        }
    }
    if (ping_count >= minimum_lines && pong_count >= minimum_lines && malformed_count == 0U &&
        pending_len == 0U && timing_ok) {
        puts("PASS: all observed first-byte UDR writes are within the configured bound");
        puts("Scope: simavr model only; this does not prove physical-hardware timing.");
        result = 0;
    } else {
        puts("FAIL: event count, output integrity, or timing bound not satisfied");
        puts("Scope: simulator trace is meaningful only if the installed simavr version emits an IOMEM IRQ on each UDR write.");
    }

cleanup:
    if (input != NULL) fclose(input);
    free(buffer); free(events); free(lines); free(ping_cycles); free(pong_cycles); free(errors);
    return result;
}
