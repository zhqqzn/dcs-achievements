#include "protocol_contract.inc"

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

#define MAX_STR_LEN UART_OUTPUT_LINE_MAX_BYTES /* Передаваемые байты с CR/LF, без завершающего NUL. */
#define MAX_TEXT_LEN UART_USER_TEXT_MAX_BYTES /* Максимум символов пользовательского текста. */
#define COMMAND_MAX UART_COMMAND_MAX_BYTES

#define EVENT_QUEUE_SIZE 8
#define EVENT_QUEUE_MASK 7
#define EV_NONE 0
#define EV_OK 3
#define EV_ERROR 4

/* Временные константы рассчитаны для 16 МГц, U2X=1 и UBRR=16. */
#define STATUS_MAX_LEN UART_STATUS_ERR_FRAME_BYTES /* Длина ответа ERR с CR/LF. */
#define STATUS_TX_CYCLES UART_STATUS_TX_CYCLES /* Передача максимального ответа ERR с CR/LF. */
#define OUTPUT_GUARD_CYCLES UART_OUTPUT_GUARD_CYCLES /* Минимальный защитный промежуток между периодическими строками. */
#define CONTROL_SLOT_SETUP UART_CONTROL_SLOT_SETUP_CYCLES /* Резерв на планирование и запуск ответа OK/ERR. */
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
