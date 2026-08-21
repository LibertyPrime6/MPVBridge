#pragma once

#include "Profiles.h"

#include <string_view>

void WriteDiagnosticLog(const ProfileStore& store, std::wstring_view message);
void OpenDiagnosticLog(const ProfileStore& store, HWND owner);

