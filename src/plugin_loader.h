#pragma once
#include <string>
#include <vector>

// UTF-8 string (from .pfl files) -> UTF-16 wstring (for Win32 API)
std::wstring Utf8ToWide(const std::string& utf8);

struct Plugin {
    std::string id;       // filename stem (e.g. "1-shutdown")
    std::string display;  // first line of .pfl (e.g. "关机")
    std::string command;  // remaining lines (SSH command or "action:xxx")

    bool isAction() const { return command.rfind("action:", 0) == 0; }
    std::string actionName() const;  // returns "" if not an action
    std::wstring displayW() const { return Utf8ToWide(display); }
};

class PluginLoader {
public:
    static std::vector<Plugin> loadFromDirectory(const std::string& dirPath);
};
