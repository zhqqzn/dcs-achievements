# Проверка атомарности конфигурации после команды, которую прошивка отклоняет.
# Предполагается, что ELF загружен в simavr с включённым GDB-сервером и символами отладки.
# Точка останова command_error срабатывает на общем пути отказа; ниже сравниваются
# действующие интервалы active_t1/active_t2 и временные кандидаты test_t1/test_t2.
# Для отклонённой команды действующая конфигурация должна оставаться неизменной.
# Не останавливать выполнение команд GDB на страницах длинного вывода.
set pagination off
target remote :1234
# Проверять состояние именно на пути отказа команды, до обработки следующей команды
break command_error
commands
  silent
  printf "\nRejected command reached command_error. Expected active_t1/t2 = 25088 (0x6200):\n"
  printf "active_t1 bytes: "
  x/2ub &active_t1
  printf "active_t2 bytes: "
  x/2ub &active_t2
  printf "test_t1 candidate bytes: "
  x/2ub &test_t1
  printf "test_t2 candidate bytes: "
  x/2ub &test_t2
  continue
end
continue
