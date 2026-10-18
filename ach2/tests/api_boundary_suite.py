"""Проверяем границы диапазона на отдельной БД с чистой историей запросов."""

import json
import os
import unittest
import urllib.error
import urllib.request

BASE_URL = os.environ.get("BASE_URL", "http://127.0.0.1:18080")
MAX_INPUT = int(os.environ.get("MAX_INPUT", "1000000000005"))


def request_number(number: int):
    req = urllib.request.Request(
        f"{BASE_URL}/api/increment",
        data=str(number).encode("ascii"),
        headers={"Content-Type": "text/plain"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as response:
            return response.status, json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        try:
            return error.code, json.loads(error.read().decode("utf-8"))
        finally:
            error.close()


class ApiBoundaryTests(unittest.TestCase):
    def test_configured_maximum_and_first_value_above_it(self):
        status, body = request_number(MAX_INPUT)
        self.assertEqual(status, 200, body)
        self.assertEqual(body["input"], MAX_INPUT)
        self.assertEqual(body["result"], MAX_INPUT + 1)

        status, body = request_number(MAX_INPUT + 1)
        self.assertEqual(status, 400, body)
        self.assertEqual(body["error"]["code"], "NUMBER_OUT_OF_RANGE")

    def test_zero_is_accepted_and_then_recognized_as_duplicate(self):
        status, body = request_number(0)
        self.assertEqual(status, 200, body)
        self.assertEqual(body["input"], 0)
        self.assertEqual(body["result"], 1)

        status, body = request_number(0)
        self.assertEqual(status, 409, body)
        self.assertEqual(body["error"]["code"], "DUPLICATE_NUMBER")


if __name__ == "__main__":
    unittest.main(verbosity=2)
