// Logger.cpp
#include "Logger.h"

#include <Windows.h>
#include <chrono>
#include <ctime>
#include <sstream>
#include <system_error>

// OutputStringToELog 声明在 McpFne.h，供入口处使用
#include "McpFne.h"

namespace {

std::string ConvertCpp(int srcCodePage, int dstCodePage, const std::string& text)
{
    if (text.empty()) return std::string();

    int wideLen = MultiByteToWideChar(srcCodePage, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (wideLen <= 0) return text;

    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    if (MultiByteToWideChar(srcCodePage, 0, text.data(),
            static_cast<int>(text.size()), wide.data(), wideLen) <= 0) {
        return text;
    }

    int outLen = WideCharToMultiByte(dstCodePage, 0, wide.data(), wideLen,
        nullptr, 0, nullptr, nullptr);
    if (outLen <= 0) return text;

    std::string out(static_cast<size_t>(outLen), '\0');
    if (WideCharToMultiByte(dstCodePage, 0, wide.data(), wideLen,
            out.data(), outLen, nullptr, nullptr) <= 0) {
        return text;
    }
    return out;
}

} // namespace

Logger& Logger::Instance()
{
    static Logger inst;
    return inst;
}

void Logger::Open(const std::string& filePath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open()) m_file.close();
    m_file.open(filePath, std::ios::binary | std::ios::trunc);
    if (m_file.is_open()) {
        const char kBom[3] = { static_cast<char>(0xEF), static_cast<char>(0xBB), static_cast<char>(0xBF) };
        m_file.write(kBom, 3);
    }
}

std::string Logger::BuildTimestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t  = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    char buf2[80];
    std::snprintf(buf2, sizeof(buf2), "%s.%03lld", buf, static_cast<long long>(ms.count()));
    return buf2;
}

void Logger::Write(const std::string& category, const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file.is_open()) return;
    std::string line = "[" + BuildTimestamp() + "]";
    if (!category.empty()) line += " [" + category + "]";
    line += " " + message + "\r\n";
    m_file.write(line.data(), static_cast<std::streamsize>(line.size()));
    m_file.flush();
}

void Logger::WriteGbk(const std::string& gbkMessage)
{
    Write("", GbkToUtf8(gbkMessage));
}

void Logger::WriteAndIde(const std::string& category, const std::string& message)
{
    Write(category, message);
    OutputStringToELog(Utf8ToGbk("[" + category + "] " + message));
}

std::string Logger::Utf8ToGbk(const std::string& text)
{
    return ConvertCpp(CP_UTF8, 936, text);
}

std::string Logger::GbkToUtf8(const std::string& text)
{
    return ConvertCpp(936, CP_UTF8, text);
}

// === OutputStringToELog 实现 ===
// 精简版：写 OutputDebugStringA + 日志文件。这里 IDE 输出窗口附加需要逆向找 Edit 控件，
// 为保持零依赖，不实现 IDE 输出窗口附加；如需可参照 AutoLinker 的 IDEFacade::AppendOutputWindowLine 自行扩展。
void OutputStringToELog(const std::string& szbuf)
{
    std::string line = "[McpFne]" + szbuf;
    OutputDebugStringA((line + "\n").c_str());
    Logger::Instance().WriteGbk(line);
}