import concurrent.futures
import json
import os
import secrets
import unittest
from typing import Optional
import urllib.error
import urllib.request

BASE_URL = os.environ.get("BASE_URL", "http://127.0.0.1:8080")
MAX_INPUT = int(os.environ.get("MAX_INPUT", "9223372036854775806"))


# Отправляем реальный HTTP-запрос и возвращаем код вместе с JSON для единых проверок.
def request(method: str, body: Optional[str] = None):
    data = None if body is None else body.encode("utf-8")
    headers = {}
    if data is not None:
        headers["Content-Type"] = "text/plain"
    req = urllib.request.Request(
        f"{BASE_URL}/api/increment", data=data, headers=headers, method=method
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as response:
            raw = response.read().decode("utf-8")
            return response.status, json.loads(raw)
    except urllib.error.HTTPError as error:
        try:
            raw = error.read().decode("utf-8")
            try:
                body = json.loads(raw)
            except json.JSONDecodeError:
                # Учитываем ответ Nginx в HTML: веб-сервер может отклонить тело раньше Drogon.
                body = {"raw": raw}
            return error.code, body
        finally:
            error.close()


def new_candidate() -> int:
    upper = min(MAX_INPUT - 2, 1_000_000_000_000)
    if upper < 2:
        raise RuntimeError("The configured MAX_INPUT is too small for integration tests")
    # Выбираем число из широкого диапазона, чтобы не столкнуться с данными прошлых
    # запусков: история тестовых запросов сохраняется в постоянном томе PostgreSQL.
    return secrets.randbelow(upper - 1) + 1


class IncrementApiTests(unittest.TestCase):
    # Проверяем успешный ответ API и точное значение n + 1.
    def test_success_returns_number_plus_one(self):
        number = new_candidate()
        for _ in range(40):
            status, body = request("POST", str(number))
            if status == 200:
                break
            number = new_candidate()
        self.assertEqual(status, 200, body)
        self.assertEqual(body["input"], number)
        self.assertEqual(body["result"], number + 1)

    # Проверяем повтор через публичный HTTP-интерфейс, а не прямой вызов сервиса.
    def test_duplicate_number_is_exception_one(self):
        number = new_candidate()
        for _ in range(40):
            status, _body = request("POST", str(number))
            if status == 200:
                break
            number = new_candidate()
        self.assertEqual(status, 200)

        status, body = request("POST", str(number))
        self.assertEqual(status, 409)
        self.assertEqual(body["error"]["code"], "DUPLICATE_NUMBER")

    # Проверяем оба исхода для одного числа: сначала исключение № 2, затем № 1.
    def test_predecessor_is_exception_two_then_duplicate(self):
        matched = False
        last_result = None
        for _ in range(40):
            predecessor = new_candidate()
            status, _body = request("POST", str(predecessor + 1))
            if status != 200:
                continue

            status, body = request("POST", str(predecessor))
            last_result = (status, body)
            if (status == 409 and
                    body.get("error", {}).get("code") == "PREDECESSOR_OF_PROCESSED_NUMBER"):
                matched = True
                break
            # Кандидат мог остаться в истории предыдущего запуска; тогда выбираем
            # другую пару, чтобы проверить именно исключение № 2.

        self.assertTrue(matched, last_result)
        self.assertEqual(body["error"]["already_processed"], predecessor + 1)

        status, duplicate_body = request("POST", str(predecessor))
        self.assertEqual(status, 409)
        self.assertEqual(duplicate_body["error"]["code"], "DUPLICATE_NUMBER")

    # Проверяем, что ведущие нули не создают отдельное числовое значение:
    # сохраняем именно число, поэтому его каноническая запись тоже считается повтором.
    def test_leading_zeros_do_not_bypass_duplicate_detection(self):
        for _attempt in range(40):
            number = new_candidate()
            status, body = request("POST", f"000{number}")
            if status == 200:
                break
        self.assertEqual(status, 200, body)
        self.assertEqual(body["input"], number)
        self.assertEqual(body["result"], number + 1)

        status, body = request("POST", str(number))
        self.assertEqual(status, 409, body)
        self.assertEqual(body["error"]["code"], "DUPLICATE_NUMBER")

    # Проверяем разные нарушения формата: пустое тело, текст, дробь, знак и пробелы.
    # Эти запросы должны отклоняться до обращения к бизнес-логике и БД.
    def test_invalid_number_formats_are_rejected(self):
        invalid_bodies = ("", "abc", "12x", "12.5", "-1", "+1", " 12", "12 ", "1\n", "1\x00", "１２")
        for raw_body in invalid_bodies:
            with self.subTest(body=raw_body):
                status, body = request("POST", raw_body)
                self.assertEqual(status, 400, body)
                self.assertEqual(body["error"]["code"], "INVALID_NUMBER")

    # Отделяем переполнение формата int64_t от числа, которое лишь выше MAX_INPUT:
    # значение за пределами int64_t не представимо и отклоняется парсером.
    def test_integer_overflow_is_rejected(self):
        status, body = request("POST", "9223372036854775808")
        self.assertEqual(status, 400, body)
        self.assertEqual(body["error"]["code"], "INVALID_NUMBER")

    # Проверяем верхнюю границу независимо от настройки MAX_INPUT: сервис не может
    # разрешить INT64_MAX, потому что для n + 1 требуется представимое значение.
    def test_number_above_configured_maximum_is_rejected(self):
        status, body = request("POST", str(MAX_INPUT + 1))
        self.assertEqual(status, 400, body)
        self.assertEqual(body["error"]["code"], "NUMBER_OUT_OF_RANGE")

        # Отказ по диапазону не меняет историю: тот же вход отклоняется той же ошибкой.
        status, body = request("POST", str(MAX_INPUT + 1))
        self.assertEqual(status, 400, body)
        self.assertEqual(body["error"]["code"], "NUMBER_OUT_OF_RANGE")

    # Подтверждаем, что маршрут не принимает другие HTTP-методы.
    def test_only_post_is_allowed(self):
        for method in ("GET", "PUT", "PATCH", "DELETE", "OPTIONS"):
            with self.subTest(method=method):
                status, body = request(method)
                self.assertEqual(status, 405, body)
                self.assertEqual(body["error"]["code"], "METHOD_NOT_ALLOWED")

    # Проверяем liveness-маршрут отдельно от бизнес-операций и состояния БД.
    def test_health_endpoint_reports_process_status(self):
        req = urllib.request.Request(f"{BASE_URL}/healthz", method="GET")
        with urllib.request.urlopen(req, timeout=10) as response:
            self.assertEqual(response.status, 200)
            self.assertEqual(json.loads(response.read().decode("utf-8")), {"status": "ok"})

    # Проверяем, что разные независимые значения тоже обрабатываются при конкуренции.
    def test_concurrent_distinct_numbers_are_processed(self):
        final_results = None
        for _attempt in range(30):
            base = new_candidate()
            numbers = [base + offset * 1000 for offset in range(8)]
            if numbers[-1] > MAX_INPUT:
                continue
            with concurrent.futures.ThreadPoolExecutor(max_workers=len(numbers)) as executor:
                futures = [executor.submit(request, "POST", str(number)) for number in numbers]
                final_results = [(number, future.result()) for number, future in zip(numbers, futures)]
            if all(status == 200 for _number, (status, _body) in final_results):
                break
        self.assertIsNotNone(final_results)
        for number, (status, body) in final_results:
            with self.subTest(number=number):
                self.assertEqual(status, 200, body)
                self.assertEqual(body["input"], number)
                self.assertEqual(body["result"], number + 1)

    # Проверяем ограничение размера тела на веб-сервере до передачи запроса приложению.
    def test_request_body_over_nginx_limit_is_rejected(self):
        status, body = request("POST", "1" * 65)
        self.assertEqual(status, 413, body)

    # Отправляем одинаковое число одновременно и требуем ровно один успешный ответ.
    def test_concurrent_duplicate_requests_are_serialized(self):
        results = []
        for _attempt in range(30):
            number = new_candidate()
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
                futures = [
                    executor.submit(request, "POST", str(number))
                    for _ in range(8)
                ]
                results = [future.result() for future in futures]
            statuses = [status for status, _body in results]
            if statuses.count(200) == 1 and statuses.count(409) == 7:
                break
        self.assertEqual(statuses.count(200), 1, results)
        self.assertEqual(statuses.count(409), 7, results)
        for status, body in results:
            if status == 409:
                self.assertEqual(body["error"]["code"], "DUPLICATE_NUMBER")


if __name__ == "__main__":
    unittest.main(verbosity=2)
