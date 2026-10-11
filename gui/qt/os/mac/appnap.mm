#include "appnap.h"

#import <Foundation/Foundation.h>

void disableAppNap() {
    // The returned activity token must stay alive for as long as the
    // process should be exempt from App Nap; it's intentionally never
    // passed to endActivity: so the effect lasts for the whole run.
    static id activityToken = [[[NSProcessInfo processInfo]
        beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                           reason:@"CEmu emulates in real time and must run at full speed "
                                  @"continuously, including while unfocused (e.g. driven "
                                  @"by the MCP dock, or during a file transfer)."] retain];
    (void)activityToken;
}
