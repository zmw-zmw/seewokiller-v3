#include "plugin_loader.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace fs = std::filesystem;

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(),
                                   nullptr, 0);
    if (len <= 0) return {};
    std::wstring wstr(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(),
                        &wstr[0], len);
    return wstr;
}

std::string Plugin::actionName() const {
    if (!isAction()) return {};
    return command.substr(7); // after "action:"
}

std::vector<Plugin> PluginLoader::loadFromDirectory(const std::string& dirPath) {
    std::vector<Plugin> plugins;

    std::error_code ec;
    if (!fs::is_directory(dirPath, ec)) return plugins;

    for (auto& entry : fs::directory_iterator(dirPath, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        if (path.extension() != ".pfl") continue;

        std::ifstream file(path);
        if (!file.is_open()) continue;

        std::stringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();

        auto firstLf = content.find('\n');
        if (firstLf == std::string::npos) continue;

        Plugin p;
        p.id = path.stem().string();

        p.display = content.substr(0, firstLf);
        p.display.erase(p.display.find_last_not_of(" \r\n\t") + 1);
        if (p.display.empty()) continue;

        auto cmdStart = content.find_first_not_of("\r\n", firstLf);
        if (cmdStart != std::string::npos) {
            p.command = content.substr(cmdStart);
            while (!p.command.empty() &&
                   (p.command.back() == '\n' || p.command.back() == '\r' ||
                    p.command.back() == ' ' || p.command.back() == '\t')) {
                p.command.pop_back();
            }
        }

        plugins.push_back(std::move(p));
    }

    std::sort(plugins.begin(), plugins.end(),
              [](const Plugin& a, const Plugin& b) { return a.id < b.id; });

    return plugins;
}
