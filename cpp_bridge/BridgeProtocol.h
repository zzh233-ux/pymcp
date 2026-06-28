#pragma once

// BridgeProtocol.h — 命名管道 IPC 协议定义
// 协议格式: [4字节小端uint32长度][UTF-8 JSON载荷]

#include <windows.h>
#include <cstdint>
#include <string>
#include "../thirdparty/json.hpp"

namespace BridgeProtocol {

// 默认管道名称
inline constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\AutoLinkerBridge";

// 协议常量
inline constexpr uint32_t kHeaderSize  = 4;                       // 长度头大小
inline constexpr uint32_t kMaxPayload  = 16u * 1024 * 1024;       // 最大载荷 16MB
inline constexpr uint32_t kBufferSize  = 65536;                   // 管道缓冲区 64KB

// ── 消息结构 ──────────────────────────────────────────────

// 工具请求 (Python → C++)
struct ToolRequest {
    std::string         id;          // 请求ID(用于关联响应)
    std::string         tool;        // 工具名称
    nlohmann::json      arguments;   // 工具参数
};

// 工具响应 (C++ → Python)
struct ToolResponse {
    std::string         id;
    bool                ok      = true;
    nlohmann::json      result;       // 成功时的结果
    nlohmann::json      error;        // 失败时的错误信息
};

// ── JSON 序列化 ──────────────────────────────────────────

inline nlohmann::json RequestToJson(const ToolRequest& req) {
    return {
        {"id", req.id},
        {"tool", req.tool},
        {"arguments", req.arguments.is_null() ? nlohmann::json::object() : req.arguments},
    };
}

inline ToolRequest JsonToRequest(const nlohmann::json& j) {
    ToolRequest req;
    req.id        = j.value("id", "");
    req.tool      = j.value("tool", "");
    req.arguments = j.value("arguments", nlohmann::json::object());
    return req;
}

inline nlohmann::json ResponseToJson(const ToolResponse& resp) {
    nlohmann::json j = {
        {"id", resp.id},
        {"ok", resp.ok},
    };
    if (resp.ok) {
        j["result"] = resp.result.is_null() ? nlohmann::json::object() : resp.result;
        j["error"]  = nullptr;
    } else {
        j["result"] = nullptr;
        j["error"]  = resp.error.is_null() ? nlohmann::json::object() : resp.error;
    }
    return j;
}

// ── 管道 I/O ─────────────────────────────────────────────

// 写一条长度前缀 JSON 消息到管道
inline bool WriteMessage(HANDLE pipe, const nlohmann::json& j) {
    std::string payload;
    try {
        payload = j.dump();
    } catch (...) {
        return false;
    }

    if (payload.size() > kMaxPayload)
        return false;

    // 4字节小端长度头
    uint32_t len = static_cast<uint32_t>(payload.size());
    char header[kHeaderSize];
    header[0] = static_cast<char>(len & 0xFF);
    header[1] = static_cast<char>((len >> 8) & 0xFF);
    header[2] = static_cast<char>((len >> 16) & 0xFF);
    header[3] = static_cast<char>((len >> 24) & 0xFF);

    // 写 header
    DWORD written = 0;
    if (!WriteFile(pipe, header, kHeaderSize, &written, nullptr))
        return false;
    if (written != kHeaderSize)
        return false;

    // 写 payload(可能需要多次 WriteFile)
    const char* p = payload.data();
    uint32_t remaining = len;
    while (remaining > 0) {
        DWORD toWrite = (remaining > kBufferSize) ? kBufferSize : remaining;
        DWORD bytesWritten = 0;
        if (!WriteFile(pipe, p, toWrite, &bytesWritten, nullptr))
            return false;
        p += bytesWritten;
        remaining -= bytesWritten;
    }
    return true;
}

// 从管道读一条长度前缀 JSON 消息
inline bool ReadMessage(HANDLE pipe, nlohmann::json& out) {
    // 读 4 字节长度头
    char header[kHeaderSize];
    DWORD totalRead = 0;
    while (totalRead < kHeaderSize) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(pipe, header + totalRead, kHeaderSize - totalRead, &bytesRead, nullptr);
        if (!ok || bytesRead == 0)
            return false;   // 管道关闭或出错
        totalRead += bytesRead;
    }

    uint32_t len = static_cast<unsigned char>(header[0])
                 | (static_cast<unsigned char>(header[1]) << 8)
                 | (static_cast<unsigned char>(header[2]) << 16)
                 | (static_cast<unsigned char>(header[3]) << 24);

    if (len == 0 || len > kMaxPayload)
        return false;

    // 读 payload
    std::string payload;
    payload.resize(len);
    DWORD totalReadPayload = 0;
    while (totalReadPayload < len) {
        DWORD toRead = (len - totalReadPayload > kBufferSize) ? kBufferSize : (len - totalReadPayload);
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(pipe, &payload[totalReadPayload], toRead, &bytesRead, nullptr);
        if (!ok || bytesRead == 0)
            return false;
        totalReadPayload += bytesRead;
    }

    try {
        out = nlohmann::json::parse(payload);
    } catch (...) {
        return false;
    }
    return true;
}

} // namespace BridgeProtocol
