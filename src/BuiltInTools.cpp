// BuiltInTools.cpp
// 三个示例工具，演示如何扩展。用户自己的工具按同样模式调用 McpTools::Register 即可。
#include "BuiltInTools.h"

#include <Windows.h>
#include <string>

#include "McpToolRegistry.h"
#include "LocalMcpServer.h"

namespace BuiltInTools {

namespace {

void RegisterPing()
{
    McpTools::Register({
        "mcp_ping",
        "Sanity-check tool. Returns {\"pong\":true}.",
        {
            {"type", "object"},
            {"properties", nlohmann::json::object()},
            {"additionalProperties", false}
        },
        [](const nlohmann::json&, nlohmann::json& out, std::string&) -> bool {
            out = {{"pong", true}};
            return true;
        }
    });
}

void RegisterEcho()
{
    McpTools::Register({
        "mcp_echo",
        "Echoes back the arguments object you send.",
        {
            {"type", "object"},
            {"properties", {
                {"message", {{"type", "string"}, {"description", "Any text to echo back."}}}
            }},
            {"required", nlohmann::json::array({"message"})}
        },
        [](const nlohmann::json& args, nlohmann::json& out, std::string& err) -> bool {
            if (!args.contains("message") || !args["message"].is_string()) {
                err = "missing string field: message";
                return false;
            }
            out = {{"echo", args["message"].get<std::string>()}};
            return true;
        }
    });
}

void RegisterProcessInfo()
{
    McpTools::Register({
        "mcp_process_info",
        "Returns host process info: pid, exe path, MCP bound port, MCP endpoint.",
        {
            {"type", "object"},
            {"properties", nlohmann::json::object()},
            {"additionalProperties", false}
        },
        [](const nlohmann::json&, nlohmann::json& out, std::string&) -> bool {
            char exe[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exe, MAX_PATH);

            out = {
                {"pid", static_cast<int>(GetCurrentProcessId())},
                {"exe_path", std::string(exe)},
                {"mcp_port", LocalMcpServer::GetBoundPort()},
                {"mcp_endpoint", LocalMcpServer::GetEndpoint()}
            };
            return true;
        }
    });
}

} // namespace

void RegisterAll()
{
    RegisterPing();
    RegisterEcho();
    RegisterProcessInfo();
}

} // namespace BuiltInTools