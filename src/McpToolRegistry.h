// McpToolRegistry.h
// MCP 工具登记表：进程内全局，所有 tools/list、tools/call 都从这里查表。
// 用户自定义工具自行调用 McpTools::Register(...) 即可扩展。
#pragma once

#include <functional>
#include <string>

#include "..\thirdparty\json.hpp"

namespace McpTools {

struct Tool {
    std::string name;                  // 工具名（英文标识符）
    std::string description;           // 描述（给 LLM 看，可中文）
    nlohmann::json inputSchema;        // JSON Schema 对象，描述 arguments
    // handler: returns true=成功（写 outStructured），false=失败（写 outError）
    // 图片/二进制场景返回结构化字段供 LocalMcpServer 包成 content 也可以。
    std::function<bool(
        const nlohmann::json& arguments,
        nlohmann::json& outStructured,
        std::string& outError)> handler;
};

// 注册一个工具（重名以最后注册为准）
void Register(const Tool& tool);

// 取所有已注册工具的 tools/list 形态数组
nlohmann::json BuildToolsListJson();

// 按 name 找工具
bool Has(const std::string& name);

// 执行工具。成功 -> outStructured 写回结构化结果，返回 true；
// 失败 -> outError 写错误文本，返回 false。
bool Execute(const std::string& name,
             const nlohmann::json& arguments,
             nlohmann::json& outStructured,
             std::string& outError);

} // namespace McpTools