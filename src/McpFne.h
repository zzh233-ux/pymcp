// McpFne.h - 纯 MCP 支持库（精简版 AutoLinker，仅保留 Local MCP）
// 目的：在易语言 IDE 内加载后暴露一个 MCP HTTP 服务（JSON-RPC 2.0），
//       工具实现自行登记到 LocalMcpServer 的工具注册表。不依赖 IDEFacade/Detours/e-packager。
#pragma once

#include <windows.h>
#include <tchar.h>

#include <string>

#include "lib2.h"      // elib 数据结构
#include "lang.h"       // __GBK_LANG_VER / _WT 等
#include "fnshare.h"   // NotifySys / ProcessNotifyLib 声明

#include "..\thirdparty\json.hpp"

using json = nlohmann::json;

// 易语言 IDE 通过 m_pfnNotify 字段调用本符号（无需导出，由 LIB_INFOX 字段直接传指针）
#define LIBARAYNAME "McpFne_MessageNotify"

// === 支持库元信息 ===
#define LIB_GUID_STR            "{7A9C6F2E-3B74-4D5A-9E1F-2C88A0B6F341}"
#define LIB_MajorVersion        1
#define LIB_MinorVersion         0
#define LIB_BuildNumber          1000000
#define LIB_SysMajorVer         3
#define LIB_SysMinorVer         7
#define LIB_KrnlLibMajorVer     5
#define LIB_KrnlLibMinorVer     3
#define LIB_NAME_STR            "McpFne"
#define LIB_DESCRIPTION_STR     "McpFne - pure MCP support library (JSON-RPC 2.0 over http://127.0.0.1:19207/mcp)."
#define LIB_Author              "aiqinxuancai/derived"
#define LIB_ZipCode              ""
#define LIB_Address              ""
#define LIB_Phone                ""
#define LIB_Fax                  ""
#define LIB_Email                ""
#define LIB_HomePage             "https://github.com/aiqinxuancai/AutoLinker"
#define LIB_Other                ""
#define LIB_TYPE_COUNT           1
#define LIB_TYPE_STR             "0000MCP\0" "\0"

// === 全局状态（在此精简工程中由 McpFne.cpp 定义） ===
extern HWND  g_hwnd;            // 易语言主窗口句柄
extern bool  g_notifySysReady;  // 已收到 NL_SYS_NOTIFY_FUNCTION
extern bool  g_uiInitialized;   // FneInit 已完成

// 向 IDE 输出窗口 / 调试器写一行（GBK 输入），精简版仅写 OutputDebugString + 日志文件
void OutputStringToELog(const std::string& szbuf);

extern "C" INT WINAPI McpFne_MessageNotify(INT nMsg, DWORD dwParam1, DWORD dwParam2);