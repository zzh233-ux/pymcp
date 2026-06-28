#pragma once

// BridgeServer.h — IDE 内命名管道桥接服务端
// 替换 LocalMcpServer,用命名管道代替 Winsock HTTP,
// 协议处理交给外部 Python MCP 服务器。

#include <atomic>
#include <thread>
#include <string>
#include <windows.h>

class BridgeServer {
public:
    static BridgeServer& Instance();

    // 在 IDE 就绪时(NL_IDE_READY)调用,启动管道服务线程
    void Initialize();

    // 在 IDE 关闭时调用,停止服务
    void Shutdown();

    // 是否正在运行
    bool IsRunning() const { return m_running.load(); }

private:
    BridgeServer() = default;
    ~BridgeServer() = default;
    BridgeServer(const BridgeServer&) = delete;
    BridgeServer& operator=(const BridgeServer&) = delete;

    // 服务线程主循环:创建管道 → 等待连接 → 处理请求 → 断开 → 重建
    void ServerThreadMain();

    // 处理一个已连接的客户端(循环读取请求,返回响应)
    void HandleClient(HANDLE clientPipe);

    // 派发工具调用到 IDE 主线程(通过 AIChatFeature::ExecutePublicTool)
    // 返回 JSON 响应字符串
    std::string DispatchTool(const std::string& requestId,
                             const std::string& toolName,
                             const std::string& argumentsJson);

    std::atomic<bool>   m_running{false};
    std::atomic<bool>   m_stopRequested{false};
    std::thread         m_thread;
    HANDLE              m_listenPipe = nullptr;  // 用于 Shutdown 时打断 ConnectNamedPipe
};
