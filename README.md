# AcquiringTerminalNative

Драйвер эквайринговых терминалов для **1С:Предприятие 8.3** (внешняя компонента по технологии **Native API**).

Поддерживаемые терминалы:

| Терминал | Протокол | Операции |
|---|---|---|
| **Kaspi Smart POS** | Kaspi API v2 (HTTP GET + токен) | оплата, возврат, проверка связи |
| **Halyk** | PAX Third-Party Protocol (JSON POST) | оплата, возврат, сверка |
| **BCC (PAX)** | PAX Third-Party Protocol (JSON POST) | оплата, возврат, сверка |

Реализует стандартный интерфейс драйвера «ЭквайринговыйТерминал» (ревизия 2005) для подсистемы «Подключаемое оборудование» (БПО): `PayByPaymentCard`, `CancelPaymentByPaymentCard`, `Settlement`, `DeviceTest`, `GetParameters`/`SetParameter` и т.д. Методы доступны в английском и русском написании (`ОплатитьПлатежнойКартой`, `ИтогиДняПоКартам`, ...).

## Особенности

- **Настройки приходят из 1С** — хранятся в хранилище подключаемого оборудования, передаются драйверу через `SetParameter` и/или XML при подключении. Никаких файлов конфигурации вести на кассе не нужно
- **Токен Kaspi** (24 ч) хранится автоматически и обновляется через `revoke` при 401/403; первичная регистрация кассы — «Разрешить» на терминале
- Опрос статуса Kaspi раз в 1 сек до конечного статуса, `unknown` → `actualize` (не чаще раза в 10 сек), общий таймаут настраивается
- Чек терминала возвращается в 1С через out-параметр `slip`; корректная обработка UTF-8
- Возврат: у Kaspi способ и `transactionId` запоминаются с последней оплаты; у BCC — через `tagRRN` + `method` (0 карта / 1 WeChat QR / 2 BCC QR)
- Файл состояния (токены, реквизиты последней операции): `%LOCALAPPDATA%\AcquiringTerminalNative\state.json` — создаётся автоматически

## Параметры подключения

| Параметр | Назначение | По умолчанию |
|---|---|---|
| `TerminalType` | Kaspi / Halyk / BCC (PAX) | Kaspi |
| `IpAddress` | IP терминала в локальной сети | 192.168.1.1 |
| `Port` | Порт терминала | 8080 |
| `KaspiName` | Имя кассы при регистрации (`register`) | Kassa1 |
| `KaspiOwnCheque` | true — чек печатает касса (`owncheque`) | true |
| `KaspiTimeoutSec` | Таймаут ожидания операции, сек | 240 |
| `KaspiRefundMethod` | Способ возврата, если метод оплаты не сохранён (card/qr/alaqan) | card |
| `BCCRefundMethod` | Способ возврата BCC (0/1/2) | 0 |

Терминал и касса должны быть в одной локальной сети; для Kaspi — HTTPS-домен `*.kaspipos.kz` открыт на файрволе терминала.

## Сборка

Требуется Visual Studio 2022 (Build Tools) с рабочей нагрузкой C++.

```cmd
build.cmd            :: сборка Release x64 + x86 (артефакты: 1C\Release\AcquiringTerminalNative_32/64.dll)
build.cmd v143       :: то же с переопределением toolset (если v141_xp не установлен)
assemble.cmd         :: упаковка AcquiringTerminalNative.zip (MANIFEST.xml + INFO.XML + DLL)
```

Зависимости включены в репозиторий: SDK 1С Native API (`include/`), nlohmann/json (`include/nlohmann/`). CMake-сборка также поддерживается (`CMakeLists.txt`).

## Установка в 1С

1. «Администрирование → Подключаемое оборудование → Драйверы оборудования» — загрузить `AcquiringTerminalNative.zip`
2. Либо программно: `ПодключитьВнешнююКомпоненту(<путь к zip>, "InnovaIT", AddInType.Native)` → `Новый("AddIn.InnovaIT.innNative")`
3. Создать экземпляр оборудования с типом «Эквайринговый терминал», заполнить параметры, выполнить «Тест устройства»

## Тесты

Тестовый стенд не требует реального терминала: мок-сервер эмулирует Kaspi v2 и PAX.

```cmd
python -I test\mock_terminal.py     :: мок-сервер на 127.0.0.1:8080
test\testharness.exe                :: 13 тестов: Open/DeviceTest/Pay/Refund/Settle для всех типов
```

Сценарии через переменные окружения: `MOCK_KASPI_FAIL=1` (операция отклонена), `MOCK_KASPI_UNKNOWN=1` (статус unknown → actualize), `MOCK_KASPI_WAIT=5` (задержка успеха).

## Структура

```
AcquiringTerminalNative.cpp/.h  — логика драйвера (Kaspi v2, PAX, WinHTTP)
build.cmd / assemble.cmd        — сборка и упаковка
test/mock_terminal.py           — эмулятор терминалов
test/testharness.cpp            — консольный тестовый прогон DLL
1C/Release/MANIFEST.xml         — манифест компоненты для 1С
include/                        — SDK 1С Native API + nlohmann/json
```

## Лицензия

GPL-3.0. Основано на шаблоне внешних компонент 1С Native API.
