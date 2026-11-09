// Render-dialog title matcher (port plan Task 8): the title check that backs up identity-first detection. True when the
// window title starts with the LangPack title (when known) or with REAPER's English "Render to File". Titles are UTF-8.
#pragma once
#include <string_view>

namespace ni::host::platform
{
inline bool renderTitleMatches(std::string_view windowTitle, std::string_view langpackTitle) noexcept
{
    constexpr std::string_view kEnglish = "Render to File";
    if (!langpackTitle.empty() && windowTitle.substr(0, langpackTitle.size()) == langpackTitle) return true;
    return windowTitle.substr(0, kEnglish.size()) == kEnglish;
}
} // namespace ni::host::platform
