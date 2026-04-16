#include "SystemAppearance.hpp"

#import <AppKit/AppKit.h>
#include <functional>

static std::function<void()> s_callback;

@interface AppearanceObserver : NSObject
@end

@implementation AppearanceObserver
- (void)appearanceChanged
{
    if (s_callback)
        s_callback();
}
@end

static AppearanceObserver *s_observer = nil;

namespace SystemAppearance
{
    bool IsDark()
    {
        NSAppearanceName name = [[NSApp effectiveAppearance]
            bestMatchFromAppearancesWithNames:@[
                NSAppearanceNameAqua,
                NSAppearanceNameDarkAqua
            ]];
        return [name isEqualToString:NSAppearanceNameDarkAqua];
    }

    void SetChangeCallback(std::function<void()> cb)
    {
        s_callback = std::move(cb);
        if (!s_observer)
        {
            s_observer = [[AppearanceObserver alloc] init];
            [[NSDistributedNotificationCenter defaultCenter]
                addObserver:s_observer
                   selector:@selector(appearanceChanged)
                       name:@"AppleInterfaceThemeChangedNotification"
                     object:nil];
        }
    }

    void ClearChangeCallback()
    {
        if (s_observer)
        {
            [[NSDistributedNotificationCenter defaultCenter]
                removeObserver:s_observer];
            s_observer = nil;
        }
        s_callback = nullptr;
    }
}
