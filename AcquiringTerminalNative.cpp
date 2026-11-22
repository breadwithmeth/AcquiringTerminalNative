#include "AcquiringTerminalNative.h"
#ifdef WIN32
#pragma setlocale("ru-RU" )
#endif
#include <map>
#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <ctime>

#pragma comment(lib, "winhttp.lib")

#include <fstream>

#include <nlohmann/json.hpp>
using json = nlohmann::json;


static const wchar_t* g_PropNames[] = {
    L"Verison"
};
static const wchar_t* g_PropNamesRu[] = {
    L"Версия"
};

static const wchar_t* g_MethodNames[] = {
    L"GetVersion",
    L"GetDescription",
    L"GetLastError",
    L"GetParameters",
    L"SetParameter",
    L"Open",
    L"Close",
    L"DeviceTest",
    L"GetAdditionalActions",
    L"DoAdditionalAction",
    L"PayByPaymentCard",
    L"PrintSlipOnTerminal",
    L"ConnectEquipment",
    L"CancelPaymentByPaymentCard",
    L"GetInterfaceRevision",
    L"Settlement",

};
static const wchar_t* g_MethodNamesRu[] = {
    L"ПолучитьНомерВерсии",
    L"ПолучитьОписание",
    L"ПолучитьОшибку",
    L"ПолучитьПараметры",
    L"УстановитьПараметр",
    L"Подключить",
    L"Отключить",
    L"ТестУстройства",
    L"ПолучитьДополнительныеДействия",
    L"ВыполнитьДополнительноеДействие",
    L"ОплатитьПлатежнойКартой",
    L"ПечатьКвитанцийНаТерминале",
    L"ПодключитьОборудование",
    L"ОтменитьПлатежПоПлатежнойКарте",
    L"ПолучитьРевизиюИнтерфейса",
    L"ИтогиДняПоКартам",

};

static const wchar_t g_kClassNames[] = L"innNative";
static const wchar_t* extensionName = L"InnovaIT";
static const wchar_t* extensionVersion = L"1.0.1.0"; // Мажорная версия, минорная версия, исправления, номер сборки
static IAddInDefBase* pAsyncEvent = NULL;
static const bool DEBUG = false;

// Последняя ошибка для GetLastError (заполняется в addError)
static std::wstring g_lastErrorText;
static long g_lastErrorCode = 0;
uint32_t convToShortWchar(WCHAR_T** Dest, const wchar_t* Source, uint32_t len = 0);
uint32_t convFromShortWchar(wchar_t** Dest, const WCHAR_T* Source, uint32_t len = 0);
uint32_t getLenShortWcharStr(const WCHAR_T* Source);
static AppCapabilities g_capabilities = eAppCapabilitiesInvalid;
static WcharWrapper s_names(g_kClassNames);

char* WCHAR_2_CHAR(wchar_t* in_str);
wchar_t* CHAR_2_WCHAR(char* in_str);







// Функция для конвертации std::wstring в std::string (UTF-8)
std::string wstring_to_utf8(const std::wstring& wstr) {
    std::string utf8_str;
    for (size_t i = 0; i < wstr.size(); ++i) {
        wchar_t wc = wstr[i];
        if (wc <= 0x7F) {
            // ASCII символы
            utf8_str.push_back(static_cast<char>(wc));
        }
        else if (wc <= 0x7FF) {
            // Двухбайтовые символы
            utf8_str.push_back(0xC0 | (wc >> 6));
            utf8_str.push_back(0x80 | (wc & 0x3F));
        }
        else if (wc <= 0xFFFF) {
            // Трехбайтовые символы
            utf8_str.push_back(0xE0 | (wc >> 12));
            utf8_str.push_back(0x80 | ((wc >> 6) & 0x3F));
            utf8_str.push_back(0x80 | (wc & 0x3F));
        }
        else {
            // Четырехбайтовые символы
            utf8_str.push_back(0xF0 | (wc >> 18));
            utf8_str.push_back(0x80 | ((wc >> 12) & 0x3F));
            utf8_str.push_back(0x80 | ((wc >> 6) & 0x3F));
            utf8_str.push_back(0x80 | (wc & 0x3F));
        }
    }
    return utf8_str;
}

///////////////////////////////////////////////////////////////////////////////
// Общие хелперы для обмена с терминалами (Kaspi Smart POS v2, PAX: BCC/Halyk)

// UTF-8 -> wstring (тексты чеков терминала приходят в UTF-8)
static std::wstring Utf8ToWstring(const std::string& str)
{
    if (str.empty())
        return std::wstring();

    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    if (sizeNeeded <= 0)
        return std::wstring(str.begin(), str.end());

    std::wstring result(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &result[0], sizeNeeded);
    return result;
}

// Символ тенге заменяем на слово (не все кассы печатают "₸")
static std::wstring FixTengeSymbol(const std::wstring& s)
{
    const wchar_t tenge = L'\u20B8';
    std::wstring result = s;
    size_t pos = 0;
    while ((pos = result.find(tenge, pos)) != std::wstring::npos) {
        result.replace(pos, 1, L"тенге");
        pos += 5;
    }
    return result;
}

// Результат HTTP-запроса к терминалу
struct HttpResult
{
    bool ok = false;            // ответ получен
    DWORD lastError = 0;        // код WinHTTP при ошибке
    long httpStatus = 0;
    std::string body;
};

// Единая точка HTTP-обмена с терминалом (plain HTTP).
// verb: L"GET" или L"POST"; body пуст для GET; extraHeaders добавляются перед Content-Type.
static HttpResult HttpJsonRequest(const std::wstring& ip, int port,
    const wchar_t* verb, const std::wstring& path,
    const std::wstring& extraHeaders, const std::string& body,
    DWORD timeoutMs)
{
    HttpResult result;

    HINTERNET hSession = WinHttpOpen(L"InnovaNative/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) {
        result.lastError = GetLastError();
        return result;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, ip.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) {
        result.lastError = GetLastError();
        WinHttpCloseHandle(hSession);
        return result;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, verb, path.c_str(), NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        result.lastError = GetLastError();
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    // Таймауты, чтобы касса не зависала на недоступном терминале
    WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

    std::wstring headers = extraHeaders;
    if (!body.empty() && headers.find(L"Content-Type") == std::wstring::npos)
        headers += L"Content-Type: application/json\r\n";

    LPVOID dataPtr = body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.c_str();
    DWORD dataLen = (DWORD)body.size();

    BOOL bResult = WinHttpSendRequest(hRequest,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : (DWORD)-1L,
        dataPtr, dataLen, dataLen, 0);

    if (bResult)
        bResult = WinHttpReceiveResponse(hRequest, NULL);

    if (!bResult) {
        result.lastError = GetLastError();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    DWORD statusCode = 0, statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    result.httpStatus = (long)statusCode;

    // Полное чтение ответа: крупный чек приходит частями
    for (;;) {
        DWORD dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0)
            break;
        std::vector<char> buffer(dwSize + 1, 0);
        DWORD dwDownloaded = 0;
        if (!WinHttpReadData(hRequest, (LPVOID)buffer.data(), dwSize, &dwDownloaded) || dwDownloaded == 0)
            break;
        result.body.append(buffer.data(), dwDownloaded);
    }

    result.ok = true;
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

///////////////////////////////////////////////////////////////////////////////
// Состояние драйвера: токены Kaspi и реквизиты последней операции.
// Хранится в %LOCALAPPDATA%\AcquiringTerminalNative\state.json — каталог
// создаётся автоматически, ручной подготовки не требует.

static std::wstring StateFilePath()
{
    wchar_t buf[MAX_PATH + 1] = { 0 };
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base;
    if (len > 0 && len <= MAX_PATH)
        base = buf;
    else {
        len = GetEnvironmentVariableW(L"TEMP", buf, MAX_PATH);
        base = (len > 0 && len <= MAX_PATH) ? buf : L".";
    }
    return base + L"\\AcquiringTerminalNative\\state.json";
}

static void EnsureDirectory(const std::wstring& dir)
{
    // Создаёт путь по уровням (уже существующие уровни вернут ERROR_ALREADY_EXISTS)
    std::wstring cur;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\' || dir[i] == L'/') {
            // пропускаем корень диска ("C:" / "C:\")
            bool isDriveRoot = !cur.empty() && cur.size() <= 3 && cur[1] == L':';
            if (!cur.empty() && !isDriveRoot) {
                if (!CreateDirectoryW(cur.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                    return;
            }
        }
        if (i < dir.size())
            cur += dir[i];
    }
}

static std::string StateGetString(const std::string& key, const std::string& defaultValue = "")
{
    try {
        std::ifstream in(StateFilePath());
        if (in.is_open()) {
            nlohmann::json j;
            in >> j;
            in.close();
            return j.value(key, defaultValue);
        }
    }
    catch (...) {}
    return defaultValue;
}

static bool StateSetString(const std::string& key, const std::string& value)
{
    try {
        std::wstring path = StateFilePath();
        std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
        EnsureDirectory(dir);

        nlohmann::json j;
        std::ifstream in(path);
        if (in.is_open()) {
            in >> j;
            in.close();
        }
        j[key] = value;
        std::ofstream out(path);
        if (!out.is_open())
            return false;
        out << j.dump(4);
        out.close();
        return true;
    }
    catch (...) {
        return false;
    }
}

static bool FlagIsTrue(const std::string& value)
{
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    return (v == "true" || v == "1" || v == "да");
}

///////////////////////////////////////////////////////////////////////////////
// Настройки из 1С: разбор XML параметров подключения
// (формат <Settings><Parameter Name="..." Value="..."/>...</Settings>)

static std::map<std::wstring, std::wstring> ParseSettingsXml(const std::wstring& xml)
{
    std::map<std::wstring, std::wstring> result;
    if (xml.find(L'<') == std::wstring::npos)
        return result;

    // Идём по элементам: внутри каждого ищем атрибуты Name="..." и Value="..."
    size_t pos = 0;
    while (pos < xml.size()) {
        size_t elemStart = xml.find(L'<', pos);
        if (elemStart == std::wstring::npos)
            break;
        size_t elemEnd = xml.find(L'>', elemStart);
        if (elemEnd == std::wstring::npos)
            break;
        std::wstring elem = xml.substr(elemStart + 1, elemEnd - elemStart - 1);

        auto getAttr = [&elem](const wchar_t* attrName) -> std::wstring {
            std::wstring needle = std::wstring(attrName) + L"=\"";
            size_t a = elem.find(needle);
            if (a == std::wstring::npos)
                return L"";
            a += needle.size();
            size_t b = elem.find(L'"', a);
            if (b == std::wstring::npos)
                return L"";
            return elem.substr(a, b - a);
        };

        std::wstring name = getAttr(L"Name");
        std::wstring value = getAttr(L"Value");
        if (!name.empty() && !value.empty())
            result[name] = value;

        pos = elemEnd + 1;
    }
    return result;
}

//---------------------------------------------------------------------------//
// Значение настройки, переданной из 1С (пустое значение = по умолчанию)
std::wstring innNative::GetSetting(const wchar_t* name, const wchar_t* defaultValue) const
{
    auto it = m_params.find(name);
    if (it != m_params.end() && !it->second.empty())
        return it->second;
    return defaultValue;
}

//---------------------------------------------------------------------------//
// Параметры терминала из настроек 1С
bool innNative::LoadTerminalConfig(TerminalConfig& out) const
{
    out.terminalType = wstring_to_utf8(GetSetting(L"TerminalType", L"Kaspi"));
    out.ip = GetSetting(L"IpAddress", L"127.0.0.1");

    try {
        out.port = std::stoi(GetSetting(L"Port", L"8080"));
    }
    catch (...) {
        out.port = 8080;
    }

    // Kaspi Smart POS
    out.kaspiOwnCheque = FlagIsTrue(wstring_to_utf8(GetSetting(L"KaspiOwnCheque", L"true")));
    try {
        out.kaspiTimeoutSec = std::stoi(GetSetting(L"KaspiTimeoutSec", L"240"));
    }
    catch (...) {
        out.kaspiTimeoutSec = 240;
    }
    if (out.kaspiTimeoutSec <= 0)
        out.kaspiTimeoutSec = 240;
    out.kaspiRefundMethod = wstring_to_utf8(GetSetting(L"KaspiRefundMethod", L"card"));

    // BCC (PAX)
    try {
        out.bccRefundMethod = std::stoi(GetSetting(L"BCCRefundMethod", L"0"));
    }
    catch (...) {
        out.bccRefundMethod = 0;
    }
    if (out.bccRefundMethod < 0 || out.bccRefundMethod > 2)
        out.bccRefundMethod = 0;

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// Протокол PAX (BCC, Halyk): POST / {"task": "...", "data": {...}} — синхронный ответ

static bool PaxRequest(const TerminalConfig& cfg, const std::string& task, const json& taskData,
    json& dataOut, std::wstring& errorText, DWORD timeoutMs)
{
    json req = { {"task", task} };
    if (!taskData.empty())
        req["data"] = taskData;

    HttpResult r = HttpJsonRequest(cfg.ip, cfg.port, L"POST", L"/", L"", req.dump(), timeoutMs);
    if (!r.ok) {
        errorText = L"Нет связи с терминалом (ошибка " + std::to_wstring(r.lastError) + L")";
        return false;
    }

    try {
        json resp = json::parse(r.body);
        if (!resp.contains("data") || resp["data"].is_null()) {
            errorText = L"Некорректный ответ терминала";
            return false;
        }
        dataOut = resp["data"];
        std::string resultCode = dataOut.value("result", "-1");
        if (resultCode != "0") {
            std::string msg = dataOut.value("msg", "");
            errorText = L"Терминал вернул ошибку: " +
                Utf8ToWstring(msg.empty() ? resultCode : msg);
            return false;
        }
        return true;
    }
    catch (...) {
        errorText = L"Ошибка разбора ответа терминала";
        return false;
    }
}

///////////////////////////////////////////////////////////////////////////////
// Kaspi Smart POS v2: GET /v2/*, асинхронный статус.
// Регистрация кассы и токены не используются: «Защита интеграции» на терминале
// должна быть выключена (Панель администратора Smart POS).

// GET к терминалу Kaspi; terminalId добавляется в заголовок, если известен
static HttpResult KaspiGet(const TerminalConfig& cfg, const std::wstring& path,
    const std::wstring& terminalId, DWORD timeoutMs)
{
    std::wstring headers;
    if (!terminalId.empty())
        headers += L"terminalId: " + terminalId + L"\r\n";
    return HttpJsonRequest(cfg.ip, cfg.port, L"GET", path, headers, "", timeoutMs);
}

// Проверка связи: deviceinfo; заодно сохраняет KaspiTerminalId (нужен для /v2/status)
static bool KaspiPing(const TerminalConfig& cfg, std::wstring& info, std::wstring& errorText)
{
    info.clear();

    HttpResult r = KaspiGet(cfg, L"/v2/deviceinfo", L"", 15000);
    if (!r.ok) {
        errorText = L"Нет связи с терминалом Kaspi (ошибка " + std::to_wstring(r.lastError) + L")";
        return false;
    }

    try {
        json resp = json::parse(r.body);
        if (resp.value("statusCode", -1) == 0 && resp.contains("data") && !resp["data"].is_null()) {
            std::string terminalId = resp["data"].value("terminalId", "");
            std::string serialNum = resp["data"].value("serialNum", "");
            std::string posNum = resp["data"].value("posNum", "");

            if (!terminalId.empty())
                StateSetString("KaspiTerminalId", terminalId);

            info = L"Kaspi Smart POS";
            if (!terminalId.empty()) info += L", terminalId: " + Utf8ToWstring(terminalId);
            if (!serialNum.empty()) info += L", serial: " + Utf8ToWstring(serialNum);
            if (!posNum.empty()) info += L", POS: " + Utf8ToWstring(posNum);
            return true;
        }
        // 401 без токена — на терминале включена защита интеграции
        errorText = L"Терминал Kaspi отклонил запрос deviceinfo: " +
            Utf8ToWstring(resp.value("errorText", "")) +
            L" (если на терминале включена «Защита интеграции», выключите её)";
        return false;
    }
    catch (...) {
        errorText = L"Ошибка разбора ответа deviceinfo Kaspi (если на терминале включена «Защита интеграции», выключите её)";
        return false;
    }
}

// Опрос статуса операции до конечного результата (success/fail) с актуализацией unknown
static bool ProcessKaspiOperation(const TerminalConfig& cfg,
    const std::wstring& processId, int timeoutSec,
    std::wstring& finalStatus, std::string& dataDump, std::wstring& errorText)
{
    const std::wstring terminalId = Utf8ToWstring(StateGetString("KaspiTerminalId", ""));
    const std::wstring statusPath = L"/v2/status?processId=" + processId;
    const DWORD started = GetTickCount();
    time_t lastActualize = 0;

    finalStatus.clear();
    dataDump.clear();

    for (;;) {
        HttpResult r = KaspiGet(cfg, statusPath, terminalId, 10000);
        bool protocolError = false;
        std::wstring protocolErrorText;

        if (r.ok) {
            try {
                json resp = json::parse(r.body);
                if (resp.contains("data") && !resp["data"].is_null()) {
                    json data = resp["data"];
                    std::string status = data.value("status", "");

                    if (status == "success") {
                        finalStatus = L"success";
                        dataDump = data.dump();
                        return true;
                    }
                    if (status == "fail") {
                        finalStatus = L"fail";
                        errorText = L"Операция отклонена терминалом Kaspi: " +
                            Utf8ToWstring(data.value("message", ""));
                        return false;
                    }
                    if (status == "unknown") {
                        // Актуализация не чаще раза в 10 секунд
                        time_t now = time(NULL);
                        if (now - lastActualize >= 10) {
                            lastActualize = now;
                            KaspiGet(cfg, L"/v2/actualize?processId=" + processId, L"", 15000);
                        }
                    }
                    // "wait" — продолжаем опрос
                }
                else if (resp.value("statusCode", 0) != 0) {
                    // Ошибка протокола (процесс не найден и т.п.)
                    protocolError = true;
                    protocolErrorText = L"Ошибка опроса статуса Kaspi: " +
                        Utf8ToWstring(resp.value("errorText", ""));
                }
            }
            catch (...) {
                // Нечитаемый ответ — считаем временной проблемой
            }
        }
        // Если запрос не прошёл (r.ok == false) — продолжаем опрос до общего таймаута

        if (protocolError) {
            errorText = protocolErrorText;
            finalStatus = L"fail";
            return false;
        }

        if (GetTickCount() - started >= (DWORD)timeoutSec * 1000) {
            finalStatus = L"timeout";
            errorText = L"Превышено время ожидания операции терминала Kaspi";
            return false;
        }
        Sleep(1000);
    }
}

// Текст чека из chequeInfo ответа Kaspi
static std::wstring BuildKaspiSlipText(const json& data, bool isRefund)
{
    json cheque = json::object();
    if (data.contains("chequeInfo") && !data["chequeInfo"].is_null() && data["chequeInfo"].is_object())
        cheque = data["chequeInfo"];

    auto field = [&cheque](const char* key) -> std::wstring {
        if (!cheque.contains(key) || cheque[key].is_null())
            return L"";
        if (cheque[key].is_string())
            return Utf8ToWstring(cheque[key].get<std::string>());
        if (cheque[key].is_number_integer())
            return std::to_wstring(cheque[key].get<long long>());
        return Utf8ToWstring(cheque[key].dump());
    };

    std::wstring slip = L"================================\n";
    slip += isRefund ? L"          ВОЗВРАТ ОПЛАТЫ\n" : L"              ОПЛАТА\n";
    slip += L"================================\n";

    auto add = [&slip](const wchar_t* caption, const std::wstring& value) {
        if (!value.empty())
            slip += std::wstring(caption) + L": " + value + L"\n";
    };

    add(L"Магазин", field("storeName"));
    add(L"Город", field("city"));
    add(L"Адрес", field("address"));
    add(L"БИН", field("bin"));
    add(L"Терминал", field("terminalId"));
    add(L"Дата", field("date"));
    add(L"Сумма", field("amount").empty() ? L"" : field("amount") + L" тенге");
    add(L"Способ", field("method"));
    add(L"Заказ", field("orderNumber"));
    add(L"Карта", field("cardMask"));
    add(L"RRN", field("rrn"));
    add(L"Код авторизации", field("authorizationCode"));
    add(L"Транзакция", Utf8ToWstring(data.value("transactionId", "")));
    add(L"Статус", field("status"));

    slip += L"================================\n";
    return FixTengeSymbol(slip);
}

// Проверка связи с терминалом по его типу (echo у PAX, deviceinfo у Kaspi)
static bool PingTerminal(const TerminalConfig& cfg, std::wstring& info, std::wstring& errorText, DWORD timeoutMs = 10000)
{
    info.clear();

    if (cfg.terminalType == "Kaspi")
        return KaspiPing(cfg, info, errorText);

    if (cfg.terminalType == "BCC" || cfg.terminalType == "Halyk") {
        json dataOut;
        if (!PaxRequest(cfg, "echo", json::object(), dataOut, errorText, timeoutMs))
            return false;
        std::string msg = dataOut.value("msg", "");
        std::string terminalId = dataOut.value("terminalId", "");
        info = L"PAX терминал";
        if (!terminalId.empty())
            info += L", terminalId: " + Utf8ToWstring(terminalId);
        if (!msg.empty())
            info += L" (" + Utf8ToWstring(msg) + L")";
        return true;
    }

    errorText = L"Неизвестный тип терминала";
    return false;
}

// Функция записи данных в файл








//---------------------------------------------------------------------------//
long GetClassObject(const WCHAR_T* wsName, IComponentBase** pInterface)
{
    if (!*pInterface)
    {
        *pInterface = new innNative;
        return (long)*pInterface;
    }
    return 0;
}
//---------------------------------------------------------------------------//
AppCapabilities SetPlatformCapabilities(const AppCapabilities capabilities)
{
    g_capabilities = capabilities;
    return eAppCapabilitiesLast;
}
//---------------------------------------------------------------------------//
long DestroyObject(IComponentBase** pIntf)
{
    if (!*pIntf)
        return -1;

    delete* pIntf;
    *pIntf = 0;
    return 0;
}
//---------------------------------------------------------------------------//
const WCHAR_T* GetClassNames()
{
    return s_names;
}
//---------------------------------------------------------------------------//
// innNative
//---------------------------------------------------------------------------//
innNative::innNative()
{
    m_iMemory = 0;
    m_iConnect = 0;
}
//---------------------------------------------------------------------------//
innNative::~innNative()
{
}
//---------------------------------------------------------------------------//
bool innNative::Init(void* pConnection)
{
    m_iConnect = (IAddInDefBase*)pConnection;
    m_iConnect->SetEventBufferDepth(100);
    return m_iConnect != NULL;
}
//---------------------------------------------------------------------------//
long innNative::GetInfo()
{
    // Component should put supported component technology version 
    // This component supports 2.0 version
    return 2005;
}
bool innNative::GetInterfaceRevision(tVariant* retVal)
{


    // Устанавливаем результат операции сразу в retVal
    TV_VT(retVal) = VTYPE_I4;
    TV_I4(retVal) = 2005;  // Возвращаем версию 3001

    return true;
}

//---------------------------------------------------------------------------//
void innNative::Done()
{

}
/////////////////////////////////////////////////////////////////////////////
// ILanguageExtenderBase
//---------------------------------------------------------------------------//
bool innNative::RegisterExtensionAs(WCHAR_T** wsExtensionName)
{
    const wchar_t* wsExtension = extensionName;
    size_t iActualSize = ::wcslen(wsExtension) + 1;
    WCHAR_T* dest = 0;

    if (m_iMemory)
    {
        if (m_iMemory->AllocMemory((void**)wsExtensionName, iActualSize * sizeof(WCHAR_T)))
            ::convToShortWchar(wsExtensionName, wsExtension, iActualSize);
        return true;
    }

    return false;
}
//---------------------------------------------------------------------------//
long innNative::GetNProps()
{
    return epLast;
}
//---------------------------------------------------------------------------//
long innNative::FindProp(const WCHAR_T* wsPropName)
{
    long plPropNum = -1;
    wchar_t* propName = 0;

    ::convFromShortWchar(&propName, wsPropName);
    plPropNum = findName(g_PropNames, propName, epLast);

    if (plPropNum == -1)
        plPropNum = findName(g_PropNamesRu, propName, epLast);

    delete[] propName;

    return plPropNum;
}
//---------------------------------------------------------------------------//
const WCHAR_T* innNative::GetPropName(long lPropNum, long lPropAlias)
{
    if (lPropNum >= epLast)
        return NULL;

    wchar_t* wsCurrentName = NULL;
    WCHAR_T* wsPropName = NULL;
    int iActualSize = 0;

    switch (lPropAlias)
    {
    case 0: // First language
        wsCurrentName = (wchar_t*)g_PropNames[lPropNum];
        break;
    case 1: // Second language
        wsCurrentName = (wchar_t*)g_PropNamesRu[lPropNum];
        break;
    default:
        return 0;
    }

    iActualSize = wcslen(wsCurrentName) + 1;

    if (m_iMemory && wsCurrentName)
    {
        if (m_iMemory->AllocMemory((void**)&wsPropName, iActualSize * sizeof(WCHAR_T)))
            ::convToShortWchar(&wsPropName, wsCurrentName, iActualSize);
    }

    return wsPropName;
}
//---------------------------------------------------------------------------//
bool innNative::GetPropVal(const long lPropNum, tVariant* pvarPropVal)
{
    switch (lPropNum)
    {
    case epVersion:
    {
        setWStringToTVariant(pvarPropVal, extensionVersion);
        return true;
    }
    default:
        return false;
    }

    return true;
}
//---------------------------------------------------------------------------//
bool innNative::SetPropVal(const long lPropNum, tVariant* varPropVal)
{
    switch (lPropNum)
    {
    default:
        return false;
    }

    return true;
}
//---------------------------------------------------------------------------//
bool innNative::IsPropReadable(const long lPropNum)
{
    switch (lPropNum)
    {
    case epVersion:
        return true;
    default:
        return false;
    }

    return false;
}
//---------------------------------------------------------------------------//
bool innNative::IsPropWritable(const long lPropNum)
{
    switch (lPropNum)
    {
    case epVersion:
        return false;
    default:
        return false;
    }

    return false;
}
//---------------------------------------------------------------------------//
long innNative::GetNMethods()
{
    return emLast;
}
//---------------------------------------------------------------------------//
long innNative::FindMethod(const WCHAR_T* wsMethodName)
{
    long plMethodNum = -1;
    wchar_t* name = 0;

    ::convFromShortWchar(&name, wsMethodName);

    plMethodNum = findName(g_MethodNames, name, emLast);

    if (plMethodNum == -1)
        plMethodNum = findName(g_MethodNamesRu, name, emLast);

    delete[] name;

    return plMethodNum;
}
//---------------------------------------------------------------------------//
const WCHAR_T* innNative::GetMethodName(const long lMethodNum, const long lMethodAlias)
{
    if (lMethodNum >= emLast)
        return NULL;

    wchar_t* wsCurrentName = NULL;
    WCHAR_T* wsMethodName = NULL;
    int iActualSize = 0;

    switch (lMethodAlias)
    {
    case 0: // First language
        wsCurrentName = (wchar_t*)g_MethodNames[lMethodNum];
        break;
    case 1: // Second language
        wsCurrentName = (wchar_t*)g_MethodNamesRu[lMethodNum];
        break;
    default:
        return 0;
    }

    iActualSize = wcslen(wsCurrentName) + 1;

    if (m_iMemory && wsCurrentName)
    {
        if (m_iMemory->AllocMemory((void**)&wsMethodName, iActualSize * sizeof(WCHAR_T)))
            ::convToShortWchar(&wsMethodName, wsCurrentName, iActualSize);
    }

    return wsMethodName;
}
//---------------------------------------------------------------------------//
long innNative::GetNParams(const long lMethodNum)
{
    switch (lMethodNum)
    {
    case emGetVersion:
        return 0;
    case emGetDescription:
        return 7;
    case emGetLastError:
        return 1;
    case emGetParameters:
        return 1;
    case emSetParameter:
        return 2;
    case emOpen:
        return 1;
    case emClose:
        return 1;
    case emDeviceTest:
        return 2;
    case emGetAdditionalActions:
        return 1;
    case emDoAdditionalAction:
        return 1;
    case emPayByPaymentCard:
        return 7; // deviceID, cardNumber, amount, receiptNumber, rrnCode, authorizationCode, slip
    case emCancelPaymentByPaymentCard:
        return 7;
    case emSettlement:
        return 2;
    case emConnectEquipment:
        return 3; // deviceID (out), equipmentType, connectionParams

    default:
        return 0;
    }

    return 0;
}


bool innNative::Settlement(tVariant* retVal, tVariant* deviceID, tVariant* slip) {
    try {
        if (!deviceID) {
            addError(1, L"Settlement", L"Неверные входные параметры", -1);
            return false;
        }
        try {
            SYSTEMTIME st;
            GetLocalTime(&st);

            wchar_t fileName[512];
            std::wstring statePath = StateFilePath();
            std::wstring stateDir = statePath.substr(0, statePath.find_last_of(L"\\/"));
            EnsureDirectory(stateDir);
            swprintf(fileName, 512, L"%s\\SettlementLog_%04d%02d%02d_%02d%02d%02d.txt",
                stateDir.c_str(),
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

            std::wofstream logFile(fileName);
            if (logFile.is_open()) {
                logFile << L"Settlement вызвана в: "
                    << st.wDay << L"." << st.wMonth << L"." << st.wYear << L" "
                    << st.wHour << L":" << st.wMinute << L":" << st.wSecond << std::endl;
                logFile.close();
            }
        }
        catch (...) {
            addError(1, L"Settlement", L"Не удалось создать лог-файл в папке config", -1);
            return false;
        }

        // Чтение конфигурации
        TerminalConfig cfg;
        if (!LoadTerminalConfig(cfg)) {
            addError(1, L"Settlement", L"Ошибка чтения конфигурации из C:\\config\\parameters.json", -1);
            return false;
        }
        const std::string& terminalType = cfg.terminalType;

        // === PAX: BCC, Halyk — команда settle ===
        if (terminalType == "BCC" || terminalType == "Halyk") {
            json taskData;
            taskData["printFirstReceipt"] = "true";

            json dataOut;
            std::wstring errorText;
            if (!PaxRequest(cfg, "settle", taskData, dataOut, errorText, 180000)) {
                addError(1, L"Settlement", errorText.c_str(), -1);
                if (retVal) {
                    TV_VT(retVal) = VTYPE_BOOL;
                    TV_BOOL(retVal) = false;
                }
                return false;
            }

            // Чек сверки возвращаем 1С через out-параметр slip
            std::string receipt = dataOut.value("receiptInfo", "");
            if (!receipt.empty())
                setWStringToTVariant(slip, FixTengeSymbol(Utf8ToWstring(receipt)).c_str());

            if (retVal) {
                TV_VT(retVal) = VTYPE_BOOL;
                TV_BOOL(retVal) = true;
            }
            return true;
        }

        // === Kaspi: сверка выполняется на терминале, в протоколе v2 её нет ===
        if (terminalType == "Kaspi") {
            std::wstring notice =
                L"Сверка итогов (Z-отчет) для Kaspi Smart POS\n"
                L"формируется на терминале: Панель администратора.\n"
                L"Выполните сверку на устройстве и снимите отчет.";

            setWStringToTVariant(slip, notice.c_str());

            if (retVal) {
                TV_VT(retVal) = VTYPE_BOOL;
                TV_BOOL(retVal) = true;
            }
            return true;
        }

        // Неизвестный тип
        addError(1, L"Settlement", L"Неизвестный тип терминала", -1);
        return false;

    }
    catch (...) {
        addError(2, L"Settlement", L"Неожиданная ошибка", -1);
        return false;
    }
}





//---------------------------------------------------------------------------//
// Запуск операции Kaspi (payment/refund) и опрос статуса до конечного результата
bool innNative::KaspiRunOperation(const TerminalConfig& cfg, const std::wstring& startPath,
    bool isRefund, const wchar_t* source, tVariant* slip, tVariant* retVal)
{
    auto fail = [&](const std::wstring& errorText) {
        addError(1, source, errorText.c_str(), -1);
        if (retVal) {
            TV_VT(retVal) = VTYPE_BOOL;
            TV_BOOL(retVal) = false;
        }
        return false;
    };

    std::wstring errorText;

    // Проверка связи и получение terminalId (сохраняется в state.json)
    std::wstring info;
    if (!KaspiPing(cfg, info, errorText))
        return fail(errorText);

    // Старт операции
    HttpResult r = KaspiGet(cfg, startPath, L"", 15000);
    if (!r.ok) {
        wchar_t msg[256];
        swprintf(msg, 256, L"Нет связи с терминалом Kaspi (ошибка %lu)", r.lastError);
        return fail(msg);
    }

    std::string processId;
    try {
        json resp = json::parse(r.body);
        if (resp.value("statusCode", -1) == 0 && resp.contains("data") && !resp["data"].is_null())
            processId = resp["data"].value("processId", "");
        else
            errorText = L"Терминал Kaspi отклонил операцию: " + Utf8ToWstring(resp.value("errorText", ""));
    }
    catch (...) {
        errorText = L"Ошибка разбора ответа Kaspi";
    }
    if (processId.empty())
        return fail(errorText.empty() ? std::wstring(L"Терминал Kaspi не вернул processId") : errorText);

    int timeoutSec = cfg.kaspiTimeoutSec;

    std::wstring finalStatus;
    std::string dataDump;
    if (!ProcessKaspiOperation(cfg, Utf8ToWstring(processId), timeoutSec, finalStatus, dataDump, errorText))
        return fail(errorText);

    // Успех: чек и реквизиты транзакции для последующего возврата
    try {
        json data = json::parse(dataDump);
        if (slip)
            setWStringToTVariant(slip, BuildKaspiSlipText(data, isRefund).c_str());

        if (!isRefund) {
            json cheque = json::object();
            if (data.contains("chequeInfo") && data["chequeInfo"].is_object())
                cheque = data["chequeInfo"];
            std::string method = cheque.value("method", "");
            if (method.empty())
                method = cheque.value("rrn", "").empty() ? "qr" : "card";
            StateSetString("LastKaspiMethod", method);
            StateSetString("LastKaspiOrderNumber", cheque.value("orderNumber", ""));
            StateSetString("LastKaspiRRN", cheque.value("rrn", ""));
            StateSetString("LastKaspiTransactionId", data.value("transactionId", ""));
        }
    }
    catch (...) {}

    if (retVal) {
        TV_VT(retVal) = VTYPE_BOOL;
        TV_BOOL(retVal) = true;
    }
    return true;
}

//---------------------------------------------------------------------------//
bool innNative::PayByPaymentCard(tVariant* retVal, tVariant* deviceID, tVariant* cardNumber,
    tVariant* amount, tVariant* receiptNumber, tVariant* rrnCode,
    tVariant* authorizationCode, tVariant* slip)
{
    try {
        if (!deviceID || !cardNumber || !amount || !receiptNumber || !rrnCode || !authorizationCode || !slip) {
            addError(1, L"PayByPaymentCard", L"Неверные входные параметры", -1);
            return false;
        }

        // Чтение конфигурации
        TerminalConfig cfg;
        if (!LoadTerminalConfig(cfg)) {
            addError(1, L"PayByPaymentCard", L"Ошибка чтения конфигурации из C:\\config\\parameters.json", -1);
            return false;
        }
        const std::string& terminalType = cfg.terminalType;

        // Получение суммы
        int paymentAmount = 0;
        if (TV_VT(amount) == VTYPE_R8)
            paymentAmount = static_cast<int>(TV_R8(amount));
        else if (TV_VT(amount) == VTYPE_I4)
            paymentAmount = TV_I4(amount);
        else {
            addError(1, L"PayByPaymentCard", L"Неверный тип параметра amount", -1);
            return false;
        }

        // === KASPI ===
        if (terminalType == "Kaspi") {
            wchar_t amountStr[32];
            swprintf(amountStr, 32, L"%d", paymentAmount);

            std::wstring ownCheque = cfg.kaspiOwnCheque ? L"true" : L"false";
            std::wstring startPath = std::wstring(L"/v2/payment?amount=") + amountStr + L"&owncheque=" + ownCheque;

            // Старт оплаты, опрос статуса, чек и сохранение реквизитов транзакции
            return KaspiRunOperation(cfg, startPath, false, L"PayByPaymentCard", slip, retVal);
        }

        // === PAX: BCC, Halyk ===
        if (terminalType == "BCC" || terminalType == "Halyk") {
            json taskData;
            taskData["amount"] = std::to_string(paymentAmount);
            if (terminalType == "BCC")
                taskData["printFirstReceipt"] = "true";
            else
                taskData["isPrintFirst"] = "true";

            json dataOut;
            std::wstring errorText;
            if (!PaxRequest(cfg, "purchase", taskData, dataOut, errorText, 180000)) {
                addError(1, L"PayByPaymentCard", errorText.c_str(), -1);
                if (retVal) {
                    TV_VT(retVal) = VTYPE_BOOL;
                    TV_BOOL(retVal) = false;
                }
                return false;
            }

            // Чек терминала возвращаем 1С через out-параметр slip
            std::string receipt = dataOut.value("receiptInfo", "");
            if (!receipt.empty())
                setWStringToTVariant(slip, FixTengeSymbol(Utf8ToWstring(receipt)).c_str());

            if (retVal) {
                TV_VT(retVal) = VTYPE_BOOL;
                TV_BOOL(retVal) = true;
            }
            return true;
        }

        else {
            addError(1, L"PayByPaymentCard", L"Неизвестный тип терминала", -1);
            return false;
        }

    }
    catch (...) {
        addError(2, L"PayByPaymentCard", L"Неожиданная ошибка", -1);
        return false;
    }
}



bool innNative::CancelPaymentByPaymentCard(tVariant* retVal, tVariant* deviceID, tVariant* cardNumber, tVariant* amount, tVariant* receiptNumber, tVariant* rrnCode, tVariant* authorizationCode, tVariant* slip)
{
    try {
        // Проверка входных параметров
        if (!deviceID || !cardNumber || !amount || !receiptNumber || !rrnCode || !authorizationCode || !slip) {
            addError(1, L"CancelPaymentByPaymentCard", L"Неверные входные параметры", -1);
            return false;
        }

        // RRN исходной операции (обязателен для возврата на карту)
        wchar_t* wRRNCode = nullptr;
        if (TV_VT(rrnCode) == VTYPE_PWSTR) {
            convFromShortWchar(&wRRNCode, TV_WSTR(rrnCode));
        }
        else {
            addError(1, L"CancelPaymentByPaymentCard", L"Неверный тип параметра rrnCode", -1);
            return false;
        }
        std::string rrnCodeStr = wstring_to_utf8(std::wstring(wRRNCode ? wRRNCode : L""));
        delete[] wRRNCode;

        // Чтение конфигурации
        TerminalConfig cfg;
        if (!LoadTerminalConfig(cfg)) {
            addError(1, L"CancelPaymentByPaymentCard", L"Ошибка чтения конфигурации из C:\\config\\parameters.json", -1);
            return false;
        }
        const std::string& terminalType = cfg.terminalType;

        // Сумма возврата
        int cancelAmount = 0;
        if (TV_VT(amount) == VTYPE_R8)
            cancelAmount = static_cast<int>(TV_R8(amount));
        else if (TV_VT(amount) == VTYPE_I4)
            cancelAmount = TV_I4(amount);
        else {
            addError(1, L"CancelPaymentByPaymentCard", L"Неверный тип параметра amount", -1);
            return false;
        }

        // === KASPI ===
        if (terminalType == "Kaspi") {
            // Способ исходной оплаты: сохранённый при оплате или параметр драйвера
            std::string method = StateGetString("LastKaspiMethod", "");
            if (method.empty())
                method = cfg.kaspiRefundMethod;

            // Для карты transactionId = rrn, для qr/alaqan = orderNumber
            std::string transactionId;
            if (method == "card") {
                transactionId = rrnCodeStr;
                if (transactionId.empty())
                    transactionId = StateGetString("LastKaspiRRN", "");
            }
            else {
                transactionId = StateGetString("LastKaspiOrderNumber", "");
            }

            if (transactionId.empty()) {
                addError(1, L"CancelPaymentByPaymentCard", L"Не найден идентификатор исходной транзакции для возврата", -1);
                if (retVal) {
                    TV_VT(retVal) = VTYPE_BOOL;
                    TV_BOOL(retVal) = false;
                }
                return false;
            }

            wchar_t amountStr[32];
            swprintf(amountStr, 32, L"%d", cancelAmount);

            std::wstring ownCheque = cfg.kaspiOwnCheque ? L"true" : L"false";
            std::wstring startPath = L"/v2/refund?method=" + Utf8ToWstring(method) +
                L"&amount=" + amountStr +
                L"&transactionId=" + Utf8ToWstring(transactionId) +
                L"&owncheque=" + ownCheque;

            return KaspiRunOperation(cfg, startPath, true, L"CancelPaymentByPaymentCard", slip, retVal);
        }

        // === PAX: BCC, Halyk ===
        if (terminalType == "BCC" || terminalType == "Halyk") {
            if (rrnCodeStr.empty()) {
                addError(1, L"CancelPaymentByPaymentCard", L"Не указан RRN исходной операции (tagRRN)", -1);
                if (retVal) {
                    TV_VT(retVal) = VTYPE_BOOL;
                    TV_BOOL(retVal) = false;
                }
                return false;
            }

            json taskData;
            taskData["amount"] = std::to_string(cancelAmount);
            taskData["tagRRN"] = rrnCodeStr;
            if (terminalType == "BCC") {
                // method: 0 — возврат на карту, 1 — WeChat QR, 2 — BCC QR
                int refundMethod = cfg.bccRefundMethod;
                taskData["method"] = refundMethod;
                taskData["printFirstReceipt"] = "true";
            }
            else {
                taskData["isPrintFirst"] = "true";
            }

            json dataOut;
            std::wstring errorText;
            if (!PaxRequest(cfg, "refund", taskData, dataOut, errorText, 180000)) {
                addError(1, L"CancelPaymentByPaymentCard", errorText.c_str(), -1);
                if (retVal) {
                    TV_VT(retVal) = VTYPE_BOOL;
                    TV_BOOL(retVal) = false;
                }
                return false;
            }

            std::string receipt = dataOut.value("receiptInfo", "");
            if (!receipt.empty())
                setWStringToTVariant(slip, FixTengeSymbol(Utf8ToWstring(receipt)).c_str());

            if (retVal) {
                TV_VT(retVal) = VTYPE_BOOL;
                TV_BOOL(retVal) = true;
            }
            return true;
        }

        addError(1, L"CancelPaymentByPaymentCard", L"Неизвестный тип терминала", -1);
        if (retVal) {
            TV_VT(retVal) = VTYPE_BOOL;
            TV_BOOL(retVal) = false;
        }
        return false;
    }
    catch (...) {
        addError(2, L"CancelPaymentByPaymentCard", L"Неожиданная ошибка при отмене платежа", -1);
        return false;
    }
}





bool innNative::GetParamDefValue(const long lMethodNum, const long lParamNum, tVariant* pvarParamDefValue)
{
    TV_VT(pvarParamDefValue) = VTYPE_EMPTY;

    switch (lMethodNum)
    {
    default:
        return false;
    }

    return false;
}
//---------------------------------------------------------------------------//
bool innNative::HasRetVal(const long lMethodNum)
{
    switch (lMethodNum)
    {
    case emGetVersion:
        return true;
    case emGetDescription:
        return true;
    case emGetLastError:
        return true;
    case emGetParameters:
        return true;
    case emSetParameter:
        return true;
    case emOpen:
        return true;
    case emClose:
        return true;
    case emDeviceTest:
        return true;
    case emGetAdditionalActions:
        return true;
    case emDoAdditionalAction:
        return true;
    case emPayByPaymentCard:
        return true;
    case emPrintSlipOnTerminal:
        return true;
    case emCancelPaymentByPaymentCard:
        return true;
    case emSettlement:
        return true;
        // метод возвращает значение


    case emConnectEquipment:
        return true;
    default:
        return false;
    }

    return false;
}
//---------------------------------------------------------------------------//
bool innNative::CallAsProc(const long lMethodNum, tVariant* paParams, const long lSizeArray)
{
    switch (lMethodNum)
    {
    default:
        return false;
    }
    return true;
}
//---------------------------------------------------------------------------//
void innNative::setWStringToTVariant(tVariant* dest, const wchar_t* source) {

    size_t len = ::wcslen(source) + 1;

    TV_VT(dest) = VTYPE_PWSTR;

    if (m_iMemory->AllocMemory((void**)&dest->pwstrVal, len * sizeof(WCHAR_T)))
        convToShortWchar(&dest->pwstrVal, source, len);
    dest->wstrLen = ::wcslen(source);
}
//---------------------------------------------------------------------------//
bool innNative::mGetVersion(tVariant* retVal)
{
    if (retVal) {
        setWStringToTVariant(retVal, extensionVersion);
        return true;
    }
    return false;
}
//---------------------------------------------------------------------------//
bool innNative::mGetDescription(tVariant* retVal, tVariant* name, tVariant* description, tVariant* equipmentType,
    tVariant* interfaceRevision, tVariant* integrationLibrary, tVariant* mainDriverInstalled, tVariant* getDownloadURL) {
    // Устанавливаем значения параметров
    setWStringToTVariant(name, L"Эквайринговый терминал (Kaspi/Halyk/BCC)");
    setWStringToTVariant(description, L"Драйвер эквайрингового терминала: Kaspi Smart POS (v2), Halyk (PAX), BCC (PAX)");
    setWStringToTVariant(equipmentType, L"ЭквайринговыйТерминал");

    TV_VT(interfaceRevision) = VTYPE_I4;
    TV_I4(interfaceRevision) = 2005;  // Поддерживаемая версия требований

    TV_VT(integrationLibrary) = VTYPE_BOOL;
    TV_BOOL(integrationLibrary) = false;  // Не является интеграционной библиотекой

    TV_VT(mainDriverInstalled) = VTYPE_BOOL;
    TV_BOOL(mainDriverInstalled) = true;  // Основной драйвер установлен

    setWStringToTVariant(getDownloadURL, L"");  // Пустая строка для URL скачивания
    if (retVal) {
        TV_VT(retVal) = VTYPE_BOOL;
        TV_BOOL(retVal) = true;
        return true;
    }
    return false;
}







//---------------------------------------------------------------------------//
bool innNative::mGetLastError(tVariant* retVal, tVariant* retDescription)
{
    setWStringToTVariant(retDescription, g_lastErrorText.c_str());

    if (retVal) {
        TV_VT(retVal) = VTYPE_I4;
        TV_I4(retVal) = g_lastErrorCode;
        return true;
    }

    return false;
}


bool innNative::ConnectEquipment(tVariant* retVal, tVariant* deviceID, tVariant* equipmentType, tVariant* connectionParams)
{

    // Проверяем, переданы ли параметры
    if (!deviceID || !equipmentType || !connectionParams) {
        return false;
    }

    // Читаем тип оборудования (1С передаёт строкой)
    wchar_t* wType = nullptr;
    if (TV_VT(equipmentType) == VTYPE_PWSTR) {
        convFromShortWchar(&wType, TV_WSTR(equipmentType));
    }
    std::wstring equipType = wType ? wType : L"";
    delete[] wType;

    // Параметры подключения — XML с настройками от 1С
    if (TV_VT(connectionParams) == VTYPE_PWSTR) {
        wchar_t* wParams = nullptr;
        convFromShortWchar(&wParams, TV_WSTR(connectionParams));
        std::wstring wsParams(wParams ? wParams : L"");
        delete[] wParams;
        if (wsParams.find(L'<') != std::wstring::npos) {
            for (const auto& kv : ParseSettingsXml(wsParams))
                m_params[kv.first] = kv.second;
        }
    }

    // Генерируем ID устройства (в реальном коде тут должен быть логический процесс подключения)
    const wchar_t* generatedDeviceID = L"Device123";

    // Записываем сгенерированный ID в параметр deviceID
    setWStringToTVariant(deviceID, generatedDeviceID);

    // Устанавливаем возвращаемое значение в TRUE (подключение успешно)
    if (retVal) {
        TV_VT(retVal) = VTYPE_BOOL;
        TV_BOOL(retVal) = true;
    }

    return true;
}

//---------------------------------------------------------------------------//
bool innNative::mGetParameters(tVariant* retVal, tVariant* xml)
{
    const wchar_t* xmlTerminalParams =
        L"<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
        L"<Settings>"
        L"<Page Caption=\"Параметры\">"
        L"<Group Caption=\"Параметры подключения\">"

        // Параметр выбора терминала (Kaspi, Halyk, BCC)
        L"<Parameter Name=\"TerminalType\" Caption=\"Тип терминала\" TypeValue=\"String\" DefaultValue=\"Kaspi\">"
        L"<ChoiceList>"
        L"<Item Value=\"Kaspi\">Kaspi</Item>"
        L"<Item Value=\"Halyk\">Halyk</Item>"
        L"<Item Value=\"BCC\">BCC (PAX)</Item>"
        L"</ChoiceList>"
        L"</Parameter>"

        // Параметр для ввода IP-адреса
        L"<Parameter Name=\"IpAddress\" Caption=\"IP адрес\" TypeValue=\"String\" DefaultValue=\"192.168.1.1\"/>"

        // Параметр для ввода порта
        L"<Parameter Name=\"Port\" Caption=\"Порт\" TypeValue=\"String\" DefaultValue=\"8080\"/>"

        L"</Group>"
        L"<Group Caption=\"Параметры Kaspi Smart POS\">"

        // true — чек печатает касса, false — чек печатает терминал
        L"<Parameter Name=\"KaspiOwnCheque\" Caption=\"Чек печатает касса\" TypeValue=\"Bool\" DefaultValue=\"true\"/>"

        // Общий таймаут ожидания операции (опрос статуса)
        L"<Parameter Name=\"KaspiTimeoutSec\" Caption=\"Таймаут операции, сек\" TypeValue=\"String\" DefaultValue=\"240\"/>"

        // Способ возврата, если метод исходной оплаты не сохранён
        L"<Parameter Name=\"KaspiRefundMethod\" Caption=\"Способ возврата\" TypeValue=\"String\" DefaultValue=\"card\">"
        L"<ChoiceList>"
        L"<Item Value=\"card\">Карта</Item>"
        L"<Item Value=\"qr\">QR</Item>"
        L"<Item Value=\"alaqan\">Alaqan</Item>"
        L"</ChoiceList>"
        L"</Parameter>"

        L"</Group>"
        L"<Group Caption=\"Параметры BCC (PAX)\">"

        // Способ возврата: 0 — карта, 1 — WeChat QR, 2 — BCC QR
        L"<Parameter Name=\"BCCRefundMethod\" Caption=\"Способ возврата\" TypeValue=\"String\" DefaultValue=\"0\">"
        L"<ChoiceList>"
        L"<Item Value=\"0\">Карта</Item>"
        L"<Item Value=\"1\">WeChat QR</Item>"
        L"<Item Value=\"2\">BCC QR</Item>"
        L"</ChoiceList>"
        L"</Parameter>"

        L"</Group>"
        L"</Page>"
        L"</Settings>";

    // Устанавливаем значение XML в выходной параметр
    setWStringToTVariant(xml, xmlTerminalParams);

    // Устанавливаем возвращаемое значение в TRUE (успешное выполнение)
    if (retVal) {
        TV_VT(retVal) = VTYPE_BOOL;
        TV_BOOL(retVal) = true;
        return true;
    }

    return false;
}





//---------------------------------------------------------------------------//
bool innNative::mSetParameter(tVariant* retVal, tVariant* par, tVariant* val)
{
    // Настройки хранит сама 1С (хранилище подключаемого оборудования);
    // здесь запоминаем их в памяти компоненты до Open
    if (TV_VT(par) == VTYPE_PWSTR && TV_VT(val) == VTYPE_PWSTR) {
        wchar_t* wPar = nullptr;
        wchar_t* wVal = nullptr;

        convFromShortWchar(&wPar, TV_WSTR(par));
        convFromShortWchar(&wVal, TV_WSTR(val));

        std::wstring wsParam(wPar ? wPar : L"");
        std::wstring wsValue(wVal ? wVal : L"");

        delete[] wPar;
        delete[] wVal;

        if (wsValue.find(L'<') != std::wstring::npos) {
            // Значение — XML с набором параметров
            for (const auto& kv : ParseSettingsXml(wsValue))
                m_params[kv.first] = kv.second;
        }
        else if (!wsParam.empty()) {
            m_params[wsParam] = wsValue;
        }
    }

    if (retVal) {
        TV_VT(retVal) = VTYPE_BOOL;
        TV_BOOL(retVal) = true;
    }

    return true;
}





bool innNative::PrintSlipOnTerminal(tVariant* retVal)
{
    // Пример реализации печати чека на терминале
    //const wchar_t* printCommand = L"Печать чека на терминале выполнена успешно.";

    //// Здесь должна быть логика печати чека на терминале
    //// Например, отправка команды на терминал через COM-порт или другой интерфейс

    //// Устанавливаем значение результата в выходной параметр
    if (retVal) {
        TV_VT(retVal) = VTYPE_BOOL;
        TV_BOOL(retVal) = true;
        return true;
    }

    return true;
}




void LogVariant(std::wofstream& file, const tVariant* var)
{
    uint16_t vt = TV_VT(var);
    file << L"  Type: " << vt << L" ";

    switch (vt)
    {
    case 31: // VTYPE_PWSTR
    {
        wchar_t* wstr = TV_WSTR(var);
        if (wstr)
            file << L"Wide string: " << wstr << L"\n";
        else
            file << L"Wide string: nullptr\n";
        break;
    }
    case 22: // VTYPE_STRING UTF-8
    {
        const char* str = TV_STR(var);
        if (str)
        {
            size_t len = strlen(str);
            file << L"String (UTF-8) length: " << len << L" ";

            if (len > 0)
            {
                std::wstring wstr;
                int convLen = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
                if (convLen > 0)
                {
                    wchar_t* buf = new wchar_t[convLen];
                    MultiByteToWideChar(CP_UTF8, 0, str, -1, buf, convLen);
                    wstr = buf;
                    delete[] buf;
                }
                else
                {
                    wstr = L"(не удалось конвертировать)";
                }
                file << L"Content: " << wstr << L"\n";
            }
            else
            {
                file << L"Content is empty\n";
            }
        }
        else
        {
            file << L"String pointer is nullptr\n";
        }
        break;
    }
    case 3: // VT_I4 — int32
    {
        int val = TV_I4(var);
        file << L"Int32: " << val << L"\n";
        break;
    }
    case 11: // VT_BOOL
    {
        bool val = (TV_BOOL(var) != 0);
        file << L"Bool: " << (val ? L"true" : L"false") << L"\n";
        break;
    }
    case 5: // VT_R8 - double
    {
        double val = TV_R8(var);
        file << L"Double: " << val << L"\n";
        break;
    }
    default:
    {
        // Вывести первые 16 байт данных для непонятных типов
        const unsigned char* pData = (const unsigned char*)var;
        std::wstringstream ss;
        ss << L"Unknown type data (first 16 bytes hex): ";
        for (int i = 0; i < 16; ++i)
        {
            ss << std::hex << std::setw(2) << std::setfill(L'0') << (int)pData[i] << L" ";
        }
        file << ss.str() << L"\n";
        break;
    }
    }
}

bool innNative::mOpen(tVariant* retVal, tVariant* par) {

    // Параметр может быть идентификатором устройства или XML с настройками
    if (par && TV_VT(par) == VTYPE_PWSTR) {
        wchar_t* wPar = nullptr;
        convFromShortWchar(&wPar, TV_WSTR(par));
        std::wstring wsPar(wPar ? wPar : L"");
        delete[] wPar;
        if (wsPar.find(L'<') != std::wstring::npos) {
            for (const auto& kv : ParseSettingsXml(wsPar))
                m_params[kv.first] = kv.second;
        }
    }

    // Настройки из 1С, проверка связи с терминалом
    TerminalConfig cfg;
    LoadTerminalConfig(cfg);

    std::wstring info, errorText;
    bool connected = PingTerminal(cfg, info, errorText, 10000);
    if (!connected)
        addError(1, L"Open", errorText.c_str(), -1);

    if (retVal) {
        TV_VT(retVal) = VTYPE_BOOL;
        TV_BOOL(retVal) = connected;
    }
    return connected;
}


//---------------------------------------------------------------------------//
bool innNative::mClose(tVariant* retVal, tVariant* par)
{
    // Активных соединений компонента не держит — просто подтверждаем закрытие
    if (retVal) {
        TV_VT(retVal) = VTYPE_BOOL;
        TV_BOOL(retVal) = true;
    }
    return true;
}
//---------------------------------------------------------------------------//
bool innNative::mDeviceTest(tVariant* retVal, tVariant* description, tVariant* demoModeIsActivated)
{
    // Проверяем связь с терминалом и возвращаем его описание
    TerminalConfig cfg;
    LoadTerminalConfig(cfg);

    std::wstring info, errorText;
    bool connected = PingTerminal(cfg, info, errorText, 10000);
    if (!connected)
        addError(1, L"DeviceTest", errorText.c_str(), -1);

    setWStringToTVariant(description, connected ? info.c_str() : L"Терминал не отвечает");

    if (demoModeIsActivated) {
        TV_VT(demoModeIsActivated) = VTYPE_BOOL;
        TV_BOOL(demoModeIsActivated) = false;
    }

    if (retVal) {
        TV_VT(retVal) = VTYPE_BOOL;
        TV_BOOL(retVal) = connected;
    }
    return connected;
}
//---------------------------------------------------------------------------//
bool innNative::mGetAdditionalActions(tVariant* retVal, tVariant* xml)
{
    // Формируем XML строку с дополнительными действиями
    setWStringToTVariant(xml, L"<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
        L"<Actions>"
        L"<Action Name=\"Action1\" Caption=\"Дополнительное действие 1\"/>"
        L"</Actions>"
    );

    // Устанавливаем значение XML и возвращаем успешный результат
    if (retVal) {
        TV_VT(retVal) = VTYPE_BOOL;
        TV_BOOL(retVal) = true;
        return true;
    }
    return false;
}





bool innNative::TerminalParamters(tVariant* retVal, tVariant* xml)
{
    // XML с параметрами терминала
    const wchar_t* xmlTerminalParams =
        L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        L"<TerminalParameters "
        L"PrintSlipOnTerminal=\"false\" "           // Печать чека на терминале
        L"ShortSlip=\"false\" "                     // Короткий чек
        L"CashWithdrawal=\"false\" "                // Выдача наличных
        L"ElectronicCertificates=\"false\" "        // Электронные сертификаты 
        L"PartialCancellation=\"false\" "           // Частичная отмена
        L"ConsumerPresentedQR=\"false\" "           // QR код покупателя
        L"ListCardTransactions=\"false\"/>";        // Список операций по карте

    // Устанавливаем значение XML в выходной параметр
    setWStringToTVariant(xml, xmlTerminalParams);

    // Возвращаем результат
    if (retVal) {
        TV_VT(retVal) = VTYPE_BOOL;
        TV_BOOL(retVal) = true;
        return true;
    }

    return false;
}



//---------------------------------------------------------------------------//
bool innNative::mDoAdditionalAction(tVariant* retVal, tVariant* actionName)
{
    if (retVal) {
        TV_VT(retVal) = VTYPE_BOOL;
        TV_BOOL(retVal) = true;
        return true;
    }
    return false;
}


bool innNative::pay(tVariant* retVal, tVariant* a, tVariant* b, tVariant* c, tVariant* d, tVariant* e, tVariant* f)
{
    // Настройки запроса
    const wchar_t* server = L"wow.naliv.kz";
    const wchar_t* path = L"/api/payment";
    const wchar_t* postData = L"{\"amount\": 1000, \"currency\": \"KZT\"}";

    DWORD dwSize = wcslen(postData) * sizeof(wchar_t);

    HINTERNET hSession = WinHttpOpen(L"PaymentClient/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, server, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Заголовки
    const wchar_t* headers = L"Content-Type: application/json\r\n";

    // Отправка запроса
    BOOL bResult = WinHttpSendRequest(hRequest, headers, -1, (LPVOID)postData, dwSize, dwSize, 0);
    if (bResult) {
        WinHttpReceiveResponse(hRequest, NULL);
    }

    // Закрытие соединения
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    // Пример данных для XML
    const wchar_t* cardNumber = L"1234567890123456";
    const wchar_t* receiptNumber = L"987654321";
    const wchar_t* rrnCode = L"1234567890";
    const wchar_t* authorizationCode = L"AUTH1234";

    // Формируем XML строку
    std::wstring xmlResponse = L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        L"<PaymentResponse>"
        L"<CardNumber>" + std::wstring(cardNumber) + L"</CardNumber>"
        L"<ReceiptNumber>" + std::wstring(receiptNumber) + L"</ReceiptNumber>"
        L"<RRNCode>" + std::wstring(rrnCode) + L"</RRNCode>"
        L"<AuthorizationCode>" + std::wstring(authorizationCode) + L"</AuthorizationCode>"
        L"</PaymentResponse>";

    // Устанавливаем значение XML в выходной параметр
    setWStringToTVariant(retVal, xmlResponse.c_str());

    return bResult;
}


bool innNative::PrintReceiptsOnTerminal(tVariant* retVal)
{
    if (retVal) {
        TV_VT(retVal) = VTYPE_BOOL;
        TV_BOOL(retVal) = false;
    }
    return false;
}



//---------------------------------------------------------------------------//
bool innNative::CallAsFunc(const long lMethodNum, tVariant* pvarRetValue, tVariant* paParams, const long lSizeArray)
{
    std::wofstream logFile("c:\\driver_log.txt", std::ios::app);
    if (logFile.is_open())
    {
        logFile << L"CallAsFunc called. MethodNum = " << lMethodNum << L", Params count = " << lSizeArray << L"\n";
        for (long i = 0; i < lSizeArray; ++i)
        {
            logFile << L"Param " << i << L":\n";
            LogVariant(logFile, &paParams[i]);
        }
        logFile.flush();
    }

    switch (lMethodNum)
    {
    case emGetVersion:
        return mGetVersion(pvarRetValue);
    case emGetDescription:
        return mGetDescription(pvarRetValue, &paParams[0], &paParams[1], &paParams[2], &paParams[3], &paParams[4], &paParams[5], &paParams[6]);
    case emGetLastError:
        return mGetLastError(pvarRetValue, paParams);
    case emGetParameters:
        return mGetParameters(pvarRetValue, paParams);
    case emSetParameter:
        return mSetParameter(pvarRetValue, paParams, &paParams[1]);
    case emOpen:
        return mOpen(pvarRetValue, paParams);
    case emClose:
        return mClose(pvarRetValue, paParams);
    case emDeviceTest:
        return mDeviceTest(pvarRetValue, paParams, &paParams[1]);
    case emGetAdditionalActions:
        return mGetAdditionalActions(pvarRetValue, paParams);
    case emDoAdditionalAction:
        return mDoAdditionalAction(pvarRetValue, paParams);
    case emPayByPaymentCard:
        return PayByPaymentCard(pvarRetValue, paParams, &paParams[1], &paParams[2], &paParams[3], &paParams[4], &paParams[5], &paParams[6]);
    case emCancelPaymentByPaymentCard:
        return CancelPaymentByPaymentCard(pvarRetValue, &paParams[0], &paParams[1], &paParams[2], &paParams[3], &paParams[4], &paParams[5], &paParams[6]);
    case emPrintSlipOnTerminal:
        return PrintSlipOnTerminal(pvarRetValue);
    case emSettlement:
        return Settlement(pvarRetValue, &paParams[0], &paParams[1]);
    case emConnectEquipment:
        return ConnectEquipment(pvarRetValue, &paParams[0], &paParams[1], &paParams[2]);

    default:
        if (logFile.is_open())
            logFile << L"Unknown method number: " << lMethodNum << L"\n";
        return false;
    }
}




//---------------------------------------------------------------------------//
void innNative::SetLocale(const WCHAR_T* loc)
{
#if !defined( __linux__ ) && !defined(__APPLE__)
    _wsetlocale(LC_ALL, loc);
#endif
}
/////////////////////////////////////////////////////////////////////////////
// LocaleBase
//---------------------------------------------------------------------------//
bool innNative::setMemManager(void* mem)
{
    m_iMemory = (IMemoryManager*)mem;
    return m_iMemory != 0;
}
//---------------------------------------------------------------------------//
void innNative::addError(uint32_t wcode, const wchar_t* source, const wchar_t* descriptor, long code)
{
    // Запоминаем для GetLastError
    g_lastErrorText = descriptor ? descriptor : L"";
    g_lastErrorCode = code;

    if (m_iConnect)
    {
        WCHAR_T* err = 0;
        WCHAR_T* descr = 0;

        ::convToShortWchar(&err, source);
        ::convToShortWchar(&descr, descriptor);

        m_iConnect->AddError(wcode, err, descr, code);
        delete[] err;
        delete[] descr;
    }
}
//---------------------------------------------------------------------------//
long innNative::findName(const wchar_t* names[], const wchar_t* name, const uint32_t size) const
{
    long ret = -1;
    for (uint32_t i = 0; i < size; i++)
    {
        if (!wcscmp(names[i], name))
        {
            ret = i;
            break;
        }
    }
    return ret;
}

bool innNative::sendEvent(wchar_t* msg, wchar_t* data)
{

    if (m_iConnect)
    {
        WCHAR_T* who = 0, * what = 0, * wdata = 0;
        ::convToShortWchar(&who, extensionName);
        ::convToShortWchar(&what, msg);
        ::convToShortWchar(&wdata, data);

        bool res = m_iConnect->ExternalEvent(who, what, wdata);

        delete[] who;
        delete[] what;
        delete[] wdata;

        return res;
    }
    return false;
}
//---------------------------------------------------------------------------//
uint32_t convToShortWchar(WCHAR_T** Dest, const wchar_t* Source, uint32_t len)
{
    if (!len)
        len = ::wcslen(Source) + 1;

    if (!*Dest)
        *Dest = new WCHAR_T[len];

    WCHAR_T* tmpShort = *Dest;
    wchar_t* tmpWChar = (wchar_t*)Source;
    uint32_t res = 0;

    ::memset(*Dest, 0, len * sizeof(WCHAR_T));
#ifdef __linux__
    size_t succeed = (size_t)-1;
    size_t f = len * sizeof(wchar_t), t = len * sizeof(WCHAR_T);
    const char* fromCode = sizeof(wchar_t) == 2 ? "UTF-16" : "UTF-32";
    iconv_t cd = iconv_open("UTF-16LE", fromCode);
    if (cd != (iconv_t)-1)
    {
        succeed = iconv(cd, (char**)&tmpWChar, &f, (char**)&tmpShort, &t);
        iconv_close(cd);
        if (succeed != (size_t)-1)
            return (uint32_t)succeed;
    }
#endif //__linux__
    for (; len; --len, ++res, ++tmpWChar, ++tmpShort)
    {
        *tmpShort = (WCHAR_T)*tmpWChar;
    }

    return res;
}
//---------------------------------------------------------------------------//
uint32_t convFromShortWchar(wchar_t** Dest, const WCHAR_T* Source, uint32_t len)
{
    if (!len)
        len = getLenShortWcharStr(Source) + 1;

    if (!*Dest)
        *Dest = new wchar_t[len];

    wchar_t* tmpWChar = *Dest;
    WCHAR_T* tmpShort = (WCHAR_T*)Source;
    uint32_t res = 0;

    ::memset(*Dest, 0, len * sizeof(wchar_t));
#ifdef __linux__
    size_t succeed = (size_t)-1;
    const char* fromCode = sizeof(wchar_t) == 2 ? "UTF-16" : "UTF-32";
    size_t f = len * sizeof(WCHAR_T), t = len * sizeof(wchar_t);
    iconv_t cd = iconv_open("UTF-32LE", fromCode);
    if (cd != (iconv_t)-1)
    {
        succeed = iconv(cd, (char**)&tmpShort, &f, (char**)&tmpWChar, &t);
        iconv_close(cd);
        if (succeed != (size_t)-1)
            return (uint32_t)succeed;
    }
#endif //__linux__
    for (; len; --len, ++res, ++tmpWChar, ++tmpShort)
    {
        *tmpWChar = (wchar_t)*tmpShort;
    }

    return res;
}
//---------------------------------------------------------------------------//
uint32_t getLenShortWcharStr(const WCHAR_T* Source)
{
    uint32_t res = 0;
    WCHAR_T* tmpShort = (WCHAR_T*)Source;

    while (*tmpShort++)
        ++res;

    return res;
}
//---------------------------------------------------------------------------//

// --------------------------------------------------------------Innova-IT ------------------------------------------------------------------------//
char* WCHAR_2_CHAR(wchar_t* in_str)
{
    size_t len = wcslen(in_str) / sizeof(char) + 1;
    char* out_str = new char[len];
    size_t* out_str_len = 0;
    wcstombs(out_str, in_str, len);

    delete& len;
    delete out_str_len;

    return out_str;
}

// --------------------------------------------------------------Innova-IT ------------------------------------------------------------------------//
wchar_t* CHAR_2_WCHAR(char* in_str)
{
    size_t len = strlen(in_str) + 1;
    wchar_t* out_str = new wchar_t[len * sizeof(wchar_t)];

    size_t* out_str_len = 0;

    mbstowcs(out_str, in_str, len);

    return out_str;
}

//----------------------------------------------------------Процедуры Потоков-----------------------------------------------------------------------//
#ifdef _WINDOWS
static unsigned int _stdcall threadWindows(void* p)
{

    innNative* serialCl = (innNative*)p;

    while (true) {
        Sleep(3000);

        if (!serialCl->receiveInThread) {
            //serialCl->sendEvent(L"НовыеДанные", L"Стоп");
            return true;
        }

        //serialCl->sendEvent(L"НовыеДанные", L"Тик");

    }

    return true;
}
//---------------------------------------------------------------------------//
bool innNative::openThreadWindows(wchar_t* par)
{

    hTh = 0;
    hTh = (HANDLE)_beginthreadex(NULL, 10, threadWindows, (LPVOID)this, 0, &thID);

    if (hTh == 0) {
        sendEvent(L"Ошибка", L"Не удалось создать новый поток");
        return false;
    }

    return true;
}
bool innNative::closeThreadWindows(wchar_t* par)
{

    if (hTh)
    {
        DWORD stopThRes = WaitForSingleObject(hTh, 200);
        hTh = 0;
        if (stopThRes != WAIT_OBJECT_0)
            return false;
    }

    return true;
}
#endif

#if defined( __linux__ )

void* threadLinux(void* p) {

    innNative* serialCl = (innNative*)p;

    while (true) {
        usleep(3000000);

        if (!serialCl->receiveInThread) {
            serialCl->sendEvent(L"НовыеДанные", L"Стоп");
            pthread_exit(0);
        }

        serialCl->sendEvent(L"НовыеДанные", L"Тик");
    }

    pthread_exit(0);
}

bool  innNative::openThreadLinux(wchar_t* par)
{

    if (pthread_create(&thID, NULL, threadLinux, (LPVOID)this) != 0) {
        sendEvent(L"Ошибка", L"Не удалось создать новый поток");
        return false;
    }

    return true;
}

bool innNative::closeThreadLinux(wchar_t* par)
{

    pthread_join(thID, NULL);

    return true;
}
#endif // __LINE__

#ifdef LINUX_OR_MACOS
WcharWrapper::WcharWrapper(const WCHAR_T* str) : m_str_WCHAR(NULL),
m_str_wchar(NULL)
{
    if (str)
    {
        int len = getLenShortWcharStr(str);
        m_str_WCHAR = new WCHAR_T[len + 1];
        memset(m_str_WCHAR, 0, sizeof(WCHAR_T) * (len + 1));
        memcpy(m_str_WCHAR, str, sizeof(WCHAR_T) * len);
        ::convFromShortWchar(&m_str_wchar, m_str_WCHAR);
    }
}
#endif
//---------------------------------------------------------------------------//
WcharWrapper::WcharWrapper(const wchar_t* str) :
#ifdef LINUX_OR_MACOS
    m_str_WCHAR(NULL),
#endif 
    m_str_wchar(NULL)
{
    if (str)
    {
        int len = wcslen(str);
        m_str_wchar = new wchar_t[len + 1];
        memset(m_str_wchar, 0, sizeof(wchar_t) * (len + 1));
        memcpy(m_str_wchar, str, sizeof(wchar_t) * len);
#ifdef LINUX_OR_MACOS
        ::convToShortWchar(&m_str_WCHAR, m_str_wchar);
#endif
    }

}
//---------------------------------------------------------------------------//
WcharWrapper::~WcharWrapper()
{
#ifdef LINUX_OR_MACOS
    if (m_str_WCHAR)
    {
        delete[] m_str_WCHAR;
        m_str_WCHAR = NULL;
    }
#endif
    if (m_str_wchar)
    {
        delete[] m_str_wchar;
        m_str_wchar = NULL;
    }
}
//---------------------------------------------------------------------------//

