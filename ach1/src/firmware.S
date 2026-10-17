/*
 * Учебная прошивка ATmega8 на GNU AVR Assembly.
 */
#include <avr/io.h>
#include "config.inc"

/* Макросы условных переходов подключаются до кода, который их использует. */
#include "modules/branch_macros.inc"

/* Эти startup-маркеры заставляют AVR-LibC скопировать .data и очистить .bss. */
.global __do_copy_data
.global __do_clear_bss

/* Сначала размещаются переменные, затем код старта и критические ISR. */
#include "modules/state_data.inc"
#include "modules/state_bss.inc"
#include "modules/startup.inc"
#include "modules/timer1.inc"
#include "modules/timer2.inc"
#include "modules/usart.inc"
#include "modules/scheduler_restart.inc"
#include "modules/status_dispatch.inc"
#include "modules/commands_input_timer.inc"
#include "modules/commands_pair_strings.inc"
#include "modules/commands_helpers.inc"
#include "modules/validation.inc"
#include "modules/event_queue.inc"
