# Параметры целевой прошивки
MCU := atmega8
F_CPU := 16000000UL
TARGET := build/firmware
TASK2_TARGET := build/firmware-task2
STRESS_TARGET := build/firmware-stress-trace
SRC := src/firmware.S
ASM_MODULES := $(wildcard src/modules/*.inc)

# Инструменты AVR и программы на хосте
AVR_CC ?= avr-gcc
AVR_OBJCOPY ?= avr-objcopy
AVR_OBJDUMP ?= avr-objdump
SIMAVR ?= simavr
CC ?= cc
HOST_CFLAGS ?= -std=c11 -O2 -Wall -Wextra
SIMAVR_PTY := build/simavr-pty
SIMAVR_TRACE := build/simavr-trace
SIMAVR_STRESS_TRACE := build/simavr-stress-trace
STRESS_TRACE := build/stress_trace.csv
STRESS_DONE_FILE ?= /tmp/avr_pingpong_stress.done
STRESS_CYCLES ?= 800000000
# PTY- и трассировочные обёртки используют библиотеки из пакетов разработки simavr
SIMAVR_CFLAGS := $(shell pkg-config --cflags simavr simavrparts 2>/dev/null)
SIMAVR_LIBS := $(shell pkg-config --libs simavr simavrparts 2>/dev/null)

# Вспомогательные программы и файлы диагностических прогонов
CONSOLE := build/console
STRESS_CLIENT := build/stress-commands
ANALYZE_UDR := build/analyze-udr-trace
ANALYZE_STRESS := build/analyze-stress-trace
TEST_TOOLS := build/test-tools
TRACE_FIXTURE := build/test-trace-fixture

.PHONY: all all-task2 clean sim sim-task2 sim-pty sim-pty-task2 trace-task2 trace-stress disasm disasm-task2 check-tools test-model console

# Полная версия: периодический вывод и обработка команд USART
all: $(TARGET).elf $(TARGET).hex

build:
	mkdir -p build

# .S проходит C-препроцессор; подключённые .inc-файлы образуют одну единицу ассемблирования
$(TARGET).elf: $(SRC) src/config.inc $(ASM_MODULES) | build
	$(AVR_CC) -mmcu=$(MCU) -DF_CPU=$(F_CPU) -I src -x assembler-with-cpp -g -Os -Wall -Wextra $< -o $@

$(TARGET).hex: $(TARGET).elf
	$(AVR_OBJCOPY) -O ihex -R .eeprom $< $@

# Вариант средней сложности: только периодические строки с равными интервалами
$(TASK2_TARGET).elf: $(SRC) src/config.inc $(ASM_MODULES) | build
	$(AVR_CC) -mmcu=$(MCU) -DF_CPU=$(F_CPU) -DTASK2_PERIODIC_ONLY=1 -DTIMER1_INTERVAL=16384 -DTIMER2_INTERVAL=16384 -I src -x assembler-with-cpp -g -Os -Wall -Wextra $< -o $@

$(TASK2_TARGET).hex: $(TASK2_TARGET).elf
	$(AVR_OBJCOPY) -O ihex -R .eeprom $< $@

all-task2: $(TASK2_TARGET).elf $(TASK2_TARGET).hex

# Листинги для просмотра машинных инструкций и анализа временных путей
disasm: $(TARGET).elf
	$(AVR_OBJDUMP) -d -S $(TARGET).elf > $(TARGET).lst

disasm-task2: $(TASK2_TARGET).elf
	$(AVR_OBJDUMP) -d -S $(TASK2_TARGET).elf > $(TASK2_TARGET).lst

# Обычный simavr выводит UART в терминал; для ввода команд используйте PTY-цель
sim: $(TARGET).elf
	$(SIMAVR) -m $(MCU) -f 16000000 $(TARGET).elf

sim-task2: $(TASK2_TARGET).elf
	$(SIMAVR) -m $(MCU) -f 16000000 $(TASK2_TARGET).elf

# PTY связывает USART эмулируемого МК с тестовым клиентом
$(SIMAVR_PTY): tools/simavr_pty.c | build
	@pkg-config --exists simavr simavrparts || { echo "Missing simavr development files; install libsimavr-dev"; exit 1; }
	$(CC) $(HOST_CFLAGS) $(SIMAVR_CFLAGS) $< -o $@ $(SIMAVR_LIBS) -pthread -lutil

$(SIMAVR_TRACE): tools/simavr_trace.c | build
	@pkg-config --exists simavr simavrparts || { echo "Missing simavr development files; install libsimavr-dev"; exit 1; }
	$(CC) $(HOST_CFLAGS) $(SIMAVR_CFLAGS) $< -o $@ $(SIMAVR_LIBS) -pthread -lutil

# Диагностическая прошивка записывает маркеры расписания для проверки под нагрузкой
$(STRESS_TARGET).elf: $(SRC) src/config.inc $(ASM_MODULES) | build
	$(AVR_CC) -mmcu=$(MCU) -DF_CPU=$(F_CPU) -DTIMER1_INTERVAL=24064 -DTIMER2_INTERVAL=24064 -DTRACE_SCHEDULE=1 -I src -x assembler-with-cpp -g -Os -Wall -Wextra $< -o $@

$(SIMAVR_STRESS_TRACE): tools/simavr_stress_trace.c | build
	@pkg-config --exists simavr simavrparts || { echo "Missing simavr development files; install libsimavr-dev"; exit 1; }
	$(CC) $(HOST_CFLAGS) $(SIMAVR_CFLAGS) $< -o $@ $(SIMAVR_LIBS) -pthread -lutil

$(CONSOLE): tools/console.c | build
	$(CC) $(HOST_CFLAGS) $< -o $@

$(STRESS_CLIENT): tools/stress_commands.c | build
	$(CC) $(HOST_CFLAGS) $< -o $@

$(ANALYZE_UDR): tools/analyze_udr_trace.c | build
	$(CC) $(HOST_CFLAGS) $< -o $@

$(ANALYZE_STRESS): tools/analyze_stress_trace.c | build
	$(CC) $(HOST_CFLAGS) $< -o $@

$(TEST_TOOLS): tools/test_tools.c tools/schedule_model.c tools/schedule_model.h | build
	$(CC) $(HOST_CFLAGS) -I tools tools/test_tools.c tools/schedule_model.c -lm -o $@

$(TRACE_FIXTURE): tools/test_trace_fixture.c | build
	$(CC) $(HOST_CFLAGS) $< -o $@

# Запустите цель в первом терминале; после приглашения запустите клиента во втором
# Клиент создаёт STRESS_DONE_FILE при завершении, после чего трассировщик останавливается
# Анализатор проверяет целостность строк и отклонение записи первого байта в UDR (допуск 4 такта)
trace-stress: $(STRESS_TARGET).elf $(SIMAVR_STRESS_TRACE) $(STRESS_CLIENT) $(ANALYZE_STRESS)
	rm -f $(STRESS_DONE_FILE) $(STRESS_TRACE)
	./$(SIMAVR_STRESS_TRACE) $(STRESS_TARGET).elf $(STRESS_CYCLES) $(STRESS_TRACE) $(STRESS_DONE_FILE)
	./$(ANALYZE_STRESS) $(STRESS_TRACE) --tolerance 4 --minimum-epochs 20 --minimum-events 10

# Запуск без GDB: остановки процессора меняют результаты измерения. Трасса содержит циклы записи в UDR
trace-task2: $(TASK2_TARGET).elf $(SIMAVR_TRACE) $(ANALYZE_UDR)
	./$(SIMAVR_TRACE) $(TASK2_TARGET).elf 3000000 build/udr_trace.csv
	./$(ANALYZE_UDR) build/udr_trace.csv --t1 16384 --t2 16384 --phi 8192 --tolerance 4

sim-pty: $(TARGET).elf $(SIMAVR_PTY) $(CONSOLE)
	./$(SIMAVR_PTY) $(TARGET).elf

sim-pty-task2: $(TASK2_TARGET).elf $(SIMAVR_PTY) $(CONSOLE)
	./$(SIMAVR_PTY) $(TASK2_TARGET).elf

console: $(CONSOLE)
	@echo "Run ./$(CONSOLE) in another terminal while simavr-pty is running."

# Проверка наличия основных компиляторов и симулятора
check-tools:
	@command -v $(AVR_CC) >/dev/null || { echo "Missing avr-gcc; install gcc-avr avr-libc binutils-avr"; exit 1; }
	@command -v $(SIMAVR) >/dev/null || { echo "Missing simavr; install simavr"; exit 1; }
	@command -v $(CC) >/dev/null || { echo "Missing host C compiler"; exit 1; }

# Проверяет расчётную модель и анализаторы на корректных и ошибочных трассах
test-model: $(TEST_TOOLS) $(ANALYZE_UDR) $(ANALYZE_STRESS) $(TRACE_FIXTURE)
	./$(TEST_TOOLS)
	sh tools/test_analyzers.sh

# Удаляет только производные файлы из build/
clean:
	rm -f build/*
