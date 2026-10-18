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

# Общий парсер CSV должен отклонять лишние поля вместо того, чтобы молча их игнорировать
{ head -n 1 build/test-task2.csv; sed -n '2p' build/test-task2.csv | sed 's/$/,unexpected/'; tail -n +3 build/test-task2.csv; } > build/test-task2-extra-field.csv
if build/analyze-udr-trace build/test-task2-extra-field.csv --t1 16384 --t2 16384 --phi 8192 --tolerance 4 --minimum-lines 10 > build/test-task2-extra-field.out 2>&1; then
    echo 'FAIL: periodic analyzer accepted a CSV row with an extra field' >&2
    exit 1
fi
if ! grep -q '^FAIL: malformed or unknown trace CSV row:' build/test-task2-extra-field.out; then
    cat build/test-task2-extra-field.out
    echo 'FAIL: malformed CSV row was rejected for an unexpected reason' >&2
    exit 1
fi
echo 'PASS: CSV parser rejects rows with extra fields'

# U-записи представляют собой байты, поэтому значение шире 8 бит нарушает контракт трассы
{ cat build/test-task2.csv; printf 'U,"42",256\n'; } > build/test-task2-byte-overflow.csv
if build/analyze-udr-trace build/test-task2-byte-overflow.csv --t1 16384 --t2 16384 --phi 8192 --tolerance 4 --minimum-lines 10 > build/test-task2-byte-overflow.out 2>&1; then
    echo 'FAIL: periodic analyzer accepted a UDR value wider than one byte' >&2
    exit 1
fi
if ! grep -q '^FAIL: malformed or unknown trace CSV row:' build/test-task2-byte-overflow.out; then
    cat build/test-task2-byte-overflow.out
    echo 'FAIL: out-of-range CSV value was rejected for an unexpected reason' >&2
    exit 1
fi
echo 'PASS: CSV parser rejects out-of-range byte values'

# Трасса из другого режима не должна молча обрабатываться как периодическая трасса UDR
sed 's/^U,/P,/' build/test-task2.csv > build/test-task2-wrong-kind.csv
if build/analyze-udr-trace build/test-task2-wrong-kind.csv --t1 16384 --t2 16384 --phi 8192 --tolerance 4 --minimum-lines 10 > build/test-task2-wrong-kind.out 2>&1; then
    echo 'FAIL: periodic analyzer accepted a stress-only record kind' >&2
    exit 1
fi
if ! grep -q '^FAIL: record kind' build/test-task2-wrong-kind.out; then
    cat build/test-task2-wrong-kind.out
    echo 'FAIL: wrong trace kind was rejected for an unexpected reason' >&2
    exit 1
fi
echo 'PASS: periodic analyzer rejects records from another trace mode'

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
