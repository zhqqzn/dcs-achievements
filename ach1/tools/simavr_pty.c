/*
 * Лончер simavr, который подключает USART0 модели ATmega8 к PTY.
 * Для сборки нужны файлы разработки libsimavr-dev и метаданные simavr/simavrparts для pkg-config.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <simavr/sim_avr.h>
#include <simavr/sim_gdb.h>
#include <simavr/sim_elf.h>
#include <simavr/parts/uart_pty.h>

/* Загружаем ELF в модель ATmega8, подключаем USART к PTY и запускаем выполнение до остановки симулятора */
int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s firmware.elf\n", argv[0]);
        return 2;
    }

    /* ELF содержит прошивку и метаданные; здесь файл ещё не исполняется */
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

    /* Частота задаётся явно и для прошивки, и для ядра simavr: обе стороны используют 16 МГц */
    firmware.frequency = 16000000UL;
    avr_init(avr);
    avr->frequency = 16000000UL;
    avr_load_firmware(avr, &firmware);

    /* PTY предоставляет обычный файл-порт, с которым могут работать терминал и стресс-клиент */
    uart_pty_t uart_pty;
    uart_pty_init(avr, &uart_pty);
    uart_pty_connect(&uart_pty, '0');

    /* GDB включается только по явному запросу; остановки отладчика непригодны для измерения тактов */
    const char *gdb_env = getenv("SIMAVR_GDB");
    if (gdb_env != NULL && gdb_env[0] != '\0' && strcmp(gdb_env, "0") != 0) {
        avr->gdb_port = 1234;
        avr->state = cpu_Stopped;
        avr_gdb_init(avr);
        puts("GDB server is listening on :1234; target starts stopped until GDB connects.");
    }

    puts("ATmega8 is running. UART0 PTY: /tmp/simavr-uart0");
    puts("Connect in another terminal with: ./build/console");
    fflush(stdout);

    for (;;) {
        int state = avr_run(avr);
        if (state == cpu_Done || state == cpu_Crashed) {
            fprintf(stderr, "Simulation stopped (state=%d).\n", state);
            avr_terminate(avr);
            return state == cpu_Crashed ? 1 : 0;
        }
    }
}
