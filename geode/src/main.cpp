#include <Geode/Geode.hpp>
#include <Geode/loader/Dirs.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/ui/Notification.hpp>

#include <atomic>
#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace geode::prelude;

namespace {
std::atomic_bool g_analysisRunning = false;

std::filesystem::path findExistingFile(std::vector<std::filesystem::path> const& candidates) {
    std::error_code error;

    for (auto const& candidate : candidates) {
        if (candidate.empty()) {
            continue;
        }

        if (std::filesystem::exists(candidate, error) && std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }

        error.clear();
    }

    return {};
}

std::filesystem::path resolveAudioPath(GJGameLevel* level) {
    if (!level) {
        return {};
    }

    std::string audioFileName = level->getAudioFileName();
    std::filesystem::path rawPath = audioFileName;

    std::vector<std::filesystem::path> candidates;

    if (!rawPath.empty()) {
        candidates.push_back(rawPath);

        if (rawPath.is_relative()) {
            candidates.push_back(geode::dirs::getSaveDir() / rawPath);
            candidates.push_back(geode::dirs::getResourcesDir() / rawPath);
            candidates.push_back(geode::dirs::getGameDir() / rawPath);
        }
    }

    if (level->m_songID > 0) {
        auto songID = std::to_string(level->m_songID);
        auto saveDirectory = geode::dirs::getSaveDir();

        candidates.push_back(saveDirectory / (songID + ".mp3"));
        candidates.push_back(saveDirectory / (songID + ".ogg"));
        candidates.push_back(saveDirectory / (songID + ".wav"));
        candidates.push_back(saveDirectory / (songID + ".m4a"));
    }

    return findExistingFile(candidates);
}

std::string makeSongKey(GJGameLevel* level) {
    if (!level) {
        return "unknown";
    }

    if (level->m_songID > 0) {
        return "custom-" + std::to_string(level->m_songID);
    }

    return "official-" + std::to_string(level->m_audioTrack);
}

std::string makeFileSignature(std::filesystem::path const& path) {
    std::error_code error;

    auto fileSize = std::filesystem::file_size(path, error);
    if (error) {
        fileSize = 0;
        error.clear();
    }

    auto writeTime = std::filesystem::last_write_time(path, error);
    auto writeCount = error ? 0 : writeTime.time_since_epoch().count();

    return std::to_string(fileSize) + "-" + std::to_string(writeCount);
}

#ifdef GEODE_IS_WINDOWS
std::wstring quoteWindowsArgument(std::wstring const& value) {
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return value;
    }

    std::wstring escaped;
    escaped.push_back(L'"');

    for (auto iterator = value.begin(); ; ++iterator) {
        std::size_t backslashCount = 0;

        while (iterator != value.end() && *iterator == L'\\') {
            ++backslashCount;
            ++iterator;
        }

        if (iterator == value.end()) {
            escaped.append(backslashCount * 2, L'\\');
            break;
        }

        if (*iterator == L'"') {
            escaped.append(backslashCount * 2 + 1, L'\\');
            escaped.push_back(L'"');
        } else {
            escaped.append(backslashCount, L'\\');
            escaped.push_back(*iterator);
        }
    }

    escaped.push_back(L'"');
    return escaped;
}

int runBackendProcess(
    std::filesystem::path const& backendPath,
    std::filesystem::path const& audioPath,
    std::filesystem::path const& analysisPath,
    std::filesystem::path const& previewPath,
    std::filesystem::path const& logPath
) {
    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    HANDLE logHandle = CreateFileW(
        logPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &securityAttributes,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (logHandle == INVALID_HANDLE_VALUE) {
        return 10000 + static_cast<int>(GetLastError());
    }

    SetFilePointer(logHandle, 0, nullptr, FILE_END);

    std::vector<std::wstring> arguments = {
        backendPath.wstring(),
        L"audio",
        L"analyze",
        audioPath.wstring(),
        L"--out",
        analysisPath.wstring(),
        L"--beat-preview",
        previewPath.wstring(),
        L"--summary",
    };

    std::wstring commandLine;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index != 0) {
            commandLine.push_back(L' ');
        }

        commandLine += quoteWindowsArgument(arguments[index]);
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = logHandle;
    startupInfo.hStdError = logHandle;

    PROCESS_INFORMATION processInfo{};
    std::wstring mutableCommandLine = commandLine;

    BOOL created = CreateProcessW(
        backendPath.c_str(),
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        backendPath.parent_path().c_str(),
        &startupInfo,
        &processInfo
    );

    if (!created) {
        DWORD errorCode = GetLastError();
        CloseHandle(logHandle);

        std::ofstream logFile(logPath, std::ios::out | std::ios::app);
        if (logFile) {
            logFile << "CreateProcessW failed with Windows error " << errorCode << "\n";
        }

        return 10000 + static_cast<int>(errorCode);
    }

    CloseHandle(logHandle);

    WaitForSingleObject(processInfo.hProcess, INFINITE);

    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    return static_cast<int>(exitCode);
}
#endif

void showNotification(std::string const& message, NotificationIcon icon, float duration = 3.0f) {
    Notification::create(message, icon, duration)->show();
}
}

class $modify(GDAIEditorUI, EditorUI) {
    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) {
            return false;
        }

        NodeIDs::provideFor(this);

        auto menu = this->getChildByID("editor-buttons-menu");
        if (!menu) {
            log::error("GD AI Editor could not find editor-buttons-menu");
            return true;
        }

        auto buttonSprite = EditorButtonSprite::createWithSpriteFrameName(
            "GJ_musicOnBtn_001.png",
            0.55f,
            EditorBaseColor::LightBlue
        );

        if (!buttonSprite) {
            log::error("GD AI Editor could not create its editor button sprite");
            return true;
        }

        auto button = CCMenuItemSpriteExtra::create(
            buttonSprite,
            this,
            menu_selector(GDAIEditorUI::onGDAIEditor)
        );

        button->setID("analyze-song-button"_spr);
        menu->addChild(button);
        menu->updateLayout();

        if (this->m_uiItems) {
            this->m_uiItems->addObject(button);
        }

        return true;
    }

    void onGDAIEditor(CCObject*) {
        if (g_analysisRunning.exchange(true)) {
            showNotification("Song analysis is already running.", NotificationIcon::Info);
            return;
        }

        auto finishEarly = [] {
            g_analysisRunning = false;
        };

        auto level = this->m_editorLayer ? this->m_editorLayer->m_level : nullptr;
        if (!level) {
            finishEarly();
            FLAlertLayer::create(
                "GD AI Editor",
                "Could not read the current level.",
                "OK"
            )->show();
            return;
        }

        auto audioPath = resolveAudioPath(level);
        if (audioPath.empty()) {
            finishEarly();

            auto message = level->m_songID > 0
                ? "I found this level's custom song ID, but the audio file is not downloaded locally.<br><br>Download the song normally in Geometry Dash, then press the GD AI button again."
                : "I could not resolve this level's audio file.";

            FLAlertLayer::create(
                "Song Not Found",
                message,
                "OK"
            )->show();
            return;
        }

        auto backendPath = Mod::get()->getResourcesDir() / "gd-ai-backend.exe";
        if (!std::filesystem::exists(backendPath)) {
            finishEarly();
            FLAlertLayer::create(
                "Backend Missing",
                "This development build does not contain <cy>gd-ai-backend.exe</c>.<br><br>Use the GitHub Actions build named <cg>GD AI Editor - Windows</c>, or build with <cy>scripts/build-geode.ps1</c>.",
                "OK"
            )->show();
            return;
        }

        auto songDirectory = Mod::get()->getSaveDir() / "song-cache" / makeSongKey(level);
        auto signature = makeFileSignature(audioPath);

        auto analysisPath = songDirectory / ("analysis-" + signature + ".json");
        auto previewPath = songDirectory / ("beats-" + signature + ".wav");
        auto logPath = songDirectory / ("analysis-" + signature + ".log");

        if (std::filesystem::exists(analysisPath)) {
            finishEarly();
            showNotification("Song analysis already cached.", NotificationIcon::Success, 2.5f);
            log::info("Using cached song analysis: {}", analysisPath.string());
            return;
        }

        std::error_code directoryError;
        std::filesystem::create_directories(songDirectory, directoryError);

        if (directoryError) {
            finishEarly();
            FLAlertLayer::create(
                "GD AI Editor",
                "Could not create the song analysis cache directory.",
                "OK"
            )->show();
            return;
        }

        showNotification("Analyzing current song...", NotificationIcon::Loading, 2.5f);

        log::info("Analyzing song: {}", audioPath.string());
        log::info("Analysis output: {}", analysisPath.string());

        std::thread([
            backendPath,
            audioPath,
            analysisPath,
            previewPath,
            logPath
        ] {
            {
                std::ofstream logFile(logPath, std::ios::out | std::ios::trunc);
                if (logFile) {
                    logFile
                        << "GD AI Editor backend launch\n"
                        << "Backend: " << backendPath.string() << "\n"
                        << "Audio: " << audioPath.string() << "\n"
                        << "Analysis: " << analysisPath.string() << "\n"
                        << "Preview: " << previewPath.string() << "\n\n";
                }
            }

#ifdef GEODE_IS_WINDOWS
            int exitCode = runBackendProcess(
                backendPath,
                audioPath,
                analysisPath,
                previewPath,
                logPath
            );
#else
            int exitCode = -1;
#endif
            bool success = exitCode == 0 && std::filesystem::exists(analysisPath);

            g_analysisRunning = false;

            Loader::get()->queueInMainThread([
                success,
                exitCode,
                analysisPath,
                previewPath,
                logPath
            ] {
                if (success) {
                    showNotification(
                        "Song analyzed and cached.",
                        NotificationIcon::Success,
                        4.0f
                    );

                    log::info("Song analysis complete: {}", analysisPath.string());
                    log::info("Beat preview: {}", previewPath.string());
                } else {
                    showNotification(
                        "Song analysis failed. Check the Geode log.",
                        NotificationIcon::Error,
                        5.0f
                    );

                    log::error(
                        "Song analysis failed with exit code {}. Backend log: {}",
                        exitCode,
                        logPath.string()
                    );

                    FLAlertLayer::create(
                        "Analysis Failed",
                        fmt::format(
                            "The audio backend exited with code <cr>{}</c>.<br><br>A diagnostic log was written to:<br><cy>{}</c>",
                            exitCode,
                            logPath.string()
                        ),
                        "OK"
                    )->show();
                }
            });
        }).detach();
    }
};
