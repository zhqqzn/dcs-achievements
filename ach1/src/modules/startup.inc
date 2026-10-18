/*
 * Функциональный модуль: startup.inc
 * Назначение: Запуск приложения и начальная очистка программного состояния до разрешения прерываний.
 * Фрагмент включается в одну единицу трансляции firmware.S: общие метки SRAM
 * остаются видимыми без отдельной таблицы экспортируемых символов, а порядок
 * критичных инструкций и временные характеристики не меняются компоновкой.
 */
/* ------------------------------- Entry ----------------------------------- */
.section .text
.global main

main:
    cli /* Не разрешать таймерные события до завершения начальной проверки конфигурации. */
    clr r1 /* Обнулить регистр нулевого значения, используемый соглашением AVR и вычислениями. */
    rcall uart_init /* Настроить USART: U2X=1, формат 8N1, UBRR=16; разрешить TX и командный RX. */
    rcall state_init /* Обнулить индексы очереди, указатели передачи и флаги программного состояния. */
#if !TASK2_PERIODIC_ONLY
    /* Проверить стартовые константы тем же алгоритмом, что и команды; не запускать заведомо недопустимое расписание. */
    /* Передать начальную конфигурацию по регистровому контракту config_validate. */
    lds r24, active_t1
    lds r25, active_t1+1
    lds r22, active_t2
    lds r23, active_t2+1
    lds r21, active_str1_len
    lds r20, active_str2_len
    rcall config_validate /* Проверить расписание без промежуточных общих переменных. */
    tst r24 /* Проверить результат: ноль означает недопустимую начальную конфигурацию. */
    brne startup_config_valid /* Если конфигурация допустима, продолжить стандартную инициализацию. */
    ldi r30, lo8(msg_error) /* Подготовить адрес ответа ERR для случая некорректных стартовых констант. */
    ldi r31, hi8(msg_error) /* Загрузить старший байт адреса сообщения об ошибке. */
    rcall start_control_tx /* Вывести ERR через USART, не запуская некорректные таймеры. */
startup_invalid_config_wait:
    rcall service_tx_complete /* Следить за завершением сообщения об ошибке без использования таймерных прерываний. */
    rcall uart_tx_poll /* Передать очередной байт ERR, когда USART готов принять его. */
    rjmp startup_invalid_config_wait /* Остаться в безопасном состоянии: таймеры не запущены. */
startup_config_valid:
#endif
    rcall timer1_init /* Настроить Timer1 Compare A и одноразовый Compare B для запуска отдельного аппаратного Timer2. */
    sei /* Разрешить прерывания после инициализации обоих аппаратных таймеров. */

main_loop:
    rcall service_tx_complete /* вызвать подпрограмму service_tx_complete — завершить программное состояние передачи только после последнего байта и TXC. */
    rcall uart_tx_poll /* вызвать подпрограмму uart_tx_poll — подать следующий байт текущей строки, когда USART сообщает UDRE; ожидание не блокирует таймерные ISR. */
#if !TASK2_PERIODIC_ONLY
    rcall maybe_commit_restart /* вызвать подпрограмму maybe_commit_restart — применить принятую кандидатную конфигурацию в безопасном окне, не останавливая Timer1. */
    rcall uart_poll_rx /* вызвать подпрограмму uart_poll_rx — непрерывно опрашивать RXC; поступивший байт передать обработчику команд. */
    rcall handle_event_overflow /* вызвать подпрограмму handle_event_overflow — восстановиться после переполнения очереди и запросить ответ ERR, когда появится место. */
    rcall maybe_dispatch_status /* вызвать подпрограмму maybe_dispatch_status — передать один ответ OK/ERR только в зарезервированном временном окне. */
#endif
    rjmp main_loop /* безусловно передать управление на метку main_loop; выполнение продолжится с указанной метки. */

state_init:
    clr r16 /* обнулить регистр r16 перед его повторным использованием. */
    sts cmd_len, r16 /* сохранить cmd_len в SRAM (r16). */
    sts event_head, r16 /* сохранить event_head в SRAM (r16). */
    sts event_tail, r16 /* сохранить event_tail в SRAM (r16). */
    sts event_count, r16 /* сохранить event_count в SRAM (r16). */
    sts event_overflow, r16 /* сохранить event_overflow в SRAM (r16). */
    sts tx_ptr_lo, r16 /* сохранить tx_ptr_lo в SRAM (r16). */
    sts tx_ptr_hi, r16 /* сохранить tx_ptr_hi в SRAM (r16). */
    sts tx_kind, r16 /* сохранить tx_kind в SRAM (r16). */
    sts control_slot_open, r16 /* сохранить control_slot_open в SRAM (r16). */
    sts restart_pending, r16 /* сохранить restart_pending в SRAM (r16). */
    sts timer2_started, r16 /* сохранить timer2_started в SRAM (r16). */
    ret /* вернуться к инструкции после соответствующего RCALL. */
