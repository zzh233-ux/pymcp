// LocalMcpServer.h - 纯 MCP HTTP/JSON-RPC 2.0 服务端（精简版，无 IDEFacade/AIChat 依赖）
//
// 监听 127.0.0.1:19207/mcp，端口被占用自动递增到 19208..19222。
// 协议版本 2024-11-05。支持方法：initialize / notifications/initialized / ping / tools/list / tools/call。
// 工具来源：McpTools::Register(...)，本文件不关心具体工具实现。
#pragma once

#include <string>

namespace LocalMcpServer {

void Initialize();
void Shutdown();
bool IsRunning();
int  GetBoundPort();
std::string GetEndpoint();

} // namespace LocalMcpServer