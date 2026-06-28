// McpToolRegistry.cpp
#include "McpToolRegistry.h"

#include <mutex>
#include <unordered_map>

namespace McpTools {

namespace {
std::mutex g_mutex;
std::unordered_map<std::string, Tool> g_tools;
std::vector<std::string> g_order;  // 保持登记顺序，方便稳定输出
} // namespace

void Register(const Tool& tool)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_tools.find(tool.name) == g_tools.end()) {
        g_order.push_back(tool.name);
    }
    g_tools[tool.name] = tool;
}

nlohmann::json BuildToolsListJson()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& name : g_order) {
        auto it = g_tools.find(name);
        if (it == g_tools.end()) continue;
        const Tool& t = it->second;
        nlohmann::json entry = {
            {"name", t.name},
            {"description", t.description},
            {"inputSchema", t.inputSchema.is_null() ? nlohmann::json::object() : t.inputSchema},
        };
        arr.push_back(std::move(entry));
    }
    return arr;
}

bool Has(const std::string& name)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_tools.find(name) != g_tools.end();
}

bool Execute(const std::string& name,
             const nlohmann::json& arguments,
             nlohmann::json& outStructured,
             std::string& outError)
{
    Tool toolCopy;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_tools.find(name);
        if (it == g_tools.end()) {
            outError = "tool not found: " + name;
            return false;
        }
        toolCopy = it->second;  // 复制避免持锁回调
    }
    if (!toolCopy.handler) {
        outError = "tool has no handler: " + name;
        return false;
    }
    try {
        return toolCopy.handler(arguments, outStructured, outError);
    } catch (const std::exception& ex) {
        outError = std::string("tool exception: ") + ex.what();
        return false;
    } catch (...) {
        outError = "tool unknown exception";
        return false;
    }
}

} // namespace McpTools