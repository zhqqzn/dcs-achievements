#!/bin/sh
# Интеграционные тесты двух C-анализаторов: фикстуры генерируются на C, без внешних файлов-образцов
set -eu

mkdir -p build

# Сначала положительная трасса периодического режима: корректные данные должны приниматься
build/test-trace-fixture task2 build/test-task2.csv
build/analyze-udr-trace build/test-task2.csv --t1 16384 --t2 16384 --phi 8192 --tolerance 4 --minimum-lines 10 > build/test-task2.out
if ! grep -q '^PASS:' build/test-task2.out; then
    cat build/test-task2.out
    echo 'FAIL: positive task-2 trace was rejected' >&2
    exit 1
fi
echo 'PASS: valid task-2 UDR trace is accepted'

# Затем отрицательная трасса с нарушением допуска: нулевой код возврата здесь означает ошибку анализатора
build/test-trace-fixture task2-bad build/test-task2-bad.csv
if build/analyze-udr-trace build/test-task2-bad.csv --t1 16384 --t2 16384 --phi 8192 --tolerance 4 --minimum-lines 10 > build/test-task2-bad.out; then
    echo 'FAIL: out-of-bound task-2 trace was accepted' >&2
    exit 1
fi
if ! grep -q '^FAIL:' build/test-task2-bad.out; then
    cat build/test-task2-bad.out
    echo 'FAIL: bad task-2 trace failed for an unexpected reason' >&2
    exit 1
fi
echo 'PASS: out-of-bound task-2 UDR trace is rejected'

# То же для командной нагрузки: анализатор должен восстановить эпохи и целостность строк
build/test-trace-fixture stress build/test-stress.csv
build/analyze-stress-trace build/test-stress.csv --tolerance 4 --minimum-epochs 20 --minimum-events 10 > build/test-stress.out
if ! grep -q '^PASS:' build/test-stress.out; then
    cat build/test-stress.out
    echo 'FAIL: positive command-load trace was rejected' >&2
    exit 1
fi
echo 'PASS: valid command-load trace is accepted'

# Отрицательная командная трасса подтверждает, что превышение временного допуска не маскируется PASS
build/test-trace-fixture stress-bad build/test-stress-bad.csv
if build/analyze-stress-trace build/test-stress-bad.csv --tolerance 4 --minimum-epochs 20 --minimum-events 10 > build/test-stress-bad.out; then
    echo 'FAIL: out-of-bound command-load trace was accepted' >&2
    exit 1
fi
if ! grep -q '^FAIL:' build/test-stress-bad.out; then
    cat build/test-stress-bad.out
    echo 'FAIL: bad command-load trace failed for an unexpected reason' >&2
    exit 1
fi
echo 'PASS: out-of-bound command-load trace is rejected'
