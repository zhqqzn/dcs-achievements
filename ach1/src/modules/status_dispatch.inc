/*
 * Функциональный модуль: status_dispatch.inc
 * Назначение: Отправка OK/ERR только в предусмотренном расписанием окне; до запуска передачи состояние перепроверяется атомарно.
 * Фрагмент включается в одну единицу трансляции firmware.S: общие метки SRAM
 * остаются видимыми без отдельной таблицы экспортируемых символов, а порядок
 * критичных инструкций и временные характеристики не меняются компоновкой.
 */
/* Отправлять ответ на команду только в зарезервированном промежутке после первой строки. */
maybe_dispatch_status:
    lds r18, restart_pending /* прочитать из SRAM флаг ожидающего перезапуска в r18. */
    tst r18 /* проверить r18 без изменения самого регистра; результат задаёт флаги Z/N. */
    brne maybe_dispatch_return /* если Z=0 (результат не равен нулю): перейти к maybe_dispatch_return. */
    lds r18, tx_kind /* прочитать из SRAM состояние передачи в r18. */
    tst r18 /* проверить r18 без изменения самого регистра; результат задаёт флаги Z/N. */
    brne maybe_dispatch_return /* если Z=0 (результат не равен нулю): перейти к maybe_dispatch_return. */
    lds r18, control_slot_open /* прочитать из SRAM флаг свободного окна ответа в r18. */
    tst r18 /* проверить r18 без изменения самого регистра; результат задаёт флаги Z/N. */
    breq maybe_dispatch_return /* если Z=1 (результат равен нулю): перейти к maybe_dispatch_return. */
    lds r18, event_count /* прочитать из SRAM счётчик очереди в r18. */
    tst r18 /* проверить r18 без изменения самого регистра; результат задаёт флаги Z/N. */
    breq maybe_dispatch_return /* если Z=1 (результат равен нулю): перейти к maybe_dispatch_return. */
    rcall get_next_compare_delta /* вызвать подпрограмму get_next_compare_delta — атомарно получить расстояние до ближайшего следующего события сравнения. */
    /* Ответ должен передаться до следующего compare, сдвинутого раньше. */
    cpi r25, hi8(STATUS_TX_CYCLES + CONTROL_CHECK_AHEAD) /* сравнить r25 с константу hi8(STATUS_TX_CYCLES + CONTROL_CHECK_AHEAD), не сохраняя результат вычитания; далее проверяются флаги. */
    brlo maybe_dispatch_slot_missed /* если C=1 (меньше без знака): перейти к maybe_dispatch_slot_missed. */
    brne maybe_dispatch_do /* если Z=0 (результат не равен нулю): перейти к maybe_dispatch_do. */
    cpi r24, lo8(STATUS_TX_CYCLES + CONTROL_CHECK_AHEAD) /* сравнить r24 с константу lo8(STATUS_TX_CYCLES + CONTROL_CHECK_AHEAD), не сохраняя результат вычитания; далее проверяются флаги. */
    brlo maybe_dispatch_slot_missed /* если C=1 (меньше без знака): перейти к maybe_dispatch_slot_missed. */

maybe_dispatch_do:
    /* Выше уже проверено, что ближайшее сравнение таймера находится дальше
       чем STATUS_TX_CYCLES + CONTROL_CHECK_AHEAD. Поэтому перед запуском ответа
       здесь достаточно повторно проверить состояние без CLI: короткое глобальное
       маскирование прерываний могло бы задержать Compare Match Timer2 на 5–6 тактов. */
    lds r18, restart_pending /* Проверить, не ожидается ли применение новой конфигурации. */
    tst r18
    brne maybe_dispatch_return
    lds r18, tx_kind /* Не начинать статус, если передатчик уже занят строкой таймера. */
    tst r18
    brne maybe_dispatch_return
    lds r18, control_slot_open /* Убедиться, что окно ответа ещё открыто. */
    tst r18
    breq maybe_dispatch_return
    lds r18, event_count /* Не извлекать событие, если очередь успела опустеть. */
    tst r18
    breq maybe_dispatch_return
    /* При достаточном запасе до следующего события никакой таймерный ISR не может
       вмешаться в этот короткий участок, поэтому ответ запускается без CLI. */
    rcall event_dequeue
    rcall dispatch_status
    ret

maybe_dispatch_slot_missed:
    /* Ответ больше не помещается. Закрыть окно, чтобы прекратить короткие повторные секции CLI
       рядом со следующим compare; ответ в очереди дождётся следующего окна. */
    clr r18 /* обнулить регистр r18 перед его повторным использованием. */
    sts control_slot_open, r18 /* сохранить control_slot_open в SRAM (r18). */
maybe_dispatch_return:
    ret /* вернуться к инструкции после соответствующего RCALL. */

dispatch_status:
    cpi r24, EV_OK /* сравнить r24 с константу EV_OK, не сохраняя результат вычитания; далее проверяются флаги. */
    breq dispatch_status_ok /* если Z=1 (результат равен нулю): перейти к dispatch_status_ok. */
    ldi r30, lo8(msg_error) /* загрузить константу lo8(msg_error) в r30; следующая инструкция использует это значение. */
    ldi r31, hi8(msg_error) /* загрузить константу hi8(msg_error) в r31; следующая инструкция использует это значение. */
    rjmp start_control_tx /* безусловно передать управление на метку start_control_tx; выполнение продолжится с указанной метки. */
dispatch_status_ok:
    ldi r30, lo8(msg_ok) /* загрузить константу lo8(msg_ok) в r30; следующая инструкция использует это значение. */
    ldi r31, hi8(msg_ok) /* загрузить константу hi8(msg_ok) в r31; следующая инструкция использует это значение. */

start_control_tx:
    ld r24, Z+ /* Прочитать байт из SRAM по адресу Z в r24 и увеличить указатель на 1. */
    out _SFR_IO_ADDR(UDR), r24 /* записать r24 в регистр данных USART (UDR). */
#if TRACE_SCHEDULE
    /* Только для трассировки: отметить исходящие записи, отличая их от чтения UDR при приёме. */
    out _SFR_IO_ADDR(PORTB), r24 /* записать r24 в диагностический порт зеркалирования TX (PORTB). */
#endif
    ldi r24, (1<<TXC)|(1<<U2X) /* подготовить битовую маску настройки в r24; следующая инструкция использует это значение. */
    out _SFR_IO_ADDR(UCSRA), r24 /* записать r24 в статус/управление USART (UCSRA). */
    sts tx_ptr_lo, r30 /* сохранить tx_ptr_lo в SRAM (r30). */
    sts tx_ptr_hi, r31 /* сохранить tx_ptr_hi в SRAM (r31). */
    ldi r24, 3 /* загрузить константу 3 в r24; следующая инструкция использует это значение. */
    sts tx_kind, r24 /* сохранить tx_kind в SRAM (r24). */
    clr r24 /* обнулить регистр r24 перед его повторным использованием. */
    sts control_slot_open, r24 /* сохранить control_slot_open в SRAM (r24). */
    ret /* вернуться к инструкции после соответствующего RCALL. */
