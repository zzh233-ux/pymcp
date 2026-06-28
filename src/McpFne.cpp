// McpFne.cpp - elib 支持库入口，只保留 MCP 启动 / 关闭逻辑
//
// 生命周期：
//   NL_SYS_NOTIFY_FUNCTION -> NotifySys 接口就绪
//   NL_IDE_READY           -> 获取主窗口 -> 启动 LocalMcpServer
//   主窗口 WM_NCDESTROY     -> LocalMcpServer::Shutdown
//
// 不包含：AI 会话页签、右键菜单、Linker 切换、EC 自动切换、核心库重写、
//         Headless 编译、e-packager、Tavily/WebDocument、IDEFacade Hooks、版本检查。

#include "McpFne.h"

#include <CommCtrl.h>
#include <format>
#include <mutex>

#include "LocalMcpServer.h"
#include "Logger.h"
#include "BuiltInTools.h"

#pragma comment(lib, "comctl32.lib")

// === McpFne.h 中声明为 extern 的全局状态在此定义 ===
HWND g_hwnd            = nullptr;
bool g_notifySysReady  = false;
bool g_uiInitialized   = false;

namespace {

std::mutex g_initMutex;
bool g_mainWindowSubclassInstalled = false;

// 取主窗口句柄，与 AutoLinker 同样优先用 NotifySys(NES_GET_MAIN_HWND)，
// 失败则枚举本进程窗口兜底（极简实现，不依赖 WindowHelper）。
HWND GetMainWindowByProcess()
{
    HWND found = nullptr;
    EnumWindows([](HWND hWnd, LPARAM lParam) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(hWnd, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        if (IsWindowVisible(hWnd) == FALSE) return TRUE;
        if (GetWindow(hWnd, GW_OWNER) != nullptr) return TRUE;
        *reinterpret_cast<HWND*>(lParam) = hWnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

LRESULT CALLBACK MainWindowSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/)
{
    if (uMsg == WM_NCDESTROY) {
        LocalMcpServer::Shutdown();
        g_mainWindowSubclassInstalled = false;
        RemoveWindowSubclass(hWnd, MainWindowSubclassProc, uIdSubclass);
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

bool FneInit()
{
    std::lock_guard<std::mutex> lock(g_initMutex);
    if (g_uiInitialized) return true;

    if (!g_notifySysReady) {
        OutputStringToELog("McpFne: system notify interface not ready");
        return false;
    }

    if (g_hwnd == nullptr || !IsWindow(g_hwnd)) {
        g_hwnd = reinterpret_cast<HWND>(NotifySys(NES_GET_MAIN_HWND, 0, 0));
        if (g_hwnd == nullptr || !IsWindow(g_hwnd)) {
            g_hwnd = GetMainWindowByProcess();
        }
    }
    if (g_hwnd == nullptr || !IsWindow(g_hwnd)) {
        OutputStringToELog("McpFne: main window not found");
        return false;
    }

    if (!g_mainWindowSubclassInstalled) {
        if (SetWindowSubclass(g_hwnd, MainWindowSubclassProc, 0, 0) == FALSE) {
            OutputStringToELog("McpFne: subclass failed");
            return false;
        }
        g_mainWindowSubclassInstalled = true;
    }

    OutputStringToELog(std::format("McpFne loaded, pid={}", GetCurrentProcessId()));

    BuiltInTools::RegisterAll();
    LocalMcpServer::Initialize();
    g_uiInitialized = true;
    return true;
}

} // namespace

INT WINAPI McpFne_MessageNotify(INT nMsg, DWORD dwParam1, DWORD dwParam2)
{
#ifndef __E_STATIC_LIB
    if (nMsg == NL_GET_CMD_FUNC_NAMES)        return (INT)NULL;
    if (nMsg == NL_GET_NOTIFY_LIB_FUNC_NAME)  return (INT)LIBARAYNAME;
    if (nMsg == NL_GET_DEPENDENT_LIBS)       return (INT)NULL;
    if (nMsg == NL_SYS_NOTIFY_FUNCTION && dwParam1 && !g_notifySysReady) {
        g_notifySysReady = true;
        OutputStringToELog("McpFne: system notify interface ready");
    }
    if (nMsg == NL_IDE_READY) {
        if (!g_notifySysReady) {
            OutputStringToELog("McpFne: NL_IDE_READY before notify ready");
        } else {
            FneInit();
        }
    }
#endif
    return ProcessNotifyLib(nMsg, dwParam1, dwParam2);
}

#ifndef __E_STATIC_LIB
static LIB_INFOX LibInfo =
{
    LIB_FORMAT_VER,
    _T(LIB_GUID_STR),
    LIB_MajorVersion,
    LIB_MinorVersion,
    LIB_BuildNumber,
    LIB_SysMajorVer,
    LIB_SysMinorVer,
    LIB_KrnlLibMajorVer,
    LIB_KrnlLibMinorVer,
    _T(LIB_NAME_STR),
    __GBK_LANG_VER,
    _WT(LIB_DESCRIPTION_STR),
    LBS_IDE_PLUGIN | LBS_LIB_INFO2,
    _WT(LIB_Author),
    _WT(LIB_ZipCode),
    _WT(LIB_Address),
    _WT(LIB_Phone),
    _WT(LIB_Fax),
    _WT(LIB_Email),
    _WT(LIB_HomePage),
    _WT(LIB_Other),
    0,                          // m_nDataTypeCount
    NULL,                       // m_pDataType
    LIB_TYPE_COUNT,             // m_nCategoryCount
    _WT(LIB_TYPE_STR),          // m_szzCategory
    0,                          // m_nCmdCount
    NULL,                       // m_pBeginCmdInfo
    NULL,                       // m_pCmdsFunc
    NULL,                       // m_pfnRunAddInFn
    NULL,                       // m_szzAddInFnInfo
    McpFne_MessageNotify,       // m_pfnNotify (必填，IDE 由此回调)
    NULL,                       // m_pfnSuperTemplate
    NULL,                       // m_szzSuperTemplateInfo
    0,                          // m_nLibConstCount
    NULL,                       // m_pLibConst
    NULL,                       // m_szzDependFiles
    NULL,                       // m_szHardwareCode
    NULL,                       // m_szBuyingTips
    NULL,                       // m_szBuyingURL
    NULL                        // m_szLicenseToUserName
};

PLIB_INFOX WINAPI GetNewInf()
{
    return &LibInfo;
}
#endif