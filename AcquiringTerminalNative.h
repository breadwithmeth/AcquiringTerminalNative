#pragma once

#include "ComponentBase.h"
#include "AddInDefBase.h"
#include "IMemoryManager.h"
#include "stdafx.h"
#include <winhttp.h>

#if defined( __linux__ )
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <pthread.h>
#include <iconv.h>
#endif

#include <stdio.h>
#include <wchar.h>
#include <string>
#include <locale>
#include <map>
///////////////////////////////////////////////////////////////////////////////
// ���ѧ�ѧާ֧��� ���էܧݧ��֧ߧڧ� ��֧�ާڧߧѧݧ� (C:\config\parameters.json)
struct TerminalConfig
{
    std::wstring ip = L"127.0.0.1";
    int port = 8080;
    std::string terminalType = "Kaspi";

    // Kaspi Smart POS
    bool kaspiOwnCheque = true;               // true - чек печатает касса
    int kaspiTimeoutSec = 240;                // таймаут ожидания операции, сек
    std::string kaspiRefundMethod = "card";   // способ возврата, если метод оплаты не сохранен

    // BCC (PAX)
    int bccRefundMethod = 0;                  // 0 - карта, 1 - WeChat QR, 2 - BCC QR
};

///////////////////////////////////////////////////////////////////////////////
// class innNative
class innNative : public IComponentBase
{
public:
    enum Props
    {
        epVersion,
        epLast
    };

    enum Methods
    {
        emGetVersion,
        emGetDescription,
        emGetLastError,
        emGetParameters,
        emSetParameter,
        emOpen,
        emClose,
        emDeviceTest,
        emGetAdditionalActions,
        emDoAdditionalAction,
        emPayByPaymentCard,
        emPrintSlipOnTerminal,
        emConnectEquipment,
        emCancelPaymentByPaymentCard,
        emGetInterfaceRevision,
        emSettlement,
        emLast

    };

    innNative(void);
    virtual ~innNative();
    // IInitDoneBase
    virtual bool ADDIN_API Init(void*);
    virtual bool ADDIN_API setMemManager(void* mem);
    virtual long ADDIN_API GetInfo();
    virtual void ADDIN_API Done();
    // ILanguageExtenderBase
    virtual bool ADDIN_API RegisterExtensionAs(WCHAR_T**);
    virtual long ADDIN_API GetNProps();
    virtual long ADDIN_API FindProp(const WCHAR_T* wsPropName);
    virtual const WCHAR_T* ADDIN_API GetPropName(long lPropNum, long lPropAlias);
    virtual bool ADDIN_API GetPropVal(const long lPropNum, tVariant* pvarPropVal);
    virtual bool ADDIN_API SetPropVal(const long lPropNum, tVariant* varPropVal);
    virtual bool ADDIN_API IsPropReadable(const long lPropNum);
    virtual bool ADDIN_API IsPropWritable(const long lPropNum);
    virtual long ADDIN_API GetNMethods();
    virtual long ADDIN_API FindMethod(const WCHAR_T* wsMethodName);
    virtual const WCHAR_T* ADDIN_API GetMethodName(const long lMethodNum,
        const long lMethodAlias);
    virtual long ADDIN_API GetNParams(const long lMethodNum);
    virtual bool ADDIN_API GetParamDefValue(const long lMethodNum, const long lParamNum,
        tVariant* pvarParamDefValue);
    virtual bool ADDIN_API HasRetVal(const long lMethodNum);
    virtual bool ADDIN_API CallAsProc(const long lMethodNum,
        tVariant* paParams, const long lSizeArray);
    virtual bool ADDIN_API CallAsFunc(const long lMethodNum,
        tVariant* pvarRetValue, tVariant* paParams, const long lSizeArray);
    // LocaleBase
    virtual void ADDIN_API SetLocale(const WCHAR_T* loc);
    bool sendEvent(wchar_t* msg, wchar_t* Data);

    bool receiveInThread = false;

#ifdef _WINDOWS
    bool openThreadWindows(wchar_t* par);
    bool closeThreadWindows(wchar_t* par);
    HANDLE hTh;
    unsigned int thID;
#endif

#if defined( __linux__ )
    bool openThreadLinux(wchar_t* par);
    bool closeThreadLinux(wchar_t* par);
    pthread_t thID;
#endif

private:
    bool GetInterfaceRevision(tVariant* retVal);  // ������ long GetInterfaceRevision();

    bool PayByPaymentCard(tVariant* retVal, tVariant* deviceID, tVariant* cardNumber, tVariant* amount, tVariant* receiptNumber, tVariant* rrnCode, tVariant* authorizationCode, tVariant* slip);

    long findName(const wchar_t* names[], const wchar_t* name, const uint32_t size) const;
    void addError(uint32_t wcode, const wchar_t* source, const wchar_t* descriptor, long code);

    bool CancelPaymentByPaymentCard(tVariant* retVal, tVariant* deviceID, tVariant* cardNumber, tVariant* amount, tVariant* receiptNumber, tVariant* rrnCode, tVariant* authorizationCode, tVariant* slip);


    void setWStringToTVariant(tVariant* dest, const wchar_t* source);
    bool mGetVersion(tVariant* retVal);
    bool mGetDescription(tVariant* retVal, tVariant* name, tVariant* description, tVariant* equipmentType, tVariant* interfaceRevision, tVariant* integrationLibrary, tVariant* mainDriverInstalled, tVariant* getDownloadURL);
    bool mGetLastError(tVariant* retVal, tVariant* errorDescription);
    bool ConnectEquipment(tVariant* retVal, tVariant* deviceID, tVariant* equipmentType, tVariant* connectionParams);
    bool mGetParameters(tVariant* retVal, tVariant* xml);
    bool mSetParameter(tVariant* retVal, tVariant* par, tVariant* val);
    bool PrintSlipOnTerminal(tVariant* retVal);
    bool mOpen(tVariant* retVal, tVariant* par);

    bool mClose(tVariant* retVal, tVariant* id);
    bool mDeviceTest(tVariant* retVal, tVariant* description, tVariant* demoModeIsActivated);
    bool TerminalParamters(tVariant* retVal, tVariant* xml);
    bool mGetAdditionalActions(tVariant* retVal, tVariant* xml);
    bool mDoAdditionalAction(tVariant* retVal, tVariant* actionName);
    bool pay(tVariant* retVal, tVariant* a, tVariant* b, tVariant* c, tVariant* d, tVariant* e, tVariant* f);
    bool PrintReceiptsOnTerminal(tVariant* retVal);
    bool Settlement(tVariant* retVal, tVariant* deviceID, tVariant* slip);

    // Attributes
    IAddInDefBase* m_iConnect;
    IMemoryManager* m_iMemory;

    // Kaspi Smart POS v2: �٧ѧ���� ���֧�ѧ�ڧ� (payment/refund), ������ ���ѧ����, ��֧�
    bool KaspiRunOperation(const TerminalConfig& cfg, const std::wstring& startPath,
        bool isRefund, const wchar_t* source, tVariant* slip, tVariant* retVal);

    // Настройки, переданные из 1С: SetParameter и XML при подключении
    std::map<std::wstring, std::wstring> m_params;
    std::wstring GetSetting(const wchar_t* name, const wchar_t* defaultValue) const;
    bool LoadTerminalConfig(TerminalConfig& out) const;
};

class WcharWrapper
{
public:
#ifdef LINUX_OR_MACOS
    WcharWrapper(const WCHAR_T* str);
#endif
    WcharWrapper(const wchar_t* str);
    ~WcharWrapper();
#ifdef LINUX_OR_MACOS
    operator const WCHAR_T* () { return m_str_WCHAR; }
    operator WCHAR_T* () { return m_str_WCHAR; }
#endif
    operator const wchar_t* () { return m_str_wchar; }
    operator wchar_t* () { return m_str_wchar; }
private:
    WcharWrapper& operator = (const WcharWrapper& other) { return *this; }
    WcharWrapper(const WcharWrapper& other) {}
private:
#ifdef LINUX_OR_MACOS
    WCHAR_T* m_str_WCHAR;
#endif
    wchar_t* m_str_wchar;
};
