#include "plugin/render_window.hpp"
#import <AppKit/AppKit.h>
#include <algorithm>
#include <vector>

namespace ni::plugin
{
namespace
{
std::vector<NSInteger> g_before;   // window numbers before 40015
NSInteger g_dialog = 0;            // the adopted render dialog's window number, 0 = none

bool present (NSWindow* w)         // open for the user: shown, minimised, or the whole application hidden (Cmd-H)
{
    return [w isVisible] || [w isMiniaturized] || [NSApp isHidden];
}
} // namespace

void renderWindowBeforeDialog()
{
    @autoreleasepool
    {
        g_before.clear();
        for (NSWindow* w in [NSApp windows]) g_before.push_back ([w windowNumber]);
    }
}

void renderWindowAfterDialog()
{
    @autoreleasepool
    {
        g_dialog = 0;
        for (NSWindow* w in [NSApp windows])
            if ([w isVisible] && std::find (g_before.begin(), g_before.end(), [w windowNumber]) == g_before.end())
            {
                g_dialog = [w windowNumber];
                break;
            }
        g_before.clear();
    }
}

bool renderWindowOpen()
{
    @autoreleasepool
    {
        bool tracked = false;
        for (NSWindow* w in [NSApp windows])
        {
            const bool mine = g_dialog != 0 && [w windowNumber] == g_dialog;
            if (mine) tracked = present (w);
            if ((mine || [[w title] hasPrefix:@"Render to File"]) && present (w)) return true;
        }
        if (! tracked) g_dialog = 0;   // closed (or reused later for another window): forget it
    }
    return false;
}
} // namespace ni::plugin
