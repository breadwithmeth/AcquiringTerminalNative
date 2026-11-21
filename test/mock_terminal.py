# -*- coding: utf-8 -*-
"""Эмуляция терминалов для тестов драйвера innNative (без реального оборудования).

Kaspi Smart POS v2: GET /v2/register, /v2/revoke, /v2/deviceinfo, /v2/payment,
                    /v2/refund, /v2/status, /v2/actualize
PAX (BCC/Halyk):    POST / {"task": "purchase"|"refund"|"settle"|"echo", "data": {...}}

Переменные окружения:
  MOCK_PORT        порт HTTP (по умолчанию 8080)
  MOCK_KASPI_WAIT  секунд в статусе "wait" перед успехом (по умолчанию 2)
  MOCK_KASPI_FAIL  1 — завершать операцию статусом fail (по умолчанию 0)
  MOCK_KASPI_UNKNOWN 1 — два опроса отдавать unknown, затем success (по умолчанию 0)

Запуск: python -I mock_terminal.py
"""

import json as _json
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(os.environ.get("MOCK_PORT", "8080"))
KASPI_WAIT = float(os.environ.get("MOCK_KASPI_WAIT", "2"))
KASPI_FAIL = os.environ.get("MOCK_KASPI_FAIL", "0") == "1"
KASPI_UNKNOWN = os.environ.get("MOCK_KASPI_UNKNOWN", "0") == "1"

# accessToken -> refreshToken; processId -> состояние операции
TOKENS = {}
REFRESH_TOKENS = {}
PROCESSES = {}


def now_str():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def terminal_id():
    return "TERM-777"


def make_kaspi_cheque(amount, is_refund):
    """Чек Kaspi: карточный вариант (с rrn) и QR-вариант (с orderNumber)."""
    method = "card" if not is_refund else "card"
    cheque = {
        "storeName": "Магазин Тест",
        "city": "Алматы",
        "address": "ул. Тестовая 1",
        "status": "success",
        "amount": int(amount),
        "date": now_str(),
        "bin": "123456789021",
        "terminalId": terminal_id(),
        "method": method,
        "type": "refund" if is_refund else "payment",
    }
    if method == "card":
        cheque.update({
            "cardMask": "4400******1234",
            "icc": "0102AB03",
            "rrn": "517111027327",
            "authorizationCode": "AUTH123",
            "hostResponseCode": "00",
            "pinEntered": True,
        })
    else:
        cheque["orderNumber"] = "ORD-" + str(int(time.time() * 1000))
    return cheque


def kaspi_process(req_name):
    """Создаёт процесс операции: wait -> (unknown) -> success/fail."""
    pid = "PID" + str(int(time.time() * 1000))
    PROCESSES[pid] = {
        "started": time.time(),
        "wait": KASPI_WAIT,
        "fail": KASPI_FAIL,
        "unknown": KASPI_UNKNOWN,
        "unknown_count": 0,
        "is_refund": req_name.get("is_refund", False),
        "cheque": make_kaspi_cheque(req_name.get("amount", "10"), req_name.get("is_refund", False)),
        "transactionId": "TRX" + str(int(time.time() * 1000)),
    }
    return pid


def kaspi_status_payload(pid):
    proc = PROCESSES[pid]
    elapsed = time.time() - proc["started"]
    if elapsed < proc["wait"]:
        return {"processId": pid, "status": "wait", "subStatus": "WaitUser"}

    if proc["unknown"] and proc["unknown_count"] < 2:
        proc["unknown_count"] += 1
        return {"processId": pid, "status": "unknown"}

    if proc["fail"]:
        data = {"processId": pid, "status": "fail", "message": "Отклонено клиентом"}
    else:
        cheque = dict(proc["cheque"])
        cheque["status"] = "success"
        cheque["type"] = "refund" if proc.get("is_refund") else "payment"
        data = {
            "processId": pid,
            "status": "success",
            "transactionId": proc["transactionId"],
            "addInfo": {"IsOffer": False, "ProductType": "PURCHASE"},
            "chequeInfo": cheque,
        }
    return data


class Handler(BaseHTTPRequestHandler):

    def _send(self, obj, status=200):
        body = _json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _params(self):
        query = self.path.partition("?")[2]
        params = {}
        for kv in query.split("&"):
            if "=" in kv:
                k, v = kv.split("=", 1)
                params[k] = v
        return params

    def _authed(self):
        token = self.headers.get("accesstoken", "")
        return token in TOKENS

    def do_GET(self):
        path = self.path.partition("?")[0]
        p = self._params()

        if path == "/v2/register":
            name = p.get("name", "")
            token = "TOKEN-%s-%d" % (name, int(time.time()))
            TOKENS[token] = name
            refresh = "REFRESH-%s-%d" % (name, int(time.time()))
            REFRESH_TOKENS[refresh] = name
            self._send({"data": {"accessToken": token, "refreshToken": refresh,
                                 "expirationDate": now_str()}, "statusCode": 0})

        elif path == "/v2/revoke":
            refresh = p.get("refreshToken", "")
            if refresh not in REFRESH_TOKENS:
                self._send({"errorText": "bad refresh token", "statusCode": 999}, status=403)
                return
            token = "TOKEN-NEW-%d" % int(time.time())
            TOKENS[token] = REFRESH_TOKENS[refresh]
            new_refresh = "REFRESH-NEW-%d" % int(time.time())
            REFRESH_TOKENS[new_refresh] = REFRESH_TOKENS[refresh]
            self._send({"data": {"accessToken": token, "refreshToken": new_refresh,
                                 "expirationDate": now_str()}, "statusCode": 0})

        elif path == "/v2/deviceinfo":
            if not self._authed():
                self._send({"errorText": "Ошибка авторизации / сессия истекла", "statusCode": 999}, status=401)
                return
            self._send({"data": {"posNum": "POS-001", "serialNum": "SN123456",
                                 "terminalId": terminal_id()}, "statusCode": 0})

        elif path == "/v2/payment":
            if not self._authed():
                self._send({"errorText": "Ошибка авторизации / сессия истекла", "statusCode": 999}, status=401)
                return
            if not p.get("amount"):
                self._send({"errorText": "Missing param: amount", "statusCode": 999}, status=400)
                return
            pid = kaspi_process({"amount": p.get("amount", "0"), "is_refund": False})
            self._send({"data": {"processId": pid, "status": "wait"}, "statusCode": 0})

        elif path == "/v2/refund":
            if not self._authed():
                self._send({"errorText": "Ошибка авторизации / сессия истекла", "statusCode": 999}, status=401)
                return
            pid = kaspi_process({"amount": p.get("amount", "0"), "is_refund": True})
            self._send({"data": {"processId": pid, "status": "wait"}, "statusCode": 0})

        elif path == "/v2/status":
            pid = p.get("processId", "")
            if pid not in PROCESSES:
                self._send({"errorText": "Процесс не найден", "statusCode": 101})
                return
            self._send({"data": kaspi_status_payload(pid), "statusCode": 0})

        elif path == "/v2/actualize":
            pid = p.get("processId", "")
            if pid not in PROCESSES:
                self._send({"errorText": "Транзакция не найдена", "statusCode": 104})
                return
            self._send({"data": kaspi_status_payload(pid), "statusCode": 0})

        else:
            self._send({"errorText": "Неверный URL", "statusCode": 999}, status=404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length).decode("utf-8") if length else ""
        try:
            req = _json.loads(raw)
        except ValueError:
            self._send({"data": {"result": "-1", "msg": "bad json"}})
            return

        task = req.get("task", "")
        data = req.get("data") or {}

        if task == "echo":
            self._send({"data": {"msg": "Operation is Successful", "result": "0",
                                 "terminalId": "PAX-777"}})

        elif task == "purchase":
            amount = data.get("amount", "0")
            receipt = (
                "================\n"
                "       ОПЛАТА\n"
                "================\n"
                "Карта: 47617310******27\n"
                "Сумма: %s тг.\n"
                "RRN: 517111027327\n"
                "Код: AUTH123\n"
                "Терминал: PAX-777\n"
                "================\n"
            ) % amount
            self._send({"data": {
                "amount": amount + " тг.",
                "cardNo": "47617310******27",
                "channel": 5,
                "dateTime": time.strftime("%d.%m.%y.%H:%M:%S"),
                "issuerName": "VISA",
                "receiptInfo": receipt,
                "tagRRN": "517111027327",
                "msg": "Operation is Successful",
                "result": "0",
                "terminalId": "PAX-777",
            }})

        elif task == "refund":
            amount = data.get("amount", "0")
            tag_rrn = data.get("tagRRN", "")
            receipt = (
                "================\n"
                "       ВОЗВРАТ\n"
                "================\n"
                "Карта: 47617310******27\n"
                "Сумма: %s тг.\n"
                "RRN исходной: %s\n"
                "Терминал: PAX-777\n"
                "================\n"
            ) % (amount, tag_rrn)
            self._send({"data": {
                "amount": amount + " тг.",
                "cardNo": "47617310******27",
                "channel": 5,
                "dateTime": time.strftime("%d.%m.%y.%H:%M:%S"),
                "issuerName": "VISA",
                "receiptInfo": receipt,
                "tagRRN": tag_rrn or "517111027327",
                "msg": "Operation is Successful",
                "result": "0",
                "terminalId": "PAX-777",
            }})

        elif task == "settle":
            receipt = (
                "================\n"
                "   СВЕРКА ИТОГОВ\n"
                "================\n"
                "Терминал: PAX-777\n"
                "Операций: 3\n"
                "Сумма: 1500 тг.\n"
                "================\n"
            )
            self._send({"data": {
                "msg": "Operation is Successful",
                "result": "0",
                "terminalId": "PAX-777",
                "receiptInfo": receipt,
            }})

        else:
            self._send({"data": {"result": "-1", "msg": "Unknown task: " + task}})

    def log_message(self, fmt, *args):
        print("[mock] %s" % (fmt % args))


def main():
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print("Mock terminal server on http://127.0.0.1:%d (Kaspi v2 + PAX)" % PORT)
    print("  Kaspi wait=%s sec, fail=%s, unknown=%s" % (KASPI_WAIT, KASPI_FAIL, KASPI_UNKNOWN))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
