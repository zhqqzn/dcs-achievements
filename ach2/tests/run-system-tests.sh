#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$ROOT"
command -v docker >/dev/null 2>&1 || { echo "ERROR: Docker Engine CLI is required" >&2; exit 2; }

TEST_PROJECT="ach2-system-tests-$$"
TEST_PORT="${TEST_WEB_PORT:-18080}"
TEST_PORT_2="${TEST_WEB2_PORT:-18081}"
TEST_MAX_INPUT="1000000000005"
# Создаём временную конфигурацию рядом с проектом, чтобы Compose находил её и при запуске через sudo.
TEST_ENV="$(mktemp "$ROOT/.env.system-test.XXXXXX")"

cat > "$TEST_ENV" <<EOF
DB_HOST=database
DB_PORT=5432
DB_NAME=linear_test
DB_USER=linear_test
DB_PASSWORD=isolated_test_only_password
MAX_INPUT=$TEST_MAX_INPUT
WEB_PORT=$TEST_PORT
TEST_WEB2_PORT=$TEST_PORT_2
EOF
# Разрешаем читать конфигурацию клиенту Compose, который запускаем через sudo.
chmod 644 "$TEST_ENV"

if docker info >/dev/null 2>&1; then
    USE_SUDO=0
else
    USE_SUDO=1
fi

compose() {
    if [[ "$USE_SUDO" == 1 ]]; then
        sudo docker compose --env-file "$TEST_ENV" --project-name "$TEST_PROJECT" -f compose.yaml -f tests/compose-scale-test.yaml "$@"
    else
        docker compose --env-file "$TEST_ENV" --project-name "$TEST_PROJECT" -f compose.yaml -f tests/compose-scale-test.yaml "$@"
    fi
}

cleanup() {
    local status=$?
    trap - EXIT
    if [[ "$status" -ne 0 ]]; then
        echo "ERROR: isolated system tests failed; collecting diagnostics before cleanup" >&2
        compose ps >&2 || true
        compose logs --tail=150 database app app2 web web2 >&2 || true
    fi
    # Удаляем только временный тестовый проект и его том; обычную БД ach2 не трогаем.
    compose down --volumes --remove-orphans >/dev/null 2>&1 || true
    rm -f "$TEST_ENV"
    exit "$status"
}
trap cleanup EXIT

wait_for_stack() {
    local attempt
    for attempt in $(seq 1 90); do
        if compose exec -T database sh -c 'pg_isready -h 127.0.0.1 -U "$POSTGRES_USER" -d "$POSTGRES_DB"' >/dev/null 2>&1 \
            && curl --silent --show-error --fail "http://127.0.0.1:${TEST_PORT}/healthz" >/dev/null 2>&1 \
            && curl --silent --show-error --fail "http://127.0.0.1:${TEST_PORT_2}/healthz" >/dev/null 2>&1; then
            return 0
        fi
        sleep 2
    done
    echo "ERROR: test stack did not become ready on port ${TEST_PORT}" >&2
    compose ps >&2 || true
    compose logs --tail=100 database app app2 web web2 >&2 || true
    return 1
}

run_scenarios() {
    local base_url="http://127.0.0.1:${TEST_PORT}"
    BASE_URL="$base_url" MAX_INPUT="$TEST_MAX_INPUT" \
        python3 -m unittest discover -s tests -p test_api.py -v
    BASE_URL="$base_url" MAX_INPUT="$TEST_MAX_INPUT" \
        python3 tests/api_boundary_suite.py -v
    BASE_URL="$base_url" BASE_URL_B="http://127.0.0.1:${TEST_PORT_2}" MAX_INPUT="$TEST_MAX_INPUT" \
        python3 tests/system_scenarios.py cross-instance

    local persistence_number
    persistence_number="$(BASE_URL="$base_url" MAX_INPUT="$TEST_MAX_INPUT" python3 tests/system_scenarios.py prepare-persistence)"
    compose restart app >/dev/null
    wait_for_stack
    BASE_URL="$base_url" MAX_INPUT="$TEST_MAX_INPUT" \
        python3 tests/system_scenarios.py expect-duplicate "$persistence_number"

    # Перезапускаем саму БД и убеждаемся, что постоянный том сохраняет историю.
    compose restart database >/dev/null
    wait_for_database
    BASE_URL="$base_url" MAX_INPUT="$TEST_MAX_INPUT" \
        python3 tests/system_scenarios.py expect-duplicate "$persistence_number"

    local missing_state_number
    missing_state_number="$(BASE_URL="$base_url" MAX_INPUT="$TEST_MAX_INPUT" python3 tests/system_scenarios.py choose-unused)"
    compose exec -T database sh -c 'psql -v ON_ERROR_STOP=1 -U "$POSTGRES_USER" -d "$POSTGRES_DB" -c "DELETE FROM request_state WHERE id = 1"' >/dev/null
    BASE_URL="$base_url" MAX_INPUT="$TEST_MAX_INPUT" \
        python3 tests/system_scenarios.py expect-unavailable "$missing_state_number"
    compose exec -T database sh -c 'psql -v ON_ERROR_STOP=1 -U "$POSTGRES_USER" -d "$POSTGRES_DB" -c "INSERT INTO request_state (id) VALUES (1) ON CONFLICT (id) DO NOTHING"' >/dev/null
    BASE_URL="$base_url" MAX_INPUT="$TEST_MAX_INPUT" \
        python3 tests/system_scenarios.py expect-success "$missing_state_number"

    local rollback_number
    rollback_number="$(BASE_URL="$base_url" MAX_INPUT="$TEST_MAX_INPUT" python3 tests/system_scenarios.py choose-unused)"
    compose exec -T database sh -c 'psql -v ON_ERROR_STOP=1 -U "$POSTGRES_USER" -d "$POSTGRES_DB" -c "ALTER TABLE processed_numbers RENAME TO processed_numbers_test_hidden"' >/dev/null
    BASE_URL="$base_url" MAX_INPUT="$TEST_MAX_INPUT" \
        python3 tests/system_scenarios.py expect-unavailable "$rollback_number"
    compose exec -T database sh -c 'psql -v ON_ERROR_STOP=1 -U "$POSTGRES_USER" -d "$POSTGRES_DB" -c "ALTER TABLE processed_numbers_test_hidden RENAME TO processed_numbers"' >/dev/null
    BASE_URL="$base_url" MAX_INPUT="$TEST_MAX_INPUT" \
        python3 tests/system_scenarios.py expect-success "$rollback_number"

    local outage_number
    outage_number="$(BASE_URL="$base_url" MAX_INPUT="$TEST_MAX_INPUT" python3 tests/system_scenarios.py choose-unused)"
    compose stop database >/dev/null
    BASE_URL="$base_url" MAX_INPUT="$TEST_MAX_INPUT" \
        python3 tests/system_scenarios.py expect-health
    BASE_URL="$base_url" MAX_INPUT="$TEST_MAX_INPUT" \
        python3 tests/system_scenarios.py expect-unavailable "$outage_number"
    compose start database >/dev/null
    wait_for_database
    BASE_URL="$base_url" MAX_INPUT="$TEST_MAX_INPUT" \
        python3 tests/system_scenarios.py expect-success "$outage_number"

    local logs
    logs="$(compose logs --no-color --since=10m app 2>&1)"
    grep -F "INFO code=NUMBER_PROCESSED input=${persistence_number} result=$((persistence_number + 1))" <<<"$logs" >/dev/null
    grep -F "ERROR code=DUPLICATE_NUMBER input=${persistence_number}" <<<"$logs" >/dev/null
    grep -F 'ERROR code=INVALID_NUMBER message="expected a non-negative decimal integer in the request body"' <<<"$logs" >/dev/null
    grep -F 'ERROR code=METHOD_NOT_ALLOWED method=GET path=/api/increment' <<<"$logs" >/dev/null
    grep -F "ERROR code=DATABASE_OPERATION_FAILED input=${rollback_number} stage=check_processed_numbers" <<<"$logs" >/dev/null
    grep -F "ERROR code=DATABASE_SCHEMA_INVALID input=${missing_state_number} stage=request_state_row_missing" <<<"$logs" >/dev/null
    grep -E "ERROR code=(DATABASE_UNAVAILABLE|DATABASE_OPERATION_FAILED) input=${outage_number} " <<<"$logs" >/dev/null
    echo "PASS stdout logging scenarios"
}

wait_for_database() {
    local attempt
    for attempt in $(seq 1 60); do
        if compose exec -T database sh -c 'pg_isready -h 127.0.0.1 -U "$POSTGRES_USER" -d "$POSTGRES_DB"' >/dev/null 2>&1; then
            return 0
        fi
        sleep 2
    done
    echo "ERROR: database did not recover" >&2
    compose logs --tail=100 database app app2 >&2 || true
    return 1
}

printf 'Starting isolated system-test stack %s on ports %s and %s\n' "$TEST_PROJECT" "$TEST_PORT" "$TEST_PORT_2"
compose up --build -d
wait_for_stack
run_scenarios
printf '%s\n' 'PASS full system test suite'
