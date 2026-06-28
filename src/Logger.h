// Logger.h - 极简日志：UTF-8 文件 + OutputDebugString
#pragma once

#include <fstream>
#include <mutex>
#include <string>

class Logger {
public:
    static Logger& Instance();

    // 打开日志文件（每次启动覆盖，写入 UTF-8 BOM）；父目录必须存在
    void Open(const std::string& filePath);

    // 写一行到文件
    void Write(const std::string& category, const std::string& message);

    // 写文件并同步输出到 IDE 调试器（消息为 UTF-8）
    void WriteAndIde(const std::string& category, const std::string& message);

    // 接收 GBK 字符串（来自 OutputStringToELog），转 UTF-8 后写文件
    void WriteGbk(const std::string& gbkMessage);

    static std::string BuildTimestamp();

private:
    Logger() = default;
    std::mutex   m_mutex;
    std::ofstream m_file;

    static std::string Utf8ToGbk(const std::string& text);
    static std::string GbkToUtf8(const std::string& text);
};