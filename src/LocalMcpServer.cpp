// LocalMcpServer.cpp
// 改造自 AutoLinker/src/LocalMcpServer.cpp（MIT, aiqinxuancai）。
// 去掉 AIService / AIChatFeature / IDEFacade / LocalMcpInstanceRegistry / PathHelper 依赖，
// 工具调用改为查 McpTools 注册表；日志改为本地 Logger。
//
// 主要保留：
//   - winsock2 手写 HTTP/1.1 服务端（单连接串行处理）
//   - JSON-RPC 2.0 分发：initialize / notifications/initialized / ping / tools/list / tools/call
//   - 端口 19207 起递增探测，最多 16 个
//   - GET / 返回健康检查 JSON，OPTIONS 返回 204，POST / 或 /mcp 处理 JSON-RPC
//   - .dump() 全部用 error_handler_t::replace 防止非法 UTF-8 抛异常导致进程崩溃
#include "LocalMcpServer.h"
#include "McpToolRegistry.h"
#include "Logger.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "..\thirdparty\json.hpp"

#pragma comment(lib, "Ws2_32.lib")

namespace {

constexpr const char* kServerName    = "McpFne Local MCP";
constexpr const char* kServerVersion = "1.0.0";
constexpr const char* kBindHost      = "127.0.0.1";
constexpr int  kBasePort             = 19207;
constexpr int  kMaxPortAttempts      = 16;

std::atomic_bool g_stopRequested{false};
std::atomic_bool g_running{false};
std::atomic_int  g_boundPort{0};
std::mutex       g_stateMutex;
std::thread      g_serverThread;
SOCKET           g_listenSocket = INVALID_SOCKET;

struct HttpRequest {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

void LogMcp(const std::string& msg) { Logger::Instance().WriteAndIde("LocalMCP", msg); }

std::string TrimAsciiCopy(const std::string& text)
{
    size_t b = 0, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b])) != 0) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1])) != 0) --e;
    return text.substr(b, e - b);
}

std::string ToLowerAsciiCopy(const std::string& text)
{
    std::string r = text;
    std::transform(r.begin(), r.end(), r.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

void CloseSocketSafe(SOCKET& s)
{
    if (s == INVALID_SOCKET) return;
    shutdown(s, SD_BOTH);
    closesocket(s);
    s = INVALID_SOCKET;
}

std::string DumpJsonSafe(const nlohmann::json& v)
{
    return v.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

nlohmann::json BuildJsonRpcError(const nlohmann::json& id, int code, const std::string& msg)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", msg}}}};
}

nlohmann::json BuildJsonRpcResult(const nlohmann::json& id, const nlohmann::json& result)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

nlohmann::json BuildInitializeResult()
{
    return {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", {{"tools", {{"listChanged", false}}}}},
        {"serverInfo", {{"name", kServerName}, {"version", kServerVersion}}}
    };
}

std::string BuildEndpointForPort(int port)
{
    if (port <= 0) return std::string();
    return std::format("http://{}:{}/mcp", kBindHost, port);
}

bool ReadExactBytes(SOCKET sock, std::string& buffer, size_t wanted)
{
    while (buffer.size() < wanted) {
        char tmp[4096];
        int toRead = static_cast<int>((std::min)(wanted - buffer.size(), sizeof(tmp)));
        int got = recv(sock, tmp, toRead, 0);
        if (got <= 0) return false;
        buffer.append(tmp, static_cast<size_t>(got));
    }
    return true;
}

bool ReadHttpRequest(SOCKET sock, HttpRequest& out)
{
    out = {};
    std::string raw;
    size_t headerEnd = std::string::npos;
    while ((headerEnd = raw.find("\r\n\r\n")) == std::string::npos) {
        char tmp[4096];
        int got = recv(sock, tmp, static_cast<int>(sizeof(tmp)), 0);
        if (got <= 0) return false;
        raw.append(tmp, static_cast<size_t>(got));
        if (raw.size() > 1024 * 1024) return false;
    }

    const std::string headerText = raw.substr(0, headerEnd);
    std::string remaining = raw.substr(headerEnd + 4);

    size_t le = headerText.find("\r\n");
    const std::string requestLine = le == std::string::npos ? headerText : headerText.substr(0, le);
    size_t lineBegin = le == std::string::npos ? headerText.size() : le + 2;

    size_t sp1 = requestLine.find(' ');
    size_t sp2 = sp1 == std::string::npos ? std::string::npos : requestLine.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    out.method = requestLine.substr(0, sp1);
    out.path   = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);

    while (lineBegin < headerText.size()) {
        size_t l2 = headerText.find("\r\n", lineBegin);
        std::string line = l2 == std::string::npos ? headerText.substr(lineBegin)
                                                    : headerText.substr(lineBegin, l2 - lineBegin);
        lineBegin = l2 == std::string::npos ? headerText.size() : l2 + 2;
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        out.headers[ToLowerAsciiCopy(TrimAsciiCopy(line.substr(0, colon)))]
            = TrimAsciiCopy(line.substr(colon + 1));
    }

    size_t contentLength = 0;
    auto it = out.headers.find("content-length");
    if (it != out.headers.end())
        contentLength = static_cast<size_t>(std::strtoul(it->second.c_str(), nullptr, 10));
    if (contentLength > 2 * 1024 * 1024) return false;
    if (!ReadExactBytes(sock, remaining, contentLength)) return false;
    out.body = remaining.substr(0, contentLength);
    return true;
}

std::string BuildHttpResponse(int code, const char* status, const char* ct, const std::string& body)
{
    return std::format(
        "HTTP/1.1 {} {}\r\n"
        "Content-Type: {}\r\n"
        "Content-Length: {}\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: content-type, mcp-session-id\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "\r\n{}",
        code, status, ct, body.size(), body);
}

void SendHttpResponse(SOCKET sock, int code, const char* status, const char* ct, const std::string& body)
{
    std::string r = BuildHttpResponse(code, status, ct, body);
    size_t sent = 0;
    while (sent < r.size()) {
        int s = send(sock, r.data() + sent, static_cast<int>(r.size() - sent), 0);
        if (s <= 0) return;
        sent += static_cast<size_t>(s);
    }
}

// === JSON-RPC 分发 ===
bool TryBuildToolListResult(nlohmann::json& outResult, std::string& outError)
{
    try {
        outResult = {{"tools", McpTools::BuildToolsListJson()}};
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("build tools list failed: ") + ex.what();
        return false;
    }
}

bool TryBuildToolCallResult(const nlohmann::json& params, nlohmann::json& outResult, std::string& outError)
{
    if (!params.is_object()) { outError = "tools/call params must be object"; return false; }
    if (!params.contains("name") || !params["name"].is_string()) {
        outError = "tools/call requires string params.name"; return false;
    }
    const std::string toolName = params["name"].get<std::string>();
    nlohmann::json arguments = params.contains("arguments") && !params["arguments"].is_null()
        ? params["arguments"] : nlohmann::json::object();

    nlohmann::json structured;
    std::string    err;
    bool ok = McpTools::Execute(toolName, arguments, structured, err);

    // 文本内容：成功 -> structured.dump()；失败 -> err
    std::string text;
    if (ok) {
        text = structured.is_null() ? std::string("{}") : structured.dump();
    } else {
        text = err;
    }

    outResult = {
        {"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})},
        {"isError", !ok}
    };
    if (ok && !structured.is_null()) outResult["structuredContent"] = structured;
    return true;
}

bool TryHandleJsonRpc(const HttpRequest& req, int& outCode, std::string& outBody)
{
    outCode = 200;
    outBody.clear();

    nlohmann::json payload;
    try {
        payload = req.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body);
    } catch (const std::exception& ex) {
        outBody = DumpJsonSafe(BuildJsonRpcError(nullptr, -32700, std::string("parse error: ") + ex.what()));
        return true;
    }

    if (!payload.is_object()) {
        outBody = DumpJsonSafe(BuildJsonRpcError(nullptr, -32600, "request must be a JSON object"));
        return true;
    }

    const nlohmann::json id = payload.contains("id") ? payload["id"] : nlohmann::json(nullptr);
    if (!payload.contains("method") || !payload["method"].is_string()) {
        outBody = DumpJsonSafe(BuildJsonRpcError(id, -32600, "method is required"));
        return true;
    }

    const std::string method = payload["method"].get<std::string>();
    const bool hasId = payload.contains("id");
    const nlohmann::json params = payload.contains("params") ? payload["params"] : nlohmann::json::object();

    if (method == "notifications/initialized") { outCode = 202; return true; }
    if (method == "ping") {
        outBody = DumpJsonSafe(BuildJsonRpcResult(id, nlohmann::json::object()));
        return true;
    }
    if (method == "initialize") {
        outBody = DumpJsonSafe(BuildJsonRpcResult(id, BuildInitializeResult()));
        return true;
    }
    if (method == "tools/list") {
        nlohmann::json r; std::string e;
        if (!TryBuildToolListResult(r, e)) {
            outBody = DumpJsonSafe(BuildJsonRpcError(id, -32603, e)); return true;
        }
        outBody = DumpJsonSafe(BuildJsonRpcResult(id, r));
        return true;
    }
    if (method == "tools/call") {
        nlohmann::json r; std::string e;
        if (!TryBuildToolCallResult(params, r, e)) {
            outBody = DumpJsonSafe(BuildJsonRpcError(id, -32602, e)); return true;
        }
        outBody = DumpJsonSafe(BuildJsonRpcResult(id, r));
        return true;
    }
    if (!hasId) { outCode = 202; return true; }
    outBody = DumpJsonSafe(BuildJsonRpcError(id, -32601, "method not found"));
    return true;
}

void HandleClientImpl(SOCKET clientSock)
{
    DWORD timeoutMs = 5000;
    setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    HttpRequest req;
    if (!ReadHttpRequest(clientSock, req)) {
        SendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
            R"({"ok":false,"error":"invalid http request"})");
        return;
    }

    if (req.method == "OPTIONS") {
        SendHttpResponse(clientSock, 204, "No Content", "text/plain; charset=utf-8", "");
        return;
    }

    if (req.method == "GET") {
        nlohmann::json health = {
            {"ok", true},
            {"service", kServerName},
            {"version", kServerVersion},
            {"process_id", GetCurrentProcessId()},
            {"port", g_boundPort.load()},
            {"mcp_endpoint", BuildEndpointForPort(g_boundPort.load())}
        };
        SendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", DumpJsonSafe(health));
        return;
    }

    if (req.method != "POST") {
        SendHttpResponse(clientSock, 405, "Method Not Allowed", "application/json; charset=utf-8",
            R"({"ok":false,"error":"method not allowed"})");
        return;
    }

    if (req.path != "/" && req.path != "/mcp") {
        SendHttpResponse(clientSock, 404, "Not Found", "application/json; charset=utf-8",
            R"({"ok":false,"error":"not found"})");
        return;
    }

    int code = 200; std::string body;
    if (!TryHandleJsonRpc(req, code, body)) {
        SendHttpResponse(clientSock, 500, "Internal Server Error", "application/json; charset=utf-8",
            R"({"ok":false,"error":"internal server error"})");
        return;
    }
    const char* status = (code == 202) ? "Accepted" : (code == 204 ? "No Content" : "OK");
    SendHttpResponse(clientSock, code, status, "application/json; charset=utf-8", body);
}

void HandleClient(SOCKET clientSock)
{
    try {
        HandleClientImpl(clientSock);
    } catch (const std::exception& ex) {
        LogMcp(std::string("HandleClient exception: ") + ex.what());
        try { SendHttpResponse(clientSock, 500, "Internal Server Error",
            "application/json; charset=utf-8", R"({"ok":false,"error":"internal server error"})"); } catch (...) {}
    } catch (...) {
        LogMcp("HandleClient unknown exception");
        try { SendHttpResponse(clientSock, 500, "Internal Server Error",
            "application/json; charset=utf-8", R"({"ok":false,"error":"internal server error"})"); } catch (...) {}
    }
}

bool TryCreateListeningSocket(int& outPort)
{
    outPort = 0;
    for (int port = kBasePort; port < kBasePort + kMaxPortAttempts; ++port) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;

        BOOL exclusive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(static_cast<u_short>(port));
        if (inet_pton(AF_INET, kBindHost, &addr.sin_addr) != 1) { CloseSocketSafe(s); return false; }

        if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            int err = WSAGetLastError();
            CloseSocketSafe(s);
            if (err == WSAEADDRINUSE || err == WSAEACCES) continue;
            LogMcp(std::format("bind {}:{} failed, error={}", kBindHost, port, err));
            return false;
        }
        if (listen(s, SOMAXCONN) == SOCKET_ERROR) {
            int err = WSAGetLastError();
            CloseSocketSafe(s);
            LogMcp(std::format("listen {}:{} failed, error={}", kBindHost, port, err));
            return false;
        }

        { std::lock_guard<std::mutex> lock(g_stateMutex); g_listenSocket = s; }
        outPort = port;
        return true;
    }
    return false;
}

void ServerThreadMain()
{
    WSADATA wsa{}; if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { LogMcp("WSAStartup failed"); return; }

    int boundPort = 0;
    if (!TryCreateListeningSocket(boundPort)) {
        LogMcp(std::format("failed to bind {} starting at port {}", kBindHost, kBasePort));
        WSACleanup(); return;
    }

    g_boundPort.store(boundPort);
    g_running.store(true);
    LogMcp(std::format("listening on http://{}:{}/mcp", kBindHost, boundPort));

    for (;;) {
        if (g_stopRequested.load()) break;

        SOCKET listenSock; { std::lock_guard<std::mutex> lock(g_stateMutex); listenSock = g_listenSocket; }
        if (listenSock == INVALID_SOCKET) break;

        fd_set rs; FD_ZERO(&rs); FD_SET(listenSock, &rs);
        timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 250000;
        int ready = select(0, &rs, nullptr, nullptr, &tv);
        if (ready == SOCKET_ERROR) {
            if (!g_stopRequested.load()) LogMcp(std::format("select failed, error={}", WSAGetLastError()));
            break;
        }
        if (ready == 0) continue;

        sockaddr_in client{}; int clen = sizeof(client);
        SOCKET clientSock = accept(listenSock, reinterpret_cast<sockaddr*>(&client), &clen);
        if (clientSock == INVALID_SOCKET) {
            if (!g_stopRequested.load()) LogMcp(std::format("accept failed, error={}", WSAGetLastError()));
            continue;
        }
        HandleClient(clientSock);
        CloseSocketSafe(clientSock);
    }

    { std::lock_guard<std::mutex> lock(g_stateMutex); CloseSocketSafe(g_listenSocket); }
    g_running.store(false);
    g_boundPort.store(0);
    WSACleanup();
    LogMcp("stopped");
}

} // namespace

namespace LocalMcpServer {

void Initialize()
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (g_serverThread.joinable()) return;
    g_stopRequested.store(false);
    g_running.store(false);
    g_boundPort.store(0);

    // 日志文件：%TEMP%\McpFne\autolinker.log（与原版兼容命名）
    char tmp[MAX_PATH] = {}; GetTempPathA(MAX_PATH, tmp);
    std::string dir = std::string(tmp) + "McpFne";
    CreateDirectoryA(dir.c_str(), nullptr);
    Logger::Instance().Open(dir + "\\autolinker.log");

    g_serverThread = std::thread(ServerThreadMain);
}

void Shutdown()
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (!g_serverThread.joinable()) return;
        g_stopRequested.store(true);
        CloseSocketSafe(g_listenSocket);
        worker = std::move(g_serverThread);
    }
    if (worker.joinable()) worker.join();
}

bool IsRunning() { return g_running.load(); }
int  GetBoundPort() { return g_boundPort.load(); }
std::string GetEndpoint() { return BuildEndpointForPort(g_boundPort.load()); }

} // namespace LocalMcpServer