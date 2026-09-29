#include <Geode/Geode.hpp>
#include <Geode/loader/Dirs.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/ui/Notification.hpp>

#include <atomic>
#include <cstdlib>
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

std::string quoteCommandArgument(std::filesystem::path const& path) {
    auto value = path.string();

    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');

    for (char character : value) {
        if (character == '"') {
            escaped += "\\\"";
        } else {
            escaped.push_back(character);
        }
    }

    escaped.push_back('"');
    return escaped;
}

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

            auto command =
                quoteCommandArgument(backendPath)
                + " audio analyze "
                + quoteCommandArgument(audioPath)
                + " --out "
                + quoteCommandArgument(analysisPath)
                + " --beat-preview "
                + quoteCommandArgument(previewPath)
                + " --summary >> "
                + quoteCommandArgument(logPath)
                + " 2>&1";

            int exitCode = std::system(command.c_str());
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
