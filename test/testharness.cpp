// Тестовый harness для AcquiringTerminalNative.dll: загружает компоненту и прогоняет
// Open/DeviceTest/Pay/Cancel/Settlement против мок-сервера (test/mock_terminal.py).
//
// Сборка: test\build_harness.bat (из корня проекта).
// Запуск: сначала запустить мок-сервер (python -I test\mock_terminal.py),
// затем test\testharness.exe. Настройки передаются через SetParameter (как из 1С);
// файл состояния state.json (%LOCALAPPDATA%) бэкапится на время теста.

#define WIN32
#define _WINDOWS
#include <windows.h>

#include "../stdafx.h"
#include "../AcquiringTerminalNative.h"

#include <stdio.h>
#include <string>
#include <fstream>
#include <sstream>

typedef long (*GetClassObjectFn)(const WCHAR_T* wsName, IComponentBase** pInterface);
typedef long (*DestroyObjectFn)(IComponentBase** pIntf);
typedef const WCHAR_T* (*GetClassNamesFn)();
typedef AppCapabilities (*SetPlatformCapabilitiesFn)(const AppCapabilities capabilities);

static int g_failed = 0;
static int g_passed = 0;
static std::wstring g_lastAddError;

static void report(const char* name, bool ok, const std::wstring& detail = L"")
{
    if (ok) {
        ++g_passed;
        printf("[PASS] %s\n", name);
    } else {
        ++g_failed;
        printf("[FAIL] %s", name);
        if (!detail.empty()) {
            printf("  detail: ");
            for (size_t i = 0; i < detail.size(); ++i)
                putchar(detail[i] >= 32 && detail[i] < 127 ? (char)detail[i] : '?');
        }
        if (!g_lastAddError.empty()) {
            printf("  addError: ");
            for (size_t i = 0; i < g_lastAddError.size(); ++i)
                putchar(g_lastAddError[i] >= 32 && g_lastAddError[i] < 127 ? (char)g_lastAddError[i] : '?');
        }
        putchar('\n');
    }
}

// --- Мок менеджера памяти 1С ---
class MockMemoryManager : public IMemoryManager
{
public:
    bool ADDIN_API AllocMemory(void** pMemory, unsigned long ulCountByte) override
    {
        if (!pMemory || ulCountByte == 0)
            return false;
        *pMemory = malloc(ulCountByte);
        return *pMemory != nullptr;
    }
    void ADDIN_API FreeMemory(void** pMemory) override
    {
        if (pMemory && *pMemory) {
            free(*pMemory);
            *pMemory = nullptr;
        }
    }
};

static MockMemoryManager g_memory;

// --- Мок платформы 1С (IAddInDefBase): собирает AddError для отчетов ---
class MockConnection : public IAddInDefBase
{
public:
    bool ADDIN_API AddError(unsigned short wcode, const WCHAR_T* source,
        const WCHAR_T* descr, long scode) override
    {
        g_lastAddError = descr ? (const wchar_t*)descr : L"";
        return true;
    }
    bool ADDIN_API Read(WCHAR_T* wszPropName, tVariant* pVal,
        long* pErrCode, WCHAR_T** errDescriptor) override
    {
        return false;
    }
    bool ADDIN_API Write(WCHAR_T* wszPropName, tVariant* pVar) override
    {
        return false;
    }
    bool ADDIN_API RegisterProfileAs(WCHAR_T* wszProfileName) override
    {
        return true;
    }
    bool ADDIN_API SetEventBufferDepth(long lDepth) override
    {
        return true;
    }
    long ADDIN_API GetEventBufferDepth() override
    {
        return 100;
    }
    bool ADDIN_API ExternalEvent(WCHAR_T* wszSource, WCHAR_T* wszMessage, WCHAR_T* wszData) override
    {
        return true;
    }
    void ADDIN_API CleanEventBuffer() override {}
    bool ADDIN_API SetStatusLine(WCHAR_T* wszStatusLine) override
    {
        return true;
    }
    void ADDIN_API ResetStatusLine() override {}
};

static MockConnection g_connection;

// --- Хелперы tVariant ---
static void setStringIn(tVariant* v, const std::wstring& s)
{
    TV_VT(v) = VTYPE_PWSTR;
    g_memory.AllocMemory((void**)&v->pwstrVal, (unsigned long)((s.size() + 1) * sizeof(WCHAR_T)));
    wcscpy_s((wchar_t*)v->pwstrVal, s.size() + 1, s.c_str());
    v->wstrLen = (uint32_t)s.size();
}

static void setAmount(tVariant* v, double amount)
{
    TV_VT(v) = VTYPE_R8;
    TV_R8(v) = amount;
}

static std::wstring readSlip(tVariant* v)
{
    if (!v || TV_VT(v) != VTYPE_PWSTR || !TV_WSTR(v))
        return L"";
    size_t len = 0;
    while (TV_WSTR(v)[len] != 0)
        ++len;
    return std::wstring((wchar_t*)TV_WSTR(v), len);
}

static void freeVariant(tVariant* v)
{
    if (v && TV_VT(v) == VTYPE_PWSTR && TV_WSTR(v)) {
        g_memory.FreeMemory((void**)&v->pwstrVal);
        v->pwstrVal = nullptr;
        v->wstrLen = 0;
    }
}

// Вызов метода по имени с параметрами
static bool callMethod(IComponentBase* comp, const wchar_t* methodName,
    tVariant* params, long paramCount, tVariant* retVal)
{
    long num = comp->FindMethod(methodName);
    if (num < 0) {
        printf("method not found: %ls\n", methodName);
        return false;
    }
    TV_VT(retVal) = VTYPE_EMPTY;
    return comp->CallAsFunc(num, retVal, params, paramCount);
}

// Настройка через SetParameter — так же, как их передаёт 1С
static void setParam(IComponentBase* comp, const wchar_t* name, const wchar_t* value)
{
    long num = comp->FindMethod(L"SetParameter");
    if (num < 0)
        return;
    tVariant pars[2];
    memset(pars, 0, sizeof(pars));
    setStringIn(&pars[0], name);
    setStringIn(&pars[1], value);
    tVariant rv;
    memset(&rv, 0, sizeof(rv));
    comp->CallAsFunc(num, &rv, pars, 2);
    freeVariant(&pars[0]);
    freeVariant(&pars[1]);
}

static void setConfig(IComponentBase* comp, const wchar_t* terminalType, const wchar_t* port = L"8080")
{
    setParam(comp, L"TerminalType", terminalType);
    setParam(comp, L"IpAddress", L"127.0.0.1");
    setParam(comp, L"Port", port);
}

static std::wstring stateFilePath()
{
    wchar_t buf[MAX_PATH + 1] = { 0 };
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return (len > 0 && len <= MAX_PATH ? std::wstring(buf) : std::wstring(L"."))
        + L"\\AcquiringTerminalNative\\state.json";
}

int main()
{
    HMODULE dll = LoadLibraryA("1C\\Release\\AcquiringTerminalNative_64.dll");
    if (!dll) {
        printf("cannot load 1C\\Release\\AcquiringTerminalNative_64.dll (err %lu)\n", GetLastError());
        return 2;
    }

    GetClassObjectFn getClassObject = (GetClassObjectFn)GetProcAddress(dll, "GetClassObject");
    DestroyObjectFn destroyObject = (DestroyObjectFn)GetProcAddress(dll, "DestroyObject");
    SetPlatformCapabilitiesFn setCaps = (SetPlatformCapabilitiesFn)GetProcAddress(dll, "SetPlatformCapabilities");
    if (!getClassObject || !destroyObject) {
        printf("exports not found\n");
        return 2;
    }

    // Файл состояния (токены Kaspi, реквизиты операций): бэкап/восстановление
    const std::wstring kState = stateFilePath();
    const char* kStateBackup = "test\\state.json.bak";
    {
        std::ifstream in(kState.c_str(), std::ios::binary);
        if (in.good()) {
            std::stringstream ss;
            ss << in.rdbuf();
            std::ofstream out(kStateBackup, std::ios::binary);
            out << ss.str();
            printf("original state backed up to test\\state.json.bak\n");
        } else {
            std::ofstream empty(kStateBackup, std::ios::binary);
            printf("no original state, backup is empty marker\n");
        }
        in.close();
        DeleteFileW(kState.c_str()); // начинаем тесты с чистым состоянием
    }

    IComponentBase* comp = nullptr;
    // Экспорт GetClassObject в шаблоне возвращает указатель, а не код ошибки,
    // поэтому успех проверяем по ненулевому comp
    getClassObject(L"innNative", &comp);
    if (!comp) {
        printf("GetClassObject failed\n");
        return 2;
    }
    if (setCaps)
        setCaps(eAppCapabilitiesLast);
    if (!comp->setMemManager(&g_memory)) {
        printf("setMemManager failed\n");
        return 2;
    }
    if (!comp->Init(&g_connection)) {
        printf("Init failed\n");
        return 2;
    }

    WCHAR_T* extName = nullptr;
    if (comp->RegisterExtensionAs(&extName) && extName) {
        wchar_t* name = nullptr;
        // convFromShortWchar не экспортируется, WCHAR_T == wchar_t на Windows
        name = (wchar_t*)extName;
        printf("extension: %ls\n", name);
    }

    tVariant retVal;
    memset(&retVal, 0, sizeof(retVal));

    // ============================ BCC (PAX) ============================
    {
        // Open: настройки передаются XML-строкой в параметре — как БПО делает
        // при подключении (без отдельных вызовов SetParameter)
        {
            tVariant par; memset(&par, 0, sizeof(par));
            setStringIn(&par, L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                L"<Settings>"
                L"<Parameter Name=\"TerminalType\" Value=\"BCC\"/>"
                L"<Parameter Name=\"IpAddress\" Value=\"127.0.0.1\"/>"
                L"<Parameter Name=\"Port\" Value=\"8080\"/>"
                L"</Settings>");
            bool ok = callMethod(comp, L"Open", &par, 1, &retVal);
            report("BCC Open (echo, settings via XML)", ok && TV_VT(&retVal) == VTYPE_BOOL && TV_BOOL(&retVal));
            freeVariant(&par);
        }
        // DeviceTest
        {
            tVariant pars[2]; memset(pars, 0, sizeof(pars));
            bool ok = callMethod(comp, L"DeviceTest", pars, 2, &retVal);
            std::wstring desc = readSlip(&pars[0]);
            std::wstring det = L"description='" + desc + L"'";
            report("BCC DeviceTest (echo)", ok && TV_BOOL(&retVal), det);
            freeVariant(&pars[0]);
        }
        // PayByPaymentCard
        {
            tVariant pars[7]; memset(pars, 0, sizeof(pars));
            setStringIn(&pars[0], L"Device123");   // deviceID
            setStringIn(&pars[1], L"");            // cardNumber
            setAmount(&pars[2], 100);              // amount
            setStringIn(&pars[3], L"1");           // receiptNumber
            setStringIn(&pars[4], L"");            // rrnCode
            setStringIn(&pars[5], L"");            // authorizationCode
            bool ok = callMethod(comp, L"PayByPaymentCard", pars, 7, &retVal);
            std::wstring slip = readSlip(&pars[6]);
            bool slipOk = slip.find(L"517111027327") != std::wstring::npos
                && slip.find(L"ОПЛАТА") != std::wstring::npos;
            std::wstring det = L"slip='" + slip.substr(0, 60) + L"...'";
            report("BCC PayByPaymentCard", ok && TV_VT(&retVal) == VTYPE_BOOL && TV_BOOL(&retVal) && slipOk, det);
            freeVariant(&pars[0]); freeVariant(&pars[1]); freeVariant(&pars[3]);
            freeVariant(&pars[4]); freeVariant(&pars[5]); freeVariant(&pars[6]);
        }
        // CancelPaymentByPaymentCard (refund по RRN из оплаты)
        {
            tVariant pars[7]; memset(pars, 0, sizeof(pars));
            setStringIn(&pars[0], L"Device123");
            setStringIn(&pars[1], L"4400********1234"); // cardNumber
            setAmount(&pars[2], 50);                    // amount (частичный возврат)
            setStringIn(&pars[3], L"2");                // receiptNumber
            setStringIn(&pars[4], L"517111027327");     // rrnCode
            setStringIn(&pars[5], L"AUTH123");          // authorizationCode
            bool ok = callMethod(comp, L"CancelPaymentByPaymentCard", pars, 7, &retVal);
            std::wstring slip = readSlip(&pars[6]);
            bool slipOk = slip.find(L"ВОЗВРАТ") != std::wstring::npos;
            report("BCC CancelPaymentByPaymentCard", ok && TV_BOOL(&retVal) && slipOk);
            freeVariant(&pars[0]); freeVariant(&pars[1]); freeVariant(&pars[3]);
            freeVariant(&pars[4]); freeVariant(&pars[5]); freeVariant(&pars[6]);
        }
        // Settlement
        {
            tVariant pars[2]; memset(pars, 0, sizeof(pars));
            setStringIn(&pars[0], L"Device123");
            bool ok = callMethod(comp, L"Settlement", pars, 2, &retVal);
            std::wstring slip = readSlip(&pars[1]);
            bool slipOk = slip.find(L"СВЕРКА") != std::wstring::npos;
            report("BCC Settlement", ok && TV_BOOL(&retVal) && slipOk);
            freeVariant(&pars[0]); freeVariant(&pars[1]);
        }
        // GetLastError должен вернуть код последней ошибки, сначала сброс через ошибку не проверяем
    }

    // ============================ Halyk (PAX) ============================
    setConfig(comp, L"Halyk");
    {
        tVariant pars[7]; memset(pars, 0, sizeof(pars));
        setStringIn(&pars[0], L"Device123");
        setStringIn(&pars[1], L"");
        setAmount(&pars[2], 200);
        setStringIn(&pars[3], L"1");
        setStringIn(&pars[4], L"");
        setStringIn(&pars[5], L"");
        bool ok = callMethod(comp, L"PayByPaymentCard", pars, 7, &retVal);
        report("Halyk PayByPaymentCard", ok && TV_BOOL(&retVal));
        freeVariant(&pars[0]); freeVariant(&pars[1]); freeVariant(&pars[3]);
        freeVariant(&pars[4]); freeVariant(&pars[5]); freeVariant(&pars[6]);
    }

    // ============================ Kaspi v2 ============================
    setConfig(comp, L"Kaspi");
    {
        // Open — при первом запуске: register -> deviceinfo
        {
            tVariant par; memset(&par, 0, sizeof(par));
            setStringIn(&par, L"");
            bool ok = callMethod(comp, L"Open", &par, 1, &retVal);
            report("Kaspi Open (deviceinfo)", ok && TV_BOOL(&retVal));
            freeVariant(&par);
        }

        // PayByPaymentCard: register/token -> payment -> опрос статуса -> success
        {
            tVariant pars[7]; memset(pars, 0, sizeof(pars));
            setStringIn(&pars[0], L"Device123");
            setStringIn(&pars[1], L"");
            setAmount(&pars[2], 300);
            setStringIn(&pars[3], L"1");
            setStringIn(&pars[4], L"");
            setStringIn(&pars[5], L"");
            bool ok = callMethod(comp, L"PayByPaymentCard", pars, 7, &retVal);
            std::wstring slip = readSlip(&pars[6]);
            bool slipOk = slip.find(L"Магазин") != std::wstring::npos
                && slip.find(L"517111027327") != std::wstring::npos;
            report("Kaspi PayByPaymentCard (status polling)", ok && TV_BOOL(&retVal) && slipOk);
            freeVariant(&pars[0]); freeVariant(&pars[1]); freeVariant(&pars[3]);
            freeVariant(&pars[4]); freeVariant(&pars[5]); freeVariant(&pars[6]);
        }

        // Проверяем сохранение реквизитов транзакции в файл состояния
        {
            std::ifstream in(kState.c_str());
            std::stringstream ss; ss << in.rdbuf();
            std::string cfg = ss.str();
            bool ok = cfg.find("LastKaspiRRN") != std::string::npos
                && cfg.find("KaspiTerminalId") != std::string::npos;
            report("Kaspi state saved (LastKaspiRRN, KaspiTerminalId)", ok);
        }

        // CancelPaymentByPaymentCard: refund по сохранённым реквизитам
        {
            tVariant pars[7]; memset(pars, 0, sizeof(pars));
            setStringIn(&pars[0], L"Device123");
            setStringIn(&pars[1], L"4400********1234");
            setAmount(&pars[2], 100);
            setStringIn(&pars[3], L"2");
            setStringIn(&pars[4], L"517111027327");
            setStringIn(&pars[5], L"AUTH123");
            bool ok = callMethod(comp, L"CancelPaymentByPaymentCard", pars, 7, &retVal);
            std::wstring slip = readSlip(&pars[6]);
            bool slipOk = slip.find(L"ВОЗВРАТ") != std::wstring::npos;
            report("Kaspi CancelPaymentByPaymentCard (refund)", ok && TV_BOOL(&retVal) && slipOk);
            freeVariant(&pars[0]); freeVariant(&pars[1]); freeVariant(&pars[3]);
            freeVariant(&pars[4]); freeVariant(&pars[5]); freeVariant(&pars[6]);
        }

        // Settlement — информационное сообщение
        {
            tVariant pars[2]; memset(pars, 0, sizeof(pars));
            setStringIn(&pars[0], L"Device123");
            bool ok = callMethod(comp, L"Settlement", pars, 2, &retVal);
            std::wstring slip = readSlip(&pars[1]);
            bool slipOk = slip.find(L"Kaspi Smart POS") != std::wstring::npos;
            report("Kaspi Settlement (notice)", ok && TV_BOOL(&retVal) && slipOk);
            freeVariant(&pars[0]); freeVariant(&pars[1]);
        }
    }

    // ============================ Ошибка: неверный тип ============================
    {
        setConfig(comp, L"Unknown");

        tVariant pars[7]; memset(pars, 0, sizeof(pars));
        setStringIn(&pars[0], L"Device123");
        setStringIn(&pars[1], L"");
        setAmount(&pars[2], 10);
        setStringIn(&pars[3], L"1");
        setStringIn(&pars[4], L"");
        setStringIn(&pars[5], L"");
        bool ok = callMethod(comp, L"PayByPaymentCard", pars, 7, &retVal);
        report("Unknown terminal type rejected", !ok);
        freeVariant(&pars[0]); freeVariant(&pars[1]); freeVariant(&pars[3]);
        freeVariant(&pars[4]); freeVariant(&pars[5]); freeVariant(&pars[6]);

        // GetLastError теперь возвращает текст последней ошибки
        tVariant epars[1]; memset(epars, 0, sizeof(epars));
        long num = comp->FindMethod(L"GetLastError");
        tVariant code; memset(&code, 0, sizeof(code));
        comp->CallAsFunc(num, &code, epars, 1);
        std::wstring errText = readSlip(&epars[0]);
        bool lastOk = TV_VT(&code) == VTYPE_I4 && errText.find(L"Неизвестный тип терминала") != std::wstring::npos;
        report("GetLastError returns last error", lastOk);
        freeVariant(&epars[0]);
    }

    comp->Done();
    destroyObject(&comp);
    FreeLibrary(dll);

    // Восстанавливаем оригинальное состояние (или убираем тестовый файл)
    {
        std::ifstream in(kStateBackup, std::ios::binary);
        std::stringstream ss; ss << in.rdbuf();
        std::string content = ss.str();
        if (!content.empty()) {
            std::ofstream out(kState.c_str(), std::ios::binary);
            out << content;
        } else {
            DeleteFileW(kState.c_str());
        }
        remove(kStateBackup);
        printf("original state restored\n");
    }

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
