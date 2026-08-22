#pragma once

#include "AppCore.h"

#include <string_view>

class PlaybackFeedbackServer;

void MonitorMpvJsonIpc(std::wstring_view pipeName, HANDLE process,
                       PlaybackFeedbackServer& feedback,
                       bool requestBilibiliDanmaku);
