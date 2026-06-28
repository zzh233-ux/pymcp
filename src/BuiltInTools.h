// BuiltInTools.h
#pragma once

namespace BuiltInTools {
// 把内置示例工具注册到 McpTools。
// 目前包含：
//   mcp_ping        - 回 "pong"，验证链路
//   mcp_echo        - 原样回显 arguments
//   mcp_process_info- 返回当前宿主进程信息（PID / 路径 / MCP 端口）
void RegisterAll();
} // namespace BuiltInTools