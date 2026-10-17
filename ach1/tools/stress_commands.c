/*
 * Автоматический стресс-клиент USART для ATmega8 в simavr
 * Команды отправляются по одной: следующая начинается только после OK/ERR
 * Это проверяет динамическую перенастройку, отклонение плохих значений и
 * целостность строк, не создавая искусственного переполнения очереди ответов
 */

#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_TEXT "ABCDEFGHIJKLMNOPQRSTUV"
#define RX_CAPACITY 8192U
#define MAX_LINE 256U
#define MAX_TRACKED_LINES 512U
#define MAX_STATUS_QUEUE 512U
#define MAX_BAD_LINES 64U
#define BASE_LINE_COUNT 2U
#define MAX_ALLOWED_LINES 256U

static const char *const base_lines[BASE_LINE_COUNT] = {"ping", "pong"};

typedef struct {
    char text[MAX_LINE];
    unsigned long count;
} line_counter_t;

/* Общее состояние UART-клиента: буфер RX, очередь статусов и результаты проверки данных */
typedef struct {
    int fd;
    double status_timeout;
    double line_timeout;
    unsigned char rx[RX_CAPACITY];
    size_t rx_length;
    char statuses[MAX_STATUS_QUEUE][4];
    size_t status_head;
    size_t status_count;
    char allowed[MAX_ALLOWED_LINES][MAX_LINE];
    size_t allowed_count;
    line_counter_t data[MAX_TRACKED_LINES];
    size_t data_count;
    char bad[MAX_BAD_LINES][MAX_LINE];
    size_t bad_count;
    bool saw_clean_data;
    unsigned long initial_fragments_ignored;
    unsigned long commands_sent;
    unsigned long ok_count;
    unsigned long err_count;
} uart_client_t;

/* Монотонные часы не прыгают при коррекции системного времени, поэтому подходят для тайм-аутов */
static double monotonic_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Проверяем, разрешена ли строка в текущем сценарии; любое другое сообщение считается ошибкой вывода */
static bool allowed_has(const uart_client_t *client, const char *line)
{
    for (size_t i = 0U; i < client->allowed_count; ++i)
        if (strcmp(client->allowed[i], line) == 0) return true;
    return false;
}

/* Добавляем новую допустимую строку без дубликатов, сохраняя ограничение размера таблицы */
static int allowed_add(uart_client_t *client, const char *line)
{
    if (allowed_has(client, line)) return 0;
    if (client->allowed_count >= MAX_ALLOWED_LINES) return -1;
    (void)snprintf(client->allowed[client->allowed_count++], MAX_LINE, "%s", line);
    return 0;
}

/* Находим счётчик строки или создаём его по запросу вызывающего кода */
static line_counter_t *data_entry(uart_client_t *client, const char *line, bool create)
{
    for (size_t i = 0U; i < client->data_count; ++i)
        if (strcmp(client->data[i].text, line) == 0) return &client->data[i];
    if (!create || client->data_count >= MAX_TRACKED_LINES) return NULL;
    line_counter_t *entry = &client->data[client->data_count++];
    (void)snprintf(entry->text, sizeof(entry->text), "%s", line);
    entry->count = 0UL;
    return entry;
}

/* Возвращаем число наблюдений строки; отсутствие записи эквивалентно нулю */
static unsigned long data_count(uart_client_t *client, const char *line)
{
    line_counter_t *entry = data_entry(client, line, false);
    return entry == NULL ? 0UL : entry->count;
}

/* Распознаём обрывки ping/pong, возможные при подключении к PTY посреди передачи */
static bool is_timer_fragment(const char *line)
{
    for (size_t m = 0U; m < BASE_LINE_COUNT; ++m) {
        size_t length = strlen(base_lines[m]);
        for (size_t i = 1U; i < length; ++i) {
            if (strcmp(line, base_lines[m] + i) == 0 ||
                (strlen(line) == i && strncmp(line, base_lines[m], i) == 0))
                return true;
        }
    }
    return false;
}

/* Делим ответы статуса и периодические строки; неизвестный текст регистрируется как повреждение */
static void classify_line(uart_client_t *client, const unsigned char *raw, size_t length)
{
    char line[MAX_LINE];
    size_t begin = 0U;
    while (length > 0U && (raw[length - 1U] == '\r' || raw[length - 1U] == '\n' ||
                           raw[length - 1U] == ' ' || raw[length - 1U] == '\t')) --length;
    while (begin < length && (raw[begin] == ' ' || raw[begin] == '\t')) ++begin;
    length -= begin;
    raw += begin;
    if (length == 0U) return;
    if (length >= sizeof(line)) length = sizeof(line) - 1U;
    for (size_t i = 0U; i < length; ++i)
        line[i] = (raw[i] >= 0x20U && raw[i] <= 0x7eU) ? (char)raw[i] : '?';
    line[length] = '\0';

    /* Ответы команд хранятся отдельно от периодических строк, чтобы связать их с конкретной командой */
    if (strcmp(line, "OK") == 0 || strcmp(line, "ERR") == 0) {
        if (client->status_count < MAX_STATUS_QUEUE) {
            size_t at = (client->status_head + client->status_count) % MAX_STATUS_QUEUE;
            (void)snprintf(client->statuses[at], sizeof(client->statuses[at]), "%s", line);
            ++client->status_count;
        } else {
            fprintf(stderr, "FAIL: status queue overflow while receiving %s\n", line);
        }
        return;
    }
    if (allowed_has(client, line)) {
        client->saw_clean_data = true;
        line_counter_t *entry = data_entry(client, line, true);
        if (entry != NULL) ++entry->count;
        return;
    }
    if (!client->saw_clean_data && is_timer_fragment(line)) {
        ++client->initial_fragments_ignored;
        return;
    }
    client->saw_clean_data = true;
    if (client->bad_count < MAX_BAD_LINES)
        (void)snprintf(client->bad[client->bad_count++], MAX_LINE, "%s", line);
    fprintf(stderr, "CORRUPT/UNEXPECTED OUTPUT LINE: %s\n", line);
}

/* Извлекаем из произвольных блоков чтения целые строки, учитывая LF, CR и CRLF */
static void parse_rx(uart_client_t *client)
{
    for (;;) {
        size_t split = client->rx_length;
        unsigned char delimiter = 0U;
        for (size_t i = 0U; i < client->rx_length; ++i) {
            if (client->rx[i] == '\r' || client->rx[i] == '\n') {
                split = i; delimiter = client->rx[i]; break;
            }
        }
        if (split == client->rx_length) return;
        classify_line(client, client->rx, split);
        size_t consumed = split + 1U;
        if (delimiter == '\r' && consumed < client->rx_length && client->rx[consumed] == '\n') ++consumed;
        memmove(client->rx, client->rx + consumed, client->rx_length - consumed);
        client->rx_length -= consumed;
    }
}

/* Ждём входящие байты до тайм-аута и передаём их общему анализатору строк */
static int pump(uart_client_t *client, double timeout)
{
    fd_set read_set;
    struct timeval tv;
    FD_ZERO(&read_set); FD_SET(client->fd, &read_set);
    if (timeout < 0.0) timeout = 0.0;
    tv.tv_sec = (time_t)timeout;
    tv.tv_usec = (suseconds_t)((timeout - (double)tv.tv_sec) * 1e6);
    int status = select(client->fd + 1, &read_set, NULL, NULL, &tv);
    if (status < 0) {
        if (errno == EINTR) return 0;
        fprintf(stderr, "FAIL: UART select: %s\n", strerror(errno));
        return -1;
    }
    if (status == 0) return 0;
    ssize_t got = read(client->fd, client->rx + client->rx_length,
                       sizeof(client->rx) - client->rx_length);
    if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        fprintf(stderr, "FAIL: UART read: %s\n", strerror(errno));
        return -1;
    }
    if (got == 0) return 0;
    client->rx_length += (size_t)got;
    parse_rx(client);
    if (client->rx_length == sizeof(client->rx)) {
        fprintf(stderr, "FAIL: UART line buffer overflow\n");
        client->rx_length = 0U;
        return -1;
    }
    return 0;
}

/* Отправляем команду полностью, повторяя частичные записи до дедлайна или явной ошибки */
static int write_all(uart_client_t *client, const unsigned char *bytes, size_t length)
{
    size_t offset = 0U;
    double deadline = monotonic_seconds() + client->status_timeout;
    while (offset < length) {
        fd_set write_set;
        double remaining = deadline - monotonic_seconds();
        struct timeval tv;
        if (remaining <= 0.0) {
            fprintf(stderr, "FAIL: timed out writing command to UART PTY\n");
            return -1;
        }
        tv.tv_sec = (time_t)remaining;
        tv.tv_usec = (suseconds_t)((remaining - (double)tv.tv_sec) * 1e6);
        FD_ZERO(&write_set); FD_SET(client->fd, &write_set);
        int selected = select(client->fd + 1, NULL, &write_set, NULL, &tv);
        if (selected < 0 && errno == EINTR) continue;
        if (selected <= 0) continue;
        ssize_t n = write(client->fd, bytes + offset, length - offset);
        if (n > 0) offset += (size_t)n;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        else return -1;
    }
    return 0;
}

/* Снимаем первый ожидающий OK/ERR из кольцевой очереди ответов */
static int command_status_pop(uart_client_t *client, char result[4])
{
    if (client->status_count == 0U) return 0;
    (void)snprintf(result, 4U, "%s", client->statuses[client->status_head]);
    client->status_head = (client->status_head + 1U) % MAX_STATUS_QUEUE;
    --client->status_count;
    return 1;
}

/* Выполняем ровно одну команду и сверяем статус, ожидаемый вывод и продолжение старых потоков */
static int client_command(uart_client_t *client, const char *command, const char *expected_status,
                          const char *expected_output, const char *const *keep_output,
                          size_t keep_count, const char *const *existing_output,
                          size_t existing_count)
{
    char status[4] = {0};
    unsigned long expected_baseline = expected_output != NULL ? data_count(client, expected_output) : 0UL;
    unsigned long baselines[MAX_ALLOWED_LINES];
    /* Старый ответ нельзя приписывать новой команде: очередь перед отправкой должна быть пустой */
    if (client->status_count > 0U) {
        fprintf(stderr, "FAIL: unsolicited/stale UART response before '%s'\n", command);
        return -1;
    }
    if (expected_output != NULL && allowed_add(client, expected_output) != 0) return -1;
    for (size_t i = 0U; i < keep_count; ++i)
        if (allowed_add(client, keep_output[i]) != 0) return -1;
    if (existing_count > MAX_ALLOWED_LINES) return -1;
    for (size_t i = 0U; i < existing_count; ++i)
        baselines[i] = data_count(client, existing_output[i]);

    ++client->commands_sent;
    printf("TX[%03lu]: %s\n", client->commands_sent, command);
    size_t length = strlen(command);
    unsigned char payload[MAX_LINE];
    if (length + 1U > sizeof(payload)) {
        fprintf(stderr, "FAIL: command too long\n"); return -1;
    }
    memcpy(payload, command, length); payload[length] = '\r';
    if (write_all(client, payload, length + 1U) != 0) return -1;

    /* Ожидаем первый ответ только до заданного дедлайна; ответы не сопоставляются по времени постфактум */
    double deadline = monotonic_seconds() + client->status_timeout;
    while (command_status_pop(client, status) == 0 && monotonic_seconds() < deadline) {
        if (pump(client, 0.05) != 0) return -1;
    }
    if (status[0] == '\0' || (strcmp(status, "OK") != 0 && strcmp(status, "ERR") != 0)) {
        fprintf(stderr, "FAIL: no OK/ERR response for '%s' within %.1f s\n", command, client->status_timeout);
        return -1;
    }
    printf("RX: %s\n", status);
    if (strcmp(status, expected_status) != 0) {
        fprintf(stderr, "FAIL: '%s': expected %s, got %s\n", command, expected_status, status);
        return -1;
    }
    if (strcmp(status, "OK") == 0) ++client->ok_count; else ++client->err_count;

    if (expected_output != NULL) {
        deadline = monotonic_seconds() + client->line_timeout;
        while (data_count(client, expected_output) <= expected_baseline && monotonic_seconds() < deadline) {
            if (client->status_count > 0U) { fprintf(stderr, "FAIL: extra status while waiting for output '%s'\n", expected_output); return -1; }
            if (pump(client, 0.05) != 0) return -1;
        }
        if (data_count(client, expected_output) <= expected_baseline) {
            fprintf(stderr, "FAIL: accepted '%s', but output '%s' was not observed\n", command, expected_output);
            return -1;
        }
        printf("  observed output: '%s'\n", expected_output);
    }
    if (existing_count > 0U) {
        deadline = monotonic_seconds() + client->line_timeout;
        bool found = false;
        while (monotonic_seconds() < deadline) {
            for (size_t i = 0U; i < existing_count; ++i)
                if (data_count(client, existing_output[i]) > baselines[i]) found = true;
            if (found) break;
            if (client->status_count > 0U) { fprintf(stderr, "FAIL: extra status while checking continued output\n"); return -1; }
            if (pump(client, 0.05) != 0) return -1;
        }
        if (!found) {
            fprintf(stderr, "FAIL: after '%s', no active output appeared: ", command);
            for (size_t i = 0U; i < existing_count; ++i) fprintf(stderr, "%s%s", i ? "," : "", existing_output[i]);
            fputc('\n', stderr); return -1;
        }
    }
    return 0;
}

/* Принимаем данные в течение заданного интервала между группами команд */
static int drain(uart_client_t *client, double seconds)
{
    double deadline = monotonic_seconds() + seconds;
    while (monotonic_seconds() < deadline)
        if (pump(client, 0.02) != 0) return -1;
    return 0;
}

/* Сводим статистику и печатаем PASS только при отсутствии неизвестных или повреждённых строк */
static void summary(const uart_client_t *client)
{
    unsigned long total = 0UL;
    size_t custom = 0U;
    puts("\n--- Stress summary ---");
    printf("Commands: %lu; OK: %lu; ERR: %lu\n", client->commands_sent, client->ok_count, client->err_count);
    for (size_t i = 0U; i < client->data_count; ++i) {
        total += client->data[i].count;
        if (strcmp(client->data[i].text, "ping") != 0 && strcmp(client->data[i].text, "pong") != 0)
            ++custom;
    }
    printf("Observed output lines: %lu\n", total);
    printf("ping=%lu pong=%lu\n", data_count((uart_client_t *)client, "ping"),
           data_count((uart_client_t *)client, "pong"));
    printf("Distinct custom lines observed: %zu\n", custom);
    if (client->initial_fragments_ignored)
        printf("Partial PTY-attach fragments ignored: %lu\n", client->initial_fragments_ignored);
    if (client->bad_count > 0U) {
        printf("FAIL: %zu corrupt/unexpected line(s)\n", client->bad_count);
        for (size_t i = 0U; i < client->bad_count && i < 20U; ++i)
            printf("  '%s'\n", client->bad[i]);
    } else {
        puts("PASS: no corrupted or unexpected output lines observed");
    }
}

/* Сценарий проверяет обновление пары периодов, отказ недопустимых параметров и изменение строк на ходу */
static int run_scenario(uart_client_t *client, unsigned repeat)
{
    static const char *const bases[] = {"ping", "pong"};
    static const char *const left_right[] = {"left", "right"};
    static const char *const active_long2[] = {"left", "ABCDEFGHIJ"};
    char max_text[] = MAX_TEXT;
    char command[128];
    char text1[32], text2[32];
    char active1[64] = "left", active2[64] = "right";
    unsigned long before_ping, before_pong;

    /* Сначала выбираем допустимую конфигурацию, кратную шагу Timer2=128 тактов. */
    if (client_command(client, "T=30080,30080", "OK", NULL, NULL, 0U, bases, 2U) != 0) return -1;
    if (client_command(client, "S1=left", "OK", "left", NULL, 0U, NULL, 0U) != 0) return -1;
    if (client_command(client, "S2=right", "OK", "right", NULL, 0U, NULL, 0U) != 0) return -1;
    if (client_command(client, "T=25023,25023", "ERR", NULL, NULL, 0U, left_right, 2U) != 0) return -1;
    if (client_command(client, "T=25088,25088", "OK", NULL, NULL, 0U, left_right, 2U) != 0) return -1;

    /* Timer1 может быть длиннее Timer2, но период Timer2 ограничен 32768 тактами. */
    if (client_command(client, "T=50176,25088", "OK", NULL, NULL, 0U, left_right, 2U) != 0) return -1;

    /* Максимальная строка синтаксически корректна, но не помещается в расписание Timer2. */
    (void)snprintf(command, sizeof(command), "S1=%s", max_text);
    if (client_command(client, command, "ERR", NULL, NULL, 0U, left_right, 2U) != 0) return -1;
    (void)snprintf(command, sizeof(command), "S2=%s", max_text);
    if (client_command(client, command, "ERR", NULL, NULL, 0U, left_right, 2U) != 0) return -1;

    /* Проверить допустимую более длинную строку в потоке 2 на максимальном периоде Timer2. */
    if (client_command(client, "T=32768,32768", "OK", NULL, NULL, 0U, left_right, 2U) != 0) return -1;
    if (client_command(client, "S2=ABCDEFGHIJ", "OK", "ABCDEFGHIJ", NULL, 0U, NULL, 0U) != 0) return -1;
    if (client_command(client, command, "ERR", NULL, NULL, 0U, active_long2, 2U) != 0) return -1;
    if (client_command(client, "S2=right", "OK", "right", NULL, 0U, NULL, 0U) != 0) return -1;

    for (unsigned i = 0U; i < repeat; ++i) {
        (void)snprintf(text1, sizeof(text1), "alpha%02u", i);
        (void)snprintf(text2, sizeof(text2), "bravo%02u", i);
        (void)snprintf(command, sizeof(command), "S1=%s", text1);
        if (client_command(client, command, "OK", text1, NULL, 0U, NULL, 0U) != 0) return -1;
        (void)snprintf(active1, sizeof(active1), "%s", text1);
        (void)snprintf(command, sizeof(command), "S2=%s", text2);
        if (client_command(client, command, "OK", text2, NULL, 0U, NULL, 0U) != 0) return -1;
        (void)snprintf(active2, sizeof(active2), "%s", text2);
        unsigned period = 32000U + (i & 1U) * 128U;
        (void)snprintf(command, sizeof(command), "T=%u,%u", period, period);
        const char *const active[] = {active1, active2};
        if (client_command(client, command, "OK", NULL, NULL, 0U, active, 2U) != 0) return -1;
    }

    if (client_command(client, "S1=ABCDEFGHIJKLMNOPQRSTUVW", "ERR", NULL, NULL, 0U,
                       (const char *const[]){active1, active2}, 2U) != 0) return -1;
    if (client_command(client, "X", "ERR", NULL, NULL, 0U,
                       (const char *const[]){active1, active2}, 2U) != 0) return -1;
    if (client_command(client, "S1=ping", "OK", "ping", NULL, 0U, NULL, 0U) != 0) return -1;
    if (client_command(client, "S2=pong", "OK", "pong", NULL, 0U, NULL, 0U) != 0) return -1;
    if (client_command(client, "T=23680,23680", "OK", NULL, NULL, 0U, bases, 2U) != 0) return -1;
    before_ping = data_count(client, "ping"); before_pong = data_count(client, "pong");
    if (client_command(client, "RESTART", "OK", NULL, NULL, 0U, bases, 2U) != 0) return -1;
    double deadline = monotonic_seconds() + client->line_timeout;
    while ((data_count(client, "ping") <= before_ping || data_count(client, "pong") <= before_pong) &&
           monotonic_seconds() < deadline) {
        if (client->status_count > 0U) { fprintf(stderr, "FAIL: unexpected status after RESTART\n"); return -1; }
        if (pump(client, 0.05) != 0) return -1;
    }
    if (data_count(client, "ping") <= before_ping || data_count(client, "pong") <= before_pong) {
        fprintf(stderr, "FAIL: both ping and pong were not observed after RESTART\n"); return -1;
    }
    return 0;
}

/* Печатаем параметры запуска стресс-клиента и значения по умолчанию */
static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s [--device PATH] [--repeat N] [--status-timeout SEC] [--line-timeout SEC] [--done-file PATH]\n", program);
}

/* Открываем PTY, выполняем сценарий, печатаем сводку и создаём файл завершения для трассировщика */
int main(int argc, char **argv)
{
    const char *device = "/tmp/simavr-uart0";
    const char *done_path = NULL;
    unsigned repeat = 20U;
    double status_timeout = 8.0, line_timeout = 5.0;
    uart_client_t client;
    bool opened = false;
    int result = EXIT_FAILURE;
    memset(&client, 0, sizeof(client));

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) device = argv[++i];
        else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) repeat = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--status-timeout") == 0 && i + 1 < argc) status_timeout = strtod(argv[++i], NULL);
        else if (strcmp(argv[i], "--line-timeout") == 0 && i + 1 < argc) line_timeout = strtod(argv[++i], NULL);
        else if (strcmp(argv[i], "--done-file") == 0 && i + 1 < argc) done_path = argv[++i];
        else { usage(argv[0]); return 2; }
    }
    if (repeat < 1U || repeat > 100U || status_timeout <= 0.0 || line_timeout <= 0.0) {
        usage(argv[0]); return 2;
    }
    /* Удаляем маркер прошлого запуска, иначе трассировщик может завершиться до отправки первой команды. */
    if (done_path != NULL && unlink(done_path) != 0 && errno != ENOENT) {
        fprintf(stderr, "FAIL: cannot remove old done file %s: %s\n", done_path, strerror(errno));
        return 2;
    }

    printf("Connecting to %s; stress rounds=%u\n", device, repeat);
    client.fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (client.fd < 0) {
        fprintf(stderr, "FAIL: cannot open UART PTY '%s': %s\n", device, strerror(errno));
        goto finished;
    }
    opened = true;
    client.status_timeout = status_timeout;
    client.line_timeout = line_timeout;
    struct termios old_settings, raw_settings;
    bool restore_termios = false;
    if (tcgetattr(client.fd, &old_settings) == 0) {
        raw_settings = old_settings;
        cfmakeraw(&raw_settings);
        if (tcsetattr(client.fd, TCSANOW, &raw_settings) == 0) restore_termios = true;
    }
    /* Исходные строки разрешены до первой смены текста, чтобы не принять старую передачу за повреждение */
    for (size_t i = 0U; i < BASE_LINE_COUNT; ++i) (void)allowed_add(&client, base_lines[i]);
    if (drain(&client, 0.15) != 0 || run_scenario(&client, repeat) != 0 || drain(&client, 0.1) != 0) {
        summary(&client);
        if (restore_termios) (void)tcsetattr(client.fd, TCSANOW, &old_settings);
        goto finished;
    }
    summary(&client);
    if (client.bad_count == 0U) {
        puts("PASS: command responses, rejection atomicity, live string changes and RESTART verified");
        result = EXIT_SUCCESS;
    }
    if (restore_termios) (void)tcsetattr(client.fd, TCSANOW, &old_settings);

finished:
    if (opened) close(client.fd);
    if (done_path != NULL) {
        FILE *marker = fopen(done_path, "w");
        if (marker == NULL)
            fprintf(stderr, "WARNING: cannot create done file %s: %s\n", done_path, strerror(errno));
        else fclose(marker);
    }
    return result;
}
