// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-FileCopyrightText: 2026 Caracu contributors
// SPDX-License-Identifier: GPL-3.0+

// UI-only callbacks adapted from the upstream host contract. VM, queue, and
// render-surface callbacks are implemented separately, never as success stubs.
#include "pcsx2/Achievements.h"
#include "pcsx2/GS.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/VMManager.h"
#include "common/ProgressCallback.h"

#include <cstdio>

void Host::CommitBaseSettingChanges() {} // Deliberately memory-only settings.
void Host::LoadSettings(SettingsInterface&, std::unique_lock<std::mutex>&) {}
void Host::CheckForSettingsChanges(const Pcsx2Config&) {}
bool Host::RequestResetSettings(bool, bool, bool, bool, bool) { return false; }
void Host::SetDefaultUISettings(SettingsInterface&) {}
std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}
void Host::ReportInfoAsync(std::string_view title, std::string_view message)
{
	fmt::print(stderr, "{}: {}\n", title, message);
}
void Host::ReportErrorAsync(std::string_view title, std::string_view message)
{
	fmt::print(stderr, "ERROR {}: {}\n", title, message);
}
void Host::OpenURL(std::string_view) {} // No external applications or browser.
bool Host::InBatchMode() { return true; }
bool Host::InNoGUIMode() { return true; }
bool Host::CopyTextToClipboard(std::string_view) { return false; }
std::string Host::GetTextFromClipboard() { return {}; }
void Host::BeginTextInput() {}
void Host::EndTextInput() {}
std::optional<WindowInfo> Host::GetTopLevelWindowInfo() { return std::nullopt; }
void Host::OnInputDeviceConnected(std::string_view, std::string_view) {}
void Host::OnInputDeviceDisconnected(InputBindingKey, std::string_view) {}
void Host::SetMouseMode(bool, bool) {}
void Host::SetMouseLock(bool) {}
void Host::ReleaseRenderWindow() {}
void Host::BeginPresentFrame() {}
void Host::RequestResizeHostDisplay(s32, s32) {}
void Host::OnVMStarting() {}
void Host::OnVMStarted() {}
void Host::OnVMDestroyed() {}
void Host::OnVMPaused() {}
void Host::OnVMResumed() {}
void Host::OnGameChanged(const std::string&, const std::string&, const std::string&,
	const std::string&, u32, u32) {}
void Host::OnPerformanceMetricsUpdated() {}
void Host::OnSaveStateLoading(std::string_view) {}
void Host::OnSaveStateLoaded(std::string_view, bool) {}
void Host::OnSaveStateSaved(std::string_view) {}
void Host::RefreshGameListAsync(bool) {}
void Host::CancelGameListRefresh() {}
bool Host::IsFullscreen() { return false; }
void Host::SetFullscreen(bool) {}
void Host::OnCaptureStarted(const std::string&) {}
void Host::OnCaptureStopped() {}
void Host::RequestExitBigPicture() {}
s32 Host::Internal::GetTranslatedStringImpl(std::string_view, std::string_view msg, char* buf, size_t size)
{
	if (msg.size() > size)
		return -1;
	if (!msg.empty())
		std::memcpy(buf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}
std::string Host::TranslatePluralToString(const char*, const char* msg, const char*, int count)
{
	std::string result(msg);
	const std::string number = std::to_string(count);
	for (size_t pos = 0; (pos = result.find("%n", pos)) != std::string::npos; pos += number.size())
		result.replace(pos, 2, number);
	return result;
}
void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason) {}
void Host::OnAchievementsLoginSuccess(const char*, u32, u32, u32) {}
void Host::OnAchievementsRefreshed() {}
void Host::OnAchievementsHardcoreModeChanged(bool) {}
bool Host::LocaleCircleConfirm() { return false; }
bool Host::ShouldPreferHostFileSelector() { return false; }
void Host::OpenHostFileSelectorAsync(std::string_view, bool, FileSelectorCallback callback,
	FileSelectorFilters, std::string_view)
{
	callback({});
}
int Host::LocaleSensitiveCompare(std::string_view a, std::string_view b) { return a.compare(b); }
std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(std::string_view) { return std::nullopt; }
std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32) { return std::nullopt; }
const char* InputManager::ConvertHostKeyboardCodeToIcon(u32) { return nullptr; }
BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()
