/*
 * Лончер simavr с PTY и трассировкой командной нагрузки.
 *
 * Наблюдатели подключаются через avr_register_io_write после штатных обработчиков
 * периферии. Так фиксируются именно записи CPU в UDR и PORTC без подмены поведения
 * USART и GPIO. PORTC используется диагностической прошивкой для передачи снимков
 * расписания: это служебный канал трассировки, а не интерфейс пользовательской платы.
 *
 * Вызов: simavr-stress-trace firmware.elf cycle-limit trace.csv done-file.
 * Симуляция заканчивается по пределу тактов или при появлении файла-маркера. Выполнение
 * периодически замедляется до скорости около 16 МГц по фактическому времени, чтобы клиент
 * из другого терминала успевал передавать команды через PTY.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <simavr/sim_avr.h>
#include <simavr/sim_elf.h>
#include <simavr/sim_io.h>
#include <simavr/sim_irq.h>
#include <simavr/parts/uart_pty.h>

#define CPU_HZ 16000000UL
/* Адреса ATmega8 в пространстве данных: UDR=0x2c, TCNT1L/H=0x4c/0x4d,
   TCCR1B=0x4e, PORTC=0x35. */
#define ATMEGA8_UDR_DATA_ADDR 0x2c
#define ATMEGA8_TCNT1L_DATA_ADDR 0x4c
#define ATMEGA8_TCNT1H_DATA_ADDR 0x4d
#define ATMEGA8_TCCR1B_DATA_ADDR 0x4e
#define ATMEGA8_PORTC_DATA_ADDR 0x35
#define TRACE_MARKER 0xa5u
#define TRACE_FRAME_FIELDS 8u
#define TRACE_MAX_LINE 24u

struct trace_context {
    avr_t *avr;
    FILE *out;
    int saw_start;
    uint64_t start_cycle;
    int marker_active;
    unsigned fields_seen;
    uint8_t fields[TRACE_FRAME_FIELDS];
    unsigned payload_left;
    unsigned trace_errors;
};

typedef struct trace_tag {
    struct trace_context *ctx;
    char kind;
} trace_tag_t;

/* Единая точка записи CSV: тип события и номер такта сохраняются вместе со значением */
static void
write_record(struct trace_context *ctx, char kind, uint64_t cycle, unsigned value)
{
    fprintf(ctx->out, "%c,\"%" PRIu64 "\",%u\n", kind, cycle, value);
}

/* Собираем кадр метаданных PORTC и проверяем границы. Так произвольные записи GPIO не принимаются за расписание */
static void
trace_portc_byte(struct trace_context *ctx, uint64_t cycle, uint8_t byte)
{
    /* A5 начинает кадр метаданных; обычная запись PORTC не считается новым снимком */
    if (!ctx->marker_active && byte == TRACE_MARKER) {
        /* Timer1 считает непрерывно с делителем 1 от записанного момента запуска.
           Вычисляем значение по общему счётчику циклов, а не по зеркалу data[], которое может отставать */
        uint16_t live_count = (uint16_t)(cycle - ctx->start_cycle);
        write_record(ctx, 'E', cycle, live_count);
        write_record(ctx, 'P', cycle, byte);
        ctx->marker_active = 1;
        ctx->fields_seen = 0;
        ctx->payload_left = 0;
        return;
    }
    if (!ctx->marker_active) {
        fprintf(stderr, "Unexpected unframed PORTC trace write: 0x%02x at cycle=%" PRIu64 "\n",
                byte, cycle);
        ctx->trace_errors++;
        return;
    }
    write_record(ctx, 'P', cycle, byte);
    if (ctx->fields_seen < TRACE_FRAME_FIELDS) {
        ctx->fields[ctx->fields_seen++] = byte;
        if (ctx->fields_seen == TRACE_FRAME_FIELDS) {
            unsigned len1 = ctx->fields[6];
            unsigned len2 = ctx->fields[7];
            if (len1 > TRACE_MAX_LINE || len2 > TRACE_MAX_LINE || len1 < 2 || len2 < 2) {
                fprintf(stderr, "Invalid schedule marker string lengths: %u, %u\n", len1, len2);
                ctx->marker_active = 0;
                ctx->trace_errors++;
                return;
            }
            /* После восьми полей идут ровно байты обеих активных строк, включая CR/LF */
            ctx->payload_left = len1 + len2;
            if (ctx->payload_left == 0)
                ctx->marker_active = 0;
        }
    } else {
        if (ctx->payload_left > 0)
            ctx->payload_left--;
        if (ctx->payload_left == 0)
            ctx->marker_active = 0;
    }
}

/* Этот обработчик включается в цепочку вызовов периферии simavr. В отличие от
 * общего IOMEM IRQ, он видит каждую запись CPU, даже если новое значение совпадает со старым */
static void
trace_register_write(avr_t *avr, avr_io_addr_t addr, uint8_t value, void *param)
{
    (void)addr;
    trace_tag_t *tag = (trace_tag_t *)param;
    struct trace_context *ctx = tag->ctx;
    uint64_t cycle = (uint64_t)avr->cycle;

    if (tag->kind == 'U') {
        /* UDR читается при приёме и записывается при передаче. Обработчик записи
           фиксирует только запись CPU, то есть нужное нам событие OUT UDR */
        write_record(ctx, 'U', cycle, value);
    } else if (tag->kind == 'P') {
        trace_portc_byte(ctx, cycle, value);
    }
}

/* По IRQ регистрируем только старт Timer1; байты UDR фиксируются отдельным обратным вызовом записи */
static void
trace_io_irq(struct avr_irq_t *irq, uint32_t value, void *param)
{
    (void)irq;
    trace_tag_t *tag = (trace_tag_t *)param;
    struct trace_context *ctx = tag->ctx;
    uint64_t cycle = (uint64_t)ctx->avr->cycle;

    if (tag->kind == 'S' && !ctx->saw_start && (value & 0x07u) == 0x01u) {
        ctx->saw_start = 1;
        ctx->start_cycle = cycle;
        write_record(ctx, 'S', cycle, value);
    }
}

/* Проверяем внешний маркер завершения стресс-клиента; отсутствие файла означает продолжение трассировки */
static int
file_exists(const char *path)
{
    return path != NULL && access(path, F_OK) == 0;
}

/* Даём PTY-клиенту реальное время на обмен, не меняя число выполняемых тактов симулятора */
static void
sleep_10ms(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

/* Запускаем диагностическую прошивку, записываем UART/метаданные и завершаем файл после клиента или лимита */
int
main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "Usage: %s firmware.elf cycle-limit trace.csv done-file\n", argv[0]);
        return 2;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(argv[2], &end, 10);
    if (errno || end == argv[2] || *end != '\0' || parsed == 0) {
        fprintf(stderr, "Invalid cycle-limit: %s\n", argv[2]);
        return 2;
    }
    uint64_t cycle_limit = (uint64_t)parsed;
    const char *trace_path = argv[3];
    const char *done_path = argv[4];

    elf_firmware_t firmware = {0};
    if (elf_read_firmware(argv[1], &firmware) != 0) {
        fprintf(stderr, "Cannot read firmware ELF: %s\n", argv[1]);
        return 1;
    }
    avr_t *avr = avr_make_mcu_by_name("atmega8");
    if (avr == NULL) {
        fprintf(stderr, "simavr does not recognize MCU atmega8\n");
        return 1;
    }
    firmware.frequency = CPU_HZ;
    avr_init(avr);
    avr->frequency = CPU_HZ;
    avr_load_firmware(avr, &firmware);

    uart_pty_t uart_pty;
    uart_pty_init(avr, &uart_pty);
    uart_pty_connect(&uart_pty, '0');

    FILE *out = fopen(trace_path, "w");
    if (out == NULL) {
        fprintf(stderr, "Cannot open trace file %s: %s\n", trace_path, strerror(errno));
        avr_terminate(avr);
        return 1;
    }
    fputs("kind,cycle,value\n", out);

    struct trace_context ctx = { .avr = avr, .out = out };
    trace_tag_t start_tag = { .ctx = &ctx, .kind = 'S' };
    trace_tag_t udr_tag = { .ctx = &ctx, .kind = 'U' };
    trace_tag_t portc_tag = { .ctx = &ctx, .kind = 'P' };
    avr_irq_t *start_irq = avr_iomem_getirq(avr, ATMEGA8_TCCR1B_DATA_ADDR,
                                            "trace.TCCR1B", AVR_IOMEM_IRQ_ALL);
    if (start_irq == NULL) {
        fprintf(stderr, "Could not attach Timer1 start trace IRQ\n");
        fclose(out);
        avr_terminate(avr);
        return 1;
    }
    avr_irq_register_notify(start_irq, trace_io_irq, &start_tag);
    /* Наблюдатели подключаются после обработчиков периферии: так фиксируются
       фактические записи CPU, а не отфильтрованный поток общих IRQ-регистров */
    avr_register_io_write(avr, ATMEGA8_UDR_DATA_ADDR, trace_register_write, &udr_tag);
    avr_register_io_write(avr, ATMEGA8_PORTC_DATA_ADDR, trace_register_write, &portc_tag);

    printf("ATmega8 running with UART0 PTY /tmp/simavr-uart0.\n");
    printf("Trace: %s; stop marker: %s; max cycles: %" PRIu64 ".\n",
           trace_path, done_path, cycle_limit);
    printf("Start ./build/stress-commands in another terminal now\n");
    fflush(stdout);

    /* 160000 тактов соответствуют 10 мс при выбранной частоте 16 МГц */
    const uint64_t pacing_slice = 160000;
    uint64_t next_pace = pacing_slice;
    uint64_t next_done_check = pacing_slice;
    int stopped_by_marker = 0;
    /* Между короткими порциями проверяем файл-маркер и даём внешнему клиенту время на ввод */
    while ((uint64_t)avr->cycle < cycle_limit) {
        int state = avr_run(avr);
        if (state == cpu_Done || state == cpu_Crashed) {
            /* Сбрасываем дамп состояния процессора и периферии перед закрытием симулятора.
            Код 7 в simavr - сбой процессора (cpu_Crashed). 
            По этому поймем, сработал сторожевой таймер или код улетел не туда */
            uint16_t sp = (uint16_t)avr->data[0x5d] |
                          ((uint16_t)avr->data[0x5e] << 8);
            uint16_t tcnt1 = (uint16_t)avr->data[ATMEGA8_TCNT1L_DATA_ADDR] |
                             ((uint16_t)avr->data[ATMEGA8_TCNT1H_DATA_ADDR] << 8);
            fprintf(stderr,
                    "Simulation stopped early (state=%d, cycle=%" PRIu64
                    ", PC=0x%04x, SP=0x%04x, SREG=0x%02x, WDTCR=0x%02x, "
                    "TCCR1B=0x%02x, TCNT1=0x%04x, TCCR2=0x%02x, TCNT2=0x%02x, "
                    "OCR2=0x%02x, TIMSK=0x%02x, TIFR=0x%02x)\n",
                    state, (uint64_t)avr->cycle, (unsigned)avr->pc, (unsigned)sp,
                    (unsigned)avr->data[0x5f], (unsigned)avr->data[0x41],
                    (unsigned)avr->data[ATMEGA8_TCCR1B_DATA_ADDR], (unsigned)tcnt1,
                    (unsigned)avr->data[0x45], (unsigned)avr->data[0x44],
                    (unsigned)avr->data[0x43], (unsigned)avr->data[0x59],
                    (unsigned)avr->data[0x58]);
            fclose(out);
            avr_terminate(avr);
            return 1;
        }
        /* файл-маркер проверяется по границам порций, чтобы не выполнять файловый вызов на каждом такте */
        if ((uint64_t)avr->cycle >= next_done_check) {
            if (file_exists(done_path)) {
                stopped_by_marker = 1;
                break;
            }
            next_done_check += pacing_slice;
        }
        if ((uint64_t)avr->cycle >= next_pace) {
            sleep_10ms();
            next_pace += pacing_slice;
        }
    }
    uint64_t final_cycle = (uint64_t)avr->cycle;
    fclose(out);
    avr_terminate(avr);
    if (!ctx.saw_start) {
        fprintf(stderr, "No Timer1 start event was observed\n");
        return 1;
    }
    if (ctx.trace_errors != 0) {
        fprintf(stderr, "Trace callback errors: %u\n", ctx.trace_errors);
        return 1;
    }
    fprintf(stderr, "Trace complete at cycle=%" PRIu64 "%s\n", final_cycle,
            stopped_by_marker ? " (stress client finished)" : " (cycle limit reached)");
    return 0;
}
