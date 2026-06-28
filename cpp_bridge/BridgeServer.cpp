// BridgeServer.cpp — 命名管道服务端实现
//
// 架构:
//   Python MCP 服务器 ←→ 命名管道 ←→ [本文件] ←→ IDE 主线程
//
// 本文件替换原有的 LocalMcpServer.cpp,去掉 HTTP/JSON-RPC 协议处理,
// 仅保留命名管道 I/O 和工具派发。MCP 协议层由外部 Python 服务器处理。
//
// 线程模型:
//   - 管道服务线程: 创建管道实例 → 等待连接 → 处理请求循环 → 断开后重建
//   - 工具执行: 通过 SendMessage 同步派发到 IDE 主线程(与原架构一致)
//
// 编码: UTF-8 with BOM, CRLF

#include "BridgeServer.h"
#include "BridgeProtocol.h"
#include "Logger.h"
#include "StringHelper.h"

// 工具派发(复用现有代码)
#include "AIChatFeature.h"

#include <windows.h>
#include <string>
#include <algorithm>

// ══════════════════════════════════════════════════════════════
//  单例
// ══════════════════════════════════════════════════════════════

BridgeServer& BridgeServer::Instance() {
    static BridgeServer instance;
    return instance;
}

// ══════════════════════════════════════════════════════════════
//  生命周期
// ══════════════════════════════════════════════════════════════

void BridgeServer::Initialize() {
    if (m_running.load()) {
        return;
    }
    m_running.store(true);
    m_thread = std::thread(&BridgeServer::ServerThreadMain, this);
    LogInfo(L"[BridgeServer] 已启动,管道: %ls", BridgeProtocol::kPipeName);
}

void BridgeServer::Shutdown() {
    if (!m_running.load()) {
        return;
    }
    m_running.store(false);

    // 关闭监听管道,使 ConnectNamedPipe 返回
    if (m_listenPipe != nullptr && m_listenPipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(m_listenPipe);
        CloseHandle(m_listenPipe);
        m_listenPipe = nullptr;
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }
    LogInfo(L"[BridgeServer] 已关闭");
}

// ══════════════════════════════════════════════════════════════
//  管道服务线程主循环
// ══════════════════════════════════════════════════════════════

void BridgeServer::ServerThreadMain() {
    constexpr DWORD kPipeBufferSize = 65536;

    while (m_running.load()) {
        // 创建管道实例
        m_listenPipe = CreateNamedPipeW(
            BridgeProtocol::kPipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE,
            1,                          // 最大实例数
            kPipeBufferSize,            // 输出缓冲区
            kPipeBufferSize,            // 输入缓冲区
            0,                          // 默认超时
            nullptr                     // 默认安全属性
        );

        if (m_listenPipe == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            LogError(L"[BridgeServer] CreateNamedPipeW 失败(%lu),1秒后重试", err);
            Sleep(1000);
            continue;
        }

        // 等待客户端连接(阻塞)
        BOOL connected = ConnectNamedPipe(m_listenPipe, nullptr);
        DWORD lastErr = GetLastError();

        if (!m_running.load()) {
            // 关闭中,退出
            CloseHandle(m_listenPipe);
            m_listenPipe = nullptr;
            break;
        }

        if (!connected && lastErr != ERROR_PIPE_CONNECTED) {
            LogError(L"[BridgeServer] ConnectNamedPipe 失败(%lu)", lastErr);
            CloseHandle(m_listenPipe);
            m_listenPipe = nullptr;
            continue;
        }

        // 处理客户端请求
        HandleClient(m_listenPipe);

        // 断开并关闭
        DisconnectNamedPipe(m_listenPipe);
        CloseHandle(m_listenPipe);
        m_listenPipe = nullptr;
    }
}

// ══════════════════════════════════════════════════════════════
//  客户端处理(请求-响应循环)
// ══════════════════════════════════════════════════════════════

void BridgeServer::HandleClient(HANDLE pipe) {
    LogInfo(L"[BridgeServer] 客户端已连接");

    while (m_running.load()) {
        // 读取请求
        nlohmann::json request;
        if (!BridgeProtocol::ReadMessage(pipe, request)) {
            break; // 客户端断开或错误
        }

        // 解析请求
        std::string id = request.value("id", "");
        std::string tool = request.value("tool", "");
        nlohmann::json arguments = request.value("arguments", nlohmann::json::object());

        LogInfo(L"[BridgeServer] 工具调用: %hs", tool.c_str());

        // 派发工具
        BridgeProtocol::ToolResponse resp;
        resp.id = id;

        try {
            resp = DispatchTool(tool, arguments);
        } catch (const std::exception& e) {
            resp.ok = false;
            resp.error = {
                {"code", -32603},
                {"message", std::string("内部错误: ") + e.what()}
            };
        } catch (...) {
            resp.ok = false;
            resp.error = {
                {"code", -32603},
                {"message", "未知内部错误"}
            };
        }

        // 发送响应
        nlohmann::json respJson = BridgeProtocol::ResponseToJson(resp);
        if (!BridgeProtocol::WriteMessage(pipe, respJson)) {
            LogError(L"[BridgeServer] 写入响应失败,管道可能已断开");
            break;
        }
    }

    LogInfo(L"[BridgeServer] 客户端已断开");
}

// ══════════════════════════════════════════════════════════════
//  工具派发
// ══════════════════════════════════════════════════════════════

BridgeProtocol::ToolResponse BridgeServer::DispatchTool(
    const std::string& tool,
    const nlohmann::json& arguments
) {
    BridgeProtocol::ToolResponse resp;

    // ── 特殊:健康检查 ──────────────────────────────────
    if (tool == "__health__") {
        resp.ok = true;
        resp.result = {
            {"pid", static_cast<int>(GetCurrentProcessId())},
            {"pipe", StringHelper::WToUtf8(BridgeProtocol::kPipeName)},
            {"status", "ok"}
        };
        return resp;
    }

    // ── 空工具名 ────────────────────────────────────────
    if (tool.empty()) {
        resp.ok = false;
        resp.error = {{"code", -32600}, {"message", "缺少工具名"}};
        return resp;
    }

    // ── 转发到现有工具派发机制 ──────────────────────────
    // AIChatFeature::ExecutePublicTool 内部会通过 SendMessage
    // 将工具调用同步派发到 IDE 主线程执行。
    std::string resultJson;
    bool toolOk = false;

    AIChatFeature::ExecutePublicTool(
        tool,
        arguments.dump(),
        resultJson,
        toolOk
    );

    resp.ok = toolOk;
    if (toolOk) {
        // 尝试解析为 JSON,失败则作为字符串
        try {
            resp.result = nlohmann::json::parse(resultJson);
        } catch (...) {
            resp.result = resultJson;
        }
    } else {
        resp.error = {
            {"code", -32603},
            {"message", resultJson}
        };
    }

    return resp;
}
