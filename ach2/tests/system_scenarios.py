"""Проверяем эксплуатационные сценарии в отдельном временном Compose-окружении."""

import concurrent.futures
import json
import os
import secrets
import sys
import time
import urllib.error
import urllib.request

BASE_URL = os.environ.get("BASE_URL", "http://127.0.0.1:18080")
MAX_INPUT = int(os.environ.get("MAX_INPUT", "1000000000005"))
BASE_URL_B = os.environ.get("BASE_URL_B", "http://127.0.0.1:18081")


def request_number(number: int, base_url: str = BASE_URL):
    req = urllib.request.Request(
        f"{base_url}/api/increment",
        data=str(number).encode("ascii"),
        headers={"Content-Type": "text/plain"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as response:
            return response.status, json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        try:
            raw = error.read().decode("utf-8")
            try:
                body = json.loads(raw)
            except json.JSONDecodeError:
                body = {"raw": raw}
            return error.code, body
        finally:
            error.close()


def choose_unused() -> int:
    # Выбираем значения отдельно от основной случайной выборки API-тестов,
    # чтобы эксплуатационные сценарии не зависели от порядка их выполнения.
    return secrets.randbelow(100_000_000_000) + 850_000_000_000


def prepare_persistence() -> None:
    for _attempt in range(100):
        number = choose_unused()
        status, body = request_number(number)
        if status == 200 and body.get("result") == number + 1:
            print(number)
            return
        if status not in (409,):
            raise RuntimeError(f"could not prepare persistence check: HTTP {status}, {body}")
    raise RuntimeError("could not find an unused number for the persistence scenario")


def expect_duplicate(number: int) -> None:
    last_result = None
    for _attempt in range(20):
        status, body = request_number(number)
        last_result = (status, body)
        if status == 503:
            time.sleep(1)
            continue
        if status != 409 or body.get("error", {}).get("code") != "DUPLICATE_NUMBER":
            raise AssertionError(f"expected persisted duplicate, got HTTP {status}: {body}")
        print(f"PASS persistence input={number}")
        return
    raise AssertionError(f"database did not recover for persisted duplicate: {last_result}")


def expect_health() -> None:
    # Проверяем liveness отдельно от доступности БД: процесс может отвечать,
    # даже когда операция, требующая хранилища, временно завершается ошибкой.
    req = urllib.request.Request(f"{BASE_URL}/healthz", method="GET")
    with urllib.request.urlopen(req, timeout=10) as response:
        body = json.loads(response.read().decode("utf-8"))
        if response.status != 200 or body != {"status": "ok"}:
            raise AssertionError(f"expected liveness response, got HTTP {response.status}: {body}")
    print("PASS liveness while database is unavailable")


def expect_unavailable(number: int) -> None:
    status, body = request_number(number)
    if status != 503 or body.get("error", {}).get("code") != "SERVICE_UNAVAILABLE":
        raise AssertionError(f"expected database failure HTTP 503, got HTTP {status}: {body}")
    print(f"PASS unavailable input={number}")


def expect_success(number: int) -> None:
    last_result = None
    for _attempt in range(20):
        status, body = request_number(number)
        last_result = (status, body)
        if status == 503:
            time.sleep(1)
            continue
        if status != 200 or body.get("input") != number or body.get("result") != number + 1:
            raise AssertionError(f"expected success after recovery/rollback, got HTTP {status}: {body}")
        print(f"PASS recovered input={number}")
        return
    raise AssertionError(f"database did not recover for successful request: {last_result}")




def cross_instance_concurrency() -> None:
    endpoints = [BASE_URL, BASE_URL_B] * 4
    for _attempt in range(50):
        number = choose_unused()
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(endpoints)) as executor:
            futures = [
                executor.submit(request_number, number, endpoint)
                for endpoint in endpoints
            ]
            results = [future.result() for future in futures]
        successes = [(status, body) for status, body in results if status == 200]
        duplicates = [
            (status, body) for status, body in results
            if status == 409 and body.get("error", {}).get("code") == "DUPLICATE_NUMBER"
        ]
        if len(successes) == 1 and len(duplicates) == len(endpoints) - 1:
            if successes[0][1].get("input") != number or successes[0][1].get("result") != number + 1:
                raise AssertionError(f"cross-instance success has the wrong result: {successes[0]}")
            print(f"PASS cross-instance concurrency input={number}")
            return
        # Если число уже использовалось, повторяем сценарий с новым кандидатом.
        if all(status == 409 for status, _body in results):
            continue
        raise AssertionError(f"unexpected cross-instance results for {number}: {results}")
    raise RuntimeError("could not find a fresh candidate for cross-instance concurrency")


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit("usage: system_scenarios.py choose-unused|prepare-persistence|cross-instance|expect-health|expect-duplicate N|expect-unavailable N|expect-success N")
    operation = sys.argv[1]
    if operation == "choose-unused":
        print(choose_unused())
        return
    if operation == "prepare-persistence":
        prepare_persistence()
        return
    if operation == "cross-instance":
        cross_instance_concurrency()
        return
    if operation == "expect-health":
        expect_health()
        return
    if len(sys.argv) != 3:
        raise SystemExit(f"{operation} requires one integer argument")
    number = int(sys.argv[2])
    if number < 0 or number > MAX_INPUT:
        raise SystemExit("scenario input is outside configured range")
    if operation == "expect-duplicate":
        expect_duplicate(number)
    elif operation == "expect-unavailable":
        expect_unavailable(number)
    elif operation == "expect-success":
        expect_success(number)
    else:
        raise SystemExit(f"unknown operation: {operation}")


if __name__ == "__main__":
    main()
