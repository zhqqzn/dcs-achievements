/*
 * Интерактивная консоль UART для виртуального порта simavr.
 * Читает команды из stdin, отправляет их с CR и выводит ответы прошивки.
 * Повторяющиеся периодические ping/pong скрываются, чтобы не засорять терминал.
 */

#define _DEFAULT_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "../src/protocol_contract.inc"

#define RX_CAPACITY 8192U
#define LINE_CAPACITY 256U
#define SEEN_CAPACITY 256U

/* Отбрасываем обрывки ping/pong, которые PTY мог отдать при подключении посреди строки */
static bool is_timer_fragment(const char *line)
{
    static const char *const messages[] = {UART_DEFAULT_STREAM1_TEXT, UART_DEFAULT_STREAM2_TEXT};
    size_t m;
    for (m = 0U; m < sizeof(messages) / sizeof(messages[0]); ++m) {
        size_t length = strlen(messages[m]);
        size_t split;
        for (split = 1U; split < length; ++split) {
            if (strcmp(line, messages[m] + split) == 0 ||
                (strlen(line) == split && strncmp(line, messages[m], split) == 0))
                return true;
        }
    }
    return false;
}

/* Распознаём последовательность периодических слов, в том числе склеенных соседних строк */
static bool is_periodic_traffic(const char *line)
{
    const char *cursor = line;
    if (*cursor == '\0')
        return true;
    while (*cursor != '\0') {
        if (strncmp(cursor, UART_DEFAULT_STREAM1_TEXT, sizeof(UART_DEFAULT_STREAM1_TEXT) - 1U) == 0)
            cursor += sizeof(UART_DEFAULT_STREAM1_TEXT) - 1U;
        else if (strncmp(cursor, UART_DEFAULT_STREAM2_TEXT, sizeof(UART_DEFAULT_STREAM2_TEXT) - 1U) == 0)
            cursor += sizeof(UART_DEFAULT_STREAM2_TEXT) - 1U;
        else
            return false;
    }
    return true;
}

/* Не даём одинаковым пользовательским строкам бесконечно повторяться в интерактивном выводе */
static bool line_was_seen(char seen[SEEN_CAPACITY][LINE_CAPACITY], size_t count,
                          const char *line)
{
    size_t i;
    for (i = 0U; i < count; ++i) {
        if (strcmp(seen[i], line) == 0)
            return true;
    }
    return false;
}

/* Нормализуем одну принятую строку и отделяем периодический трафик от ответов и данных команд */
static void show_line(const unsigned char *raw, size_t length,
                      bool *first_rx_line,
                      char seen[SEEN_CAPACITY][LINE_CAPACITY], size_t *seen_count,
                      bool periodic_summary, unsigned long *ping_count,
                      unsigned long *pong_count)
{
    char line[LINE_CAPACITY];
    size_t i;

    while (length > 0U && (raw[length - 1U] == UART_COMMAND_END_CR || raw[length - 1U] == UART_LINE_FEED ||
                           raw[length - 1U] == ' ' || raw[length - 1U] == '\t'))
        --length;
    while (length > 0U && (*raw == ' ' || *raw == '\t')) {
        ++raw;
        --length;
    }
    if (length >= sizeof(line))
        length = sizeof(line) - 1U;
    for (i = 0U; i < length; ++i)
        line[i] = (raw[i] >= 0x20U && raw[i] <= 0x7eU) ? (char)raw[i] : '?';
    line[length] = '\0';

    if (strcmp(line, UART_DEFAULT_STREAM1_TEXT) == 0) {
        ++*ping_count;
        *first_rx_line = false;
        if (!periodic_summary)
            return;
        return;
    }
    if (strcmp(line, UART_DEFAULT_STREAM2_TEXT) == 0) {
        ++*pong_count;
        *first_rx_line = false;
        if (!periodic_summary)
            return;
        return;
    }
    if (is_periodic_traffic(line)) {
        *first_rx_line = false;
        return;
    }
    if (*first_rx_line && is_timer_fragment(line)) {
        *first_rx_line = false;
        return;
    }
    *first_rx_line = false;

    /* Ответы протокола показываем всегда, пользовательские строки — только один раз */
    if (strcmp(line, UART_STATUS_OK_TEXT) != 0 && strcmp(line, UART_STATUS_ERR_TEXT) != 0) {
        if (line_was_seen(seen, *seen_count, line))
            return;
        if (*seen_count < SEEN_CAPACITY) {
            (void)snprintf(seen[*seen_count], LINE_CAPACITY, "%s", line);
            ++*seen_count;
        }
    }
    (void)printf("\nRX: %s\ncmd> ", line);
    (void)fflush(stdout);
}

/* Передаём всю команду: write() может принять только часть данных или временно вернуть EAGAIN */
static int write_all(int fd, const unsigned char *data, size_t length)
{
    size_t offset = 0U;
    while (offset < length) {
        ssize_t written = write(fd, data + offset, length - offset);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set write_set;
            struct timeval timeout = {2, 0};
            FD_ZERO(&write_set);
            FD_SET(fd, &write_set);
            if (select(fd + 1, NULL, &write_set, NULL, &timeout) <= 0)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

/* Печатаем краткую форму запуска консоли и её необязательных параметров */
static void print_usage(const char *program)
{
    (void)fprintf(stderr, "Usage: %s [--periodic-summary] [device]\n", program);
}

/* Открываем PTY, параллельно обслуживаем клавиатуру и UART и восстанавливаем режим терминала при выходе */
int main(int argc, char **argv)
{
    const char *device = "/tmp/simavr-uart0";
    bool periodic_summary = false;
    int fd;
    struct termios old_settings;
    struct termios raw_settings;
    bool restore_termios = false;
    unsigned char rx[RX_CAPACITY];
    size_t rx_length = 0U;
    unsigned char input[LINE_CAPACITY];
    size_t input_length = 0U;
    char seen[SEEN_CAPACITY][LINE_CAPACITY] = {{0}};
    size_t seen_count = 0U;
    bool first_rx_line = true;
    unsigned long ping_count = 0UL;
    unsigned long pong_count = 0UL;
    unsigned long last_ping = 0UL;
    unsigned long last_pong = 0UL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--periodic-summary") == 0) {
            periodic_summary = true;
        } else if (argv[i][0] == '-') {
            print_usage(argv[0]);
            return 2;
        } else {
            device = argv[i];
        }
    }

    /* Неблокирующий режим нужен, чтобы ожидание UART не блокировало ввод команд с клавиатуры */
    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        (void)fprintf(stderr, "Cannot open UART PTY '%s': %s\n", device, strerror(errno));
        (void)fprintf(stderr, "Start simavr first and check its printed UART/PTY path.\n");
        return 2;
    }
    /* Убираем канонический ввод и преобразования терминала; исходные настройки сохраняем для восстановления */
    if (tcgetattr(fd, &old_settings) == 0) {
        raw_settings = old_settings;
        cfmakeraw(&raw_settings);
        if (tcsetattr(fd, TCSANOW, &raw_settings) == 0)
            restore_termios = true;
    }

    (void)puts("Connected to ATmega8 USART. Type a command and press Enter.");
    (void)puts("Examples: T=30080,30080, S1=hello, S2=world, RESTART. Timer2 periods must be multiples of 128 cycles (128..32768). Ctrl+C exits.");
    (void)puts("T1/T2 values are CPU cycles at 16 MHz (16000 cycles = 1 ms). Wait for OK/ERR before the next command.");
    if (periodic_summary)
        (void)puts("Periodic summary enabled: ping/pong counts are reported once per second.");
    (void)printf("cmd> ");
    (void)fflush(stdout);

    for (;;) {
        fd_set read_set;
        struct timeval timeout = {1, 0};
        int max_fd = fd > STDIN_FILENO ? fd : STDIN_FILENO;
        FD_ZERO(&read_set);
        FD_SET(fd, &read_set);
        FD_SET(STDIN_FILENO, &read_set);
        int selected = select(max_fd + 1, &read_set, NULL, NULL, &timeout);
        if (selected < 0) {
            if (errno == EINTR)
                continue;
            (void)fprintf(stderr, "select failed: %s\n", strerror(errno));
            break;
        }
        if (selected == 0) {
            if (periodic_summary) {
                (void)printf("\nPERIODIC (1.0s): ping=%lu pong=%lu\ncmd> ",
                             ping_count - last_ping, pong_count - last_pong);
                (void)fflush(stdout);
                last_ping = ping_count;
                last_pong = pong_count;
            }
            continue;
        }

        /* UART может вернуть фрагмент строки, несколько строк сразу или CR и LF отдельными чтениями */
        if (FD_ISSET(fd, &read_set)) {
            ssize_t got = read(fd, rx + rx_length, sizeof(rx) - rx_length);
            if (got > 0) {
                rx_length += (size_t)got;
                for (;;) {
                    size_t split = rx_length;
                    size_t i;
                    unsigned char delim = 0U;
                    for (i = 0U; i < rx_length; ++i) {
                        if (rx[i] == UART_COMMAND_END_CR || rx[i] == UART_LINE_FEED) {
                            split = i;
                            delim = rx[i];
                            break;
                        }
                    }
                    if (split == rx_length)
                        break;
                    show_line(rx, split, &first_rx_line, seen, &seen_count,
                              periodic_summary, &ping_count, &pong_count);
                    size_t consumed = split + 1U;
                    if (delim == UART_COMMAND_END_CR && consumed < rx_length && rx[consumed] == UART_LINE_FEED)
                        ++consumed;
                    memmove(rx, rx + consumed, rx_length - consumed);
                    rx_length -= consumed;
                }
                if (rx_length == sizeof(rx))
                    rx_length = 0U;
            } else if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                (void)fprintf(stderr, "UART read failed: %s\n", strerror(errno));
                break;
            }
        }

        /* Команда собирается до Enter; в UART отправляется с CR, ожидаемым прошивкой */
        if (FD_ISSET(STDIN_FILENO, &read_set)) {
            unsigned char inbuf[LINE_CAPACITY];
            ssize_t got = read(STDIN_FILENO, inbuf, sizeof(inbuf));
            if (got == 0)
                break;
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                (void)fprintf(stderr, "stdin read failed: %s\n", strerror(errno));
                break;
            }
            for (ssize_t j = 0; j < got; ++j) {
                unsigned char ch = inbuf[j];
                if (ch == UART_COMMAND_END_CR || ch == UART_LINE_FEED) {
                    if (input_length > 0U) {
                        unsigned char command[LINE_CAPACITY + 1U];
                        memcpy(command, input, input_length);
                        command[input_length] = UART_COMMAND_END_CR;
                        if (write_all(fd, command, input_length + 1U) != 0) {
                            (void)fprintf(stderr, "UART write failed: %s\n", strerror(errno));
                            if (restore_termios)
                                (void)tcsetattr(fd, TCSANOW, &old_settings);
                            (void)close(fd);
                            return 2;
                        }
                        input_length = 0U;
                    }
                    (void)printf("cmd> ");
                    (void)fflush(stdout);
                } else if (ch == 0x7f || ch == '\b') {
                    if (input_length > 0U)
                        --input_length;
                } else if (ch >= 0x20U && ch <= 0x7eU && input_length + 1U < sizeof(input)) {
                    input[input_length++] = ch;
                }
            }
        }
    }

    if (restore_termios)
        (void)tcsetattr(fd, TCSANOW, &old_settings);
    (void)close(fd);
    return 0;
}
