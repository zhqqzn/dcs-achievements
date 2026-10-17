/*
 * Трассировщик записи байтов в UDR для проверки временной точности.
 *
 * Такты фиксируются через IRQ-подписку simavr, без точек останова GDB:
 * процессор продолжает выполнять прошивку обычным образом. В варианте задания
 * 2 приём USART отключён, поэтому наблюдаемые обращения UDR относятся к выводу.
 * Формат CSV:
 *   S,<cycle>,<value> — включение тактирования Timer1, начало отсчёта;
 *   U,<cycle>,<byte>  — наблюдаемая запись байта в UDR.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <simavr/sim_avr.h>
#include <simavr/sim_elf.h>
#include <simavr/sim_io.h>
#include <simavr/sim_irq.h>

#define CPU_HZ 16000000UL
/* Адреса регистров ATmega8 в пространстве данных: UDR=0x2c, TCCR1B=0x4e */
#define ATMEGA8_UDR_DATA_ADDR 0x2c
#define ATMEGA8_TCCR1B_DATA_ADDR 0x4e

typedef struct trace_context {
    avr_t *avr;
    FILE *out;
    int saw_start;
} trace_context_t;

typedef struct trace_tag {
    trace_context_t *ctx;
    char kind;
} trace_tag_t;

/* Обработчик IRQ записывает временные метки в CSV; значение UDR сохраняется без текстовых преобразований */
static void
trace_io_irq(struct avr_irq_t *irq, uint32_t value, void *param)
{
    (void)irq;
    trace_tag_t *tag = (trace_tag_t *)param;
    trace_context_t *ctx = tag->ctx;
    uint64_t cycle = (uint64_t)ctx->avr->cycle;

    if (tag->kind == 'S') {
        /* Фиксируем первую запись, включающую тактирование Timer1 (CS10=1). */
        if (!ctx->saw_start && (value & 0x07u) == 0x01u) {
            ctx->saw_start = 1;
            fprintf(ctx->out, "S,\"%" PRIu64 "\",%u\n", cycle, (unsigned)(value & 0xffu));
            fflush(ctx->out);
        }
    } else {
        fprintf(ctx->out, "U,\"%" PRIu64 "\",%u\n", cycle, (unsigned)(value & 0xffu));
    }
}

/* Загружаем ELF, подключаем наблюдателей к Timer1/UDR и выполняем модель заданное число тактов */
int
main(int argc, char **argv)
{
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "Usage: %s firmware.elf [cycle-limit] [trace.csv]\n", argv[0]);
        return 2;
    }

    char *end = NULL;
    uint64_t cycle_limit = 3000000;
    if (argc >= 3) {
        errno = 0;
        unsigned long long parsed = strtoull(argv[2], &end, 10);
        if (errno || end == argv[2] || *end != '\0' || parsed == 0) {
            fprintf(stderr, "Invalid cycle-limit: %s\n", argv[2]);
            return 2;
        }
        cycle_limit = (uint64_t)parsed;
    }
    const char *trace_path = argc >= 4 ? argv[3] : "build/udr_trace.csv";

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

    FILE *out = fopen(trace_path, "w");
    if (out == NULL) {
        fprintf(stderr, "Cannot open trace file %s: %s\n", trace_path, strerror(errno));
        avr_terminate(avr);
        return 1;
    }
    fputs("kind,cycle,value\n", out);

    /* Контекст и отдельные метки позволяют одному обработчику различать Timer1 и UDR */
    trace_context_t ctx = { .avr = avr, .out = out, .saw_start = 0 };
    trace_tag_t start_tag = { .ctx = &ctx, .kind = 'S' };
    trace_tag_t udr_tag = { .ctx = &ctx, .kind = 'U' };
    avr_irq_t *start_irq = avr_iomem_getirq(avr, ATMEGA8_TCCR1B_DATA_ADDR,
                                            "trace.TCCR1B", AVR_IOMEM_IRQ_ALL);
    avr_irq_t *udr_irq = avr_iomem_getirq(avr, ATMEGA8_UDR_DATA_ADDR,
                                          "trace.UDR", AVR_IOMEM_IRQ_ALL);
    if (start_irq == NULL || udr_irq == NULL) {
        fprintf(stderr, "Could not attach IOMEM trace IRQs\n");
        fclose(out);
        avr_terminate(avr);
        return 1;
    }
    /* IRQ-подписка не ставит CPU на паузу, в отличие от отладочной точки останова */
    avr_irq_register_notify(start_irq, trace_io_irq, &start_tag);
    avr_irq_register_notify(udr_irq, trace_io_irq, &udr_tag);

    fprintf(stderr, "Tracing %s for %" PRIu64 " simulated CPU cycles; CSV: %s\n",
            argv[1], cycle_limit, trace_path);
    /* Источник времени - счётчик цикла simavr */
    while ((uint64_t)avr->cycle < cycle_limit) {
        int state = avr_run(avr);
        if (state == cpu_Done || state == cpu_Crashed) {
            fprintf(stderr, "Simulation stopped early (state=%d, cycle=%" PRIu64 ")\n",
                    state, (uint64_t)avr->cycle);
            fclose(out);
            avr_terminate(avr);
            return 1;
        }
    }

    fclose(out);
    avr_terminate(avr);
    if (!ctx.saw_start) {
        fprintf(stderr, "No Timer1 start write was observed; trace is not analyzable\n");
        return 1;
    }
    fprintf(stderr, "Trace complete at cycle=%" PRIu64 "\n", (uint64_t)cycle_limit);
    return 0;
}
