#include "platform.h"
#include "common.h"

#ifdef __SWITCH__
#include <switch.h>

namespace platform {

void init() {
    romfsInit();
    makeDirs(userPath(""));
}

void shutdown() { romfsExit(); }

bool keyboardAvailable() { return true; }

bool showKeyboard(const std::string &header, const std::string &initial, int maxLen, std::string &out) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header.c_str());
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, maxLen);
    char buf[256] = {0};
    Result rc = swkbdShow(&kbd, buf, sizeof buf);
    swkbdClose(&kbd);
    if (R_FAILED(rc))
        return false;
    out = buf;
    return true;
}

bool isHandheld() { return appletGetOperationMode() == AppletOperationMode_Handheld; }

} // namespace platform

#else

namespace platform {
void init() { makeDirs(userPath("")); }
void shutdown() {}
bool keyboardAvailable() { return false; }
bool showKeyboard(const std::string &, const std::string &, int, std::string &) { return false; }
bool isHandheld() { return false; }
} // namespace platform

#endif
