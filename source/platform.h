#pragma once
#include <string>

namespace platform {
void init();
void shutdown();
// Opens the system keyboard. Returns false if unavailable or cancelled.
bool keyboardAvailable();
bool showKeyboard(const std::string &header, const std::string &initial, int maxLen, std::string &out);
bool isHandheld();
} // namespace platform
