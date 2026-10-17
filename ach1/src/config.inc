/*
 * Конфиг ATmega8.
 * При 16 МГц, U2X=1 и UBRR=16 фактическая скорость USART ок. 117647 бод.
 */
#ifndef TIMER1_INTERVAL
#define TIMER1_INTERVAL 24064
#endif
#ifndef TIMER2_INTERVAL
#define TIMER2_INTERVAL 24064
#endif
#ifndef TASK2_PERIODIC_ONLY
#define TASK2_PERIODIC_ONLY 0
#endif

#define UART_UBRR_VALUE 16
#define UART_BAUD_REQUESTED 115200
#define UART_U2X_VALUE 1

#define MAX_STR_LEN 24           /* Максимум передаваемых байтов вместе с CR/LF; завершающий NUL сюда не входит. */
#define MAX_TEXT_LEN 22          /* Максимум символов текста команды без завершающих CR/LF. */
#define COMMAND_MAX 31

#define EVENT_QUEUE_SIZE 8
#define EVENT_QUEUE_MASK 7
#define EV_NONE 0
#define EV_OK 3
#define EV_ERROR 4

/* Временные константы рассчитаны для 16 МГц, U2X=1 и UBRR=16. */
#define UART_FRAME_CYCLES 1360   /* Кадр 8N1 содержит 10 бит; каждый бит занимает 136 тактов CPU. */
#define STATUS_MAX_LEN 5        /* Длина ответа ERR с CR/LF. */
#define STATUS_TX_CYCLES 6800   /* Пять кадров по 1360 тактов на байт. */
#define OUTPUT_GUARD_CYCLES 32  /* Минимальный защитный промежуток между периодическими строками. */
#define CONTROL_SLOT_SETUP 512  /* Резерв на планирование и запуск ответа OK/ERR. */
#define CONTROL_CHECK_AHEAD 128 /* Минимальный запас до ближайшего compare перед отправкой ответа. */
#define COMMIT_CHECK_AHEAD 1024 /* Не изменять расписание ближе этого расстояния до следующего compare. */

/*
 * Компенсация пути от сравнения Timer1 до записи первого байта в UDR.
 * Значения 19/24 такта получены испытательным стендом для данной версии;
 * после изменения кода/оптимизации измерение необходимо повторить.
 */
#define COMPARE_EARLY_A 19
#define COMPARE_EARLY_PHASE 20 /* Timer1B: компенсация до сброса предделителя Timer2; требуется повторная трассировка после изменений ISR. */
#define TIMER2_PRESCALER 128
#define TIMER2_MAX_INTERVAL 32768 /* Timer2 CTC: (OCR2+1)*128, от 128 до 32768 тактов. */

#if TIMER1_INTERVAL < 1 || TIMER1_INTERVAL > 65535
#error "TIMER1_INTERVAL must fit the 16-bit Timer1 counter"
#endif
#if TIMER2_INTERVAL < 1 || TIMER2_INTERVAL > 65535
#error "TIMER2_INTERVAL must fit the 16-bit command/config representation"
#endif
