#include <Geode/Geode.hpp>
#include <Geode/loader/Dirs.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/ui/Notification.hpp>

#include "BaselineGenerator.hpp"
#include "GameplayExport.hpp"
#include "LlmGenerator.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
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
std::atomic_bool g_exportRunning = false;
std::atomic_bool g_generationRunning = false;

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

int runWindowsCommand(
    std::vector<std::wstring> const& arguments,
    std::filesystem::path const& workingDirectory,
    std::filesystem::path const& logPath
) {
    if (arguments.empty()) {
        return -1;
    }

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
        arguments.front().c_str(),
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDirectory.c_str(),
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

int runBackendProcess(
    std::filesystem::path const& backendPath,
    std::filesystem::path const& audioPath,
    std::filesystem::path const& analysisPath,
    std::filesystem::path const& previewPath,
    std::filesystem::path const& logPath
) {
    return runWindowsCommand(
        {
            backendPath.wstring(),
            L"audio",
            L"analyze",
            audioPath.wstring(),
            L"--out",
            analysisPath.wstring(),
            L"--beat-preview",
            previewPath.wstring(),
            L"--summary",
        },
        backendPath.parent_path(),
        logPath
    );
}

int runGameplayAlignProcess(
    std::filesystem::path const& backendPath,
    std::filesystem::path const& rawGameplayPath,
    std::filesystem::path const& analysisPath,
    std::filesystem::path const& alignedGameplayPath,
    std::filesystem::path const& logPath
) {
    return runWindowsCommand(
        {
            backendPath.wstring(),
            L"gameplay",
            L"align",
            rawGameplayPath.wstring(),
            analysisPath.wstring(),
            L"--out",
            alignedGameplayPath.wstring(),
        },
        backendPath.parent_path(),
        logPath
    );
}

int runLlmPlanProcess(
    std::filesystem::path const& backendPath,
    std::filesystem::path const& analysisPath,
    std::filesystem::path const& trainingDirectory,
    std::filesystem::path const& cachePath,
    std::filesystem::path const& planPath,
    std::filesystem::path const& logPath,
    std::string const& difficulty,
    std::string const& model,
    std::string const& reasoningEffort,
    double songOffset
) {
    return runWindowsCommand(
        {
            backendPath.wstring(),
            L"gameplay",
            L"plan-layout",
            analysisPath.wstring(),
            trainingDirectory.wstring(),
            L"--cache",
            cachePath.wstring(),
            L"--out",
            planPath.wstring(),
            L"--difficulty",
            std::filesystem::path(difficulty).wstring(),
            L"--song-offset",
            std::filesystem::path(fmt::format("{:.6f}", songOffset)).wstring(),
            L"--model",
            std::filesystem::path(model).wstring(),
            L"--reasoning-effort",
            std::filesystem::path(reasoningEffort).wstring(),
        },
        backendPath.parent_path(),
        logPath
    );
}

#endif


std::string exportStem(GJGameLevel* level) {
    std::string name = level ? std::string(level->m_levelName) : "level";

    for (char& character : name) {
        bool safe =
            (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '-'
            || character == '_';

        if (!safe) {
            character = '_';
        }
    }

    if (name.empty()) {
        name = "level";
    }

    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    return fmt::format("{}-{}", name, milliseconds);
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

        auto analyzeSprite = EditorButtonSprite::createWithSpriteFrameName(
            "GJ_musicOnBtn_001.png",
            0.55f,
            EditorBaseColor::LightBlue
        );
        auto exportSprite = EditorButtonSprite::createWithSpriteFrameName(
            "GJ_editBtn_001.png",
            0.42f,
            EditorBaseColor::LightBlue
        );
        auto generateSprite = EditorButtonSprite::createWithSpriteFrameName(
            "GJ_plusBtn_001.png",
            0.42f,
            EditorBaseColor::LightBlue
        );

        if (!analyzeSprite || !exportSprite || !generateSprite) {
            log::error("GD AI Editor could not create its editor button sprites");
            return true;
        }

        auto analyzeButton = CCMenuItemSpriteExtra::create(
            analyzeSprite,
            this,
            menu_selector(GDAIEditorUI::onGDAIEditor)
        );
        auto exportButton = CCMenuItemSpriteExtra::create(
            exportSprite,
            this,
            menu_selector(GDAIEditorUI::onExportGameplay)
        );
        auto generateButton = CCMenuItemSpriteExtra::create(
            generateSprite,
            this,
            menu_selector(GDAIEditorUI::onGenerateBaseline)
        );

        analyzeButton->setID("analyze-song-button"_spr);
        exportButton->setID("export-gameplay-button"_spr);
        generateButton->setID("generate-baseline-button"_spr);

        menu->addChild(analyzeButton);
        menu->addChild(exportButton);
        menu->addChild(generateButton);
        menu->updateLayout();

        if (this->m_uiItems) {
            this->m_uiItems->addObject(analyzeButton);
            this->m_uiItems->addObject(exportButton);
            this->m_uiItems->addObject(generateButton);
        }

        return true;
    }

    void onGenerateBaseline(CCObject*) {
        if (g_generationRunning.exchange(true)) {
            showNotification(
                "Gameplay generation is already running.",
                NotificationIcon::Info
            );
            return;
        }

        auto finishEarly = [] {
            g_generationRunning = false;
        };

        auto* editorLayer = this->m_editorLayer;
        auto* level = editorLayer ? editorLayer->m_level : nullptr;

        if (!editorLayer || !level) {
            finishEarly();
            FLAlertLayer::create(
                "GD AI Editor",
                "Could not read the current editor level.",
                "OK"
            )->show();
            return;
        }

        auto audioPath = resolveAudioPath(level);
        if (audioPath.empty()) {
            finishEarly();
            FLAlertLayer::create(
                "Song Not Found",
                "Download this level's song in Geometry Dash first.",
                "OK"
            )->show();
            return;
        }

        auto songDirectory =
            Mod::get()->getSaveDir() / "song-cache" / makeSongKey(level);
        auto signature = makeFileSignature(audioPath);
        constexpr auto cacheVersion = "v2";
        auto analysisPath =
            songDirectory
            / ("analysis-" + std::string(cacheVersion) + "-" + signature + ".json");

        if (!std::filesystem::exists(analysisPath)) {
            finishEarly();
            FLAlertLayer::create(
                "Analyze Song First",
                "Press the <cy>music-note</c> GD AI button once before generating a layout.",
                "OK"
            )->show();
            return;
        }

        bool useLlm =
            Mod::get()->getSettingValue<bool>("llm-planner-enabled");

        if (
            useLlm
            && editorLayer->m_objects
            && editorLayer->m_objects->count() > 250
        ) {
            finishEarly();
            FLAlertLayer::create(
                "Use an Empty Test Level",
                "Clear the existing generated gameplay before calling Luna. "
                "This check happens before the API request so no credits are wasted.",
                "OK"
            )->show();
            return;
        }

        if (!useLlm) {
            auto generation = generateBaselineLayout(editorLayer, analysisPath);
            finishEarly();

            if (!generation.success) {
                FLAlertLayer::create(
                    "Generator Preview",
                    generation.error,
                    "OK"
                )->show();
                return;
            }

            showNotification(
                fmt::format(
                    "Generated {} objects across {} beats.",
                    generation.createdObjects,
                    generation.usedBeats
                ),
                NotificationIcon::Success,
                4.0f
            );

            auto learningLine = generation.learnedProfileLoaded
                ? fmt::format(
                    "Training profile: <cg>{}</c> levels / <cy>{}</c> cube phrases"
                    " / <co>{}</c> style motifs",
                    generation.learnedSourceLevels,
                    generation.learnedSourcePhrases,
                    generation.learnedMotifCount
                )
                : std::string("Training profile: <cr>fallback only</c>");

            FLAlertLayer::create(
                "Gameplay Planner v3.2",
                fmt::format(
                    "Target: <cr>{}</c> ({:.0f}%).\n"
                    "Created <cg>{}</c> objects across <cy>{}</c> main beats.\n"
                    "Built <co>{}</c> structural objects and <co>{}</c> gameplay interactions "
                    "across <cy>{}</c> chunks.\n"
                    "Planned <cg>{}</c> mode sections with <cy>{}</c> form transitions.\n\n"
                    "{}\n"
                    "Mode cadence and transition order are learned from the target difficulty.\n"
                    "Modes: cube, ship, ball, UFO, wave.",
                    generation.targetDifficulty,
                    generation.targetDifficultyScore * 100.0,
                    generation.createdObjects,
                    generation.usedBeats,
                    generation.structuredBlocks,
                    generation.gameplayEvents,
                    generation.phraseCount,
                    generation.modeSections,
                    generation.modeTransitions,
                    learningLine
                ),
                "OK"
            )->show();
            return;
        }

        auto apiKey = std::getenv("OPENAI_API_KEY");
        if (!apiKey || std::string(apiKey).empty()) {
            finishEarly();
            FLAlertLayer::create(
                "Luna Planner Needs API Key",
                "Set <cy>OPENAI_API_KEY</c> in Windows, restart Geometry Dash, "
                "then press Generate again.<br><br>The key is never stored in the level or repository.",
                "OK"
            )->show();
            return;
        }

        auto backendPath =
            Mod::get()->getResourcesDir() / "gd-ai-backend.exe";
        if (!std::filesystem::exists(backendPath)) {
            finishEarly();
            FLAlertLayer::create(
                "Backend Missing",
                "The bundled GD AI backend is missing from this build.",
                "OK"
            )->show();
            return;
        }

        auto trainingDirectory =
            Mod::get()->getSaveDir() / "gameplay-exports";
        if (!std::filesystem::exists(trainingDirectory)) {
            finishEarly();
            FLAlertLayer::create(
                "No Training Exports",
                "The LLM planner needs your aligned gameplay exports first.",
                "OK"
            )->show();
            return;
        }

        auto cacheDirectory = Mod::get()->getSaveDir() / "llm-cache";
        auto planDirectory = Mod::get()->getSaveDir() / "llm-plans";
        std::error_code directoryError;
        std::filesystem::create_directories(cacheDirectory, directoryError);
        directoryError.clear();
        std::filesystem::create_directories(planDirectory, directoryError);

        if (directoryError) {
            finishEarly();
            FLAlertLayer::create(
                "GD AI Editor",
                "Could not create the LLM planner cache directory.",
                "OK"
            )->show();
            return;
        }

        auto cachePath = cacheDirectory / "human-chunks-v1.json";
        auto stem = exportStem(level);
        auto planPath = planDirectory / (stem + "-plan.json");
        auto logPath = planDirectory / (stem + "-plan.log");
        auto difficulty =
            Mod::get()->getSettingValue<std::string>("target-difficulty");
        auto model =
            Mod::get()->getSettingValue<std::string>("planner-model");
        auto reasoningEffort =
            Mod::get()->getSettingValue<std::string>("planner-reasoning-effort");
        double songOffset = editorLayer->m_levelSettings->m_songOffset;

        showNotification(
            "GPT-6 Luna is planning gameplay from your training levels...",
            NotificationIcon::Loading,
            4.0f
        );

        editorLayer->retain();

        std::thread([
            editorLayer,
            backendPath,
            analysisPath,
            trainingDirectory,
            cachePath,
            planPath,
            logPath,
            difficulty,
            model,
            reasoningEffort,
            songOffset
        ] {
            {
                std::ofstream logFile(logPath, std::ios::out | std::ios::trunc);
                if (logFile) {
                    logFile
                        << "GD AI Editor LLM gameplay planner\n"
                        << "Model: " << model << "\n"
                        << "Reasoning: " << reasoningEffort << "\n"
                        << "Difficulty: " << difficulty << "\n"
                        << "Training directory: " << trainingDirectory.string() << "\n"
                        << "Reference cache: " << cachePath.string() << "\n"
                        << "Plan output: " << planPath.string() << "\n\n";
                }
            }

#ifdef GEODE_IS_WINDOWS
            int exitCode = runLlmPlanProcess(
                backendPath,
                analysisPath,
                trainingDirectory,
                cachePath,
                planPath,
                logPath,
                difficulty,
                model,
                reasoningEffort,
                songOffset
            );
#else
            int exitCode = -1;
#endif

            bool plannerSuccess =
                exitCode == 0 && std::filesystem::exists(planPath);

            Loader::get()->queueInMainThread([
                editorLayer,
                plannerSuccess,
                exitCode,
                analysisPath,
                planPath,
                logPath,
                model
            ] {
                if (!plannerSuccess) {
                    g_generationRunning = false;
                    editorLayer->release();

                    FLAlertLayer::create(
                        "Luna Planning Failed",
                        fmt::format(
                            "The planner exited with code <cr>{}</c>.<br><br>"
                            "No procedural gameplay was substituted, so you always know "
                            "whether the LLM actually ran.<br><br>Log:<br><cy>{}</c>",
                            exitCode,
                            logPath.string()
                        ),
                        "OK"
                    )->show();
                    return;
                }

                auto generation =
                    generateLlmLayout(editorLayer, analysisPath, planPath);

                g_generationRunning = false;
                editorLayer->release();

                if (!generation.success) {
                    FLAlertLayer::create(
                        "LLM Compile Failed",
                        fmt::format(
                            "{}<br><br>Plan:<br><cy>{}</c>",
                            generation.error,
                            planPath.string()
                        ),
                        "OK"
                    )->show();
                    return;
                }

                showNotification(
                    fmt::format(
                        "Luna generated {} objects across {} sections.",
                        generation.createdObjects,
                        generation.sections
                    ),
                    NotificationIcon::Success,
                    5.0f
                );

                FLAlertLayer::create(
                    "Luna Gameplay Planner v1",
                    fmt::format(
                        "Model: <cg>{}</c>\n"
                        "Compiled <cy>{}</c> objects and <co>{}</c> gameplay interactions.\n"
                        "Sections: <cg>{}</c> / mode transitions: <cy>{}</c>.\n\n"
                        "This build uses the LLM for action/rhythm composition and "
                        "the local compiler for exact GD object placement.",
                        model,
                        generation.createdObjects,
                        generation.gameplayEvents,
                        generation.sections,
                        generation.modeTransitions
                    ),
                    "OK"
                )->show();
            });
        }).detach();
    }

    void onExportGameplay(CCObject*) {
        if (g_exportRunning.exchange(true)) {
            showNotification("Gameplay export is already running.", NotificationIcon::Info);
            return;
        }

        auto finishEarly = [] {
            g_exportRunning = false;
        };

        auto* editorLayer = this->m_editorLayer;
        auto* level = editorLayer ? editorLayer->m_level : nullptr;

        if (!editorLayer || !level) {
            finishEarly();
            FLAlertLayer::create(
                "GD AI Editor",
                "Could not read the current editor level.",
                "OK"
            )->show();
            return;
        }

        auto audioPath = resolveAudioPath(level);
        if (audioPath.empty()) {
            finishEarly();
            FLAlertLayer::create(
                "Song Not Found",
                "Download this level's song in Geometry Dash first.",
                "OK"
            )->show();
            return;
        }

        auto backendPath = Mod::get()->getResourcesDir() / "gd-ai-backend.exe";
        if (!std::filesystem::exists(backendPath)) {
            finishEarly();
            FLAlertLayer::create(
                "Backend Missing",
                "The bundled GD AI backend is missing from this build.",
                "OK"
            )->show();
            return;
        }

        auto songDirectory = Mod::get()->getSaveDir() / "song-cache" / makeSongKey(level);
        auto signature = makeFileSignature(audioPath);
        constexpr auto cacheVersion = "v2";
        auto analysisPath =
            songDirectory / ("analysis-" + std::string(cacheVersion) + "-" + signature + ".json");

        if (!std::filesystem::exists(analysisPath)) {
            finishEarly();
            FLAlertLayer::create(
                "Analyze Song First",
                "Press the <cy>music-note</c> GD AI button once before exporting gameplay.",
                "OK"
            )->show();
            return;
        }

        auto exportDirectory = Mod::get()->getSaveDir() / "gameplay-exports";
        auto stem = exportStem(level);
        auto rawGameplayPath = exportDirectory / (stem + "-raw.json");
        auto alignedGameplayPath = exportDirectory / (stem + "-aligned.json");
        auto logPath = exportDirectory / (stem + "-align.log");

        auto exportResult = exportGameplayTimeline(editorLayer, rawGameplayPath);
        if (!exportResult.success) {
            finishEarly();
            FLAlertLayer::create(
                "Gameplay Export Failed",
                exportResult.error,
                "OK"
            )->show();
            return;
        }

        showNotification(
            fmt::format("Exported {} gameplay objects. Aligning...", exportResult.exportedObjects),
            NotificationIcon::Loading,
            3.0f
        );

        log::info(
            "Exported {}/{} gameplay objects to {}",
            exportResult.exportedObjects,
            exportResult.totalObjects,
            rawGameplayPath.string()
        );

        std::thread([
            backendPath,
            rawGameplayPath,
            analysisPath,
            alignedGameplayPath,
            logPath,
            exportedObjects = exportResult.exportedObjects
        ] {
            {
                std::ofstream logFile(logPath, std::ios::out | std::ios::trunc);
                if (logFile) {
                    logFile
                        << "GD AI Editor gameplay alignment\n"
                        << "Raw gameplay: " << rawGameplayPath.string() << "\n"
                        << "Analysis: " << analysisPath.string() << "\n"
                        << "Aligned gameplay: " << alignedGameplayPath.string() << "\n\n";
                }
            }

#ifdef GEODE_IS_WINDOWS
            int exitCode = runGameplayAlignProcess(
                backendPath,
                rawGameplayPath,
                analysisPath,
                alignedGameplayPath,
                logPath
            );
#else
            int exitCode = -1;
#endif

            bool success = exitCode == 0 && std::filesystem::exists(alignedGameplayPath);
            g_exportRunning = false;

            Loader::get()->queueInMainThread([
                success,
                exitCode,
                rawGameplayPath,
                alignedGameplayPath,
                logPath,
                exportedObjects
            ] {
                if (success) {
                    showNotification(
                        fmt::format("Aligned {} gameplay objects to the song.", exportedObjects),
                        NotificationIcon::Success,
                        4.0f
                    );

                    log::info("Aligned gameplay export: {}", alignedGameplayPath.string());

                    FLAlertLayer::create(
                        "Gameplay Export Ready",
                        fmt::format(
                            "Exported <cg>{}</c> gameplay objects and aligned them to detected beats.<br><br>Output folder:<br><cy>{}</c>",
                            exportedObjects,
                            alignedGameplayPath.parent_path().string()
                        ),
                        "OK"
                    )->show();
                } else {
                    showNotification(
                        "Gameplay alignment failed.",
                        NotificationIcon::Error,
                        5.0f
                    );

                    log::error(
                        "Gameplay alignment failed with exit code {}. Log: {}",
                        exitCode,
                        logPath.string()
                    );

                    FLAlertLayer::create(
                        "Gameplay Alignment Failed",
                        fmt::format(
                            "Backend exit code: <cr>{}</c><br><br>Log:<br><cy>{}</c>",
                            exitCode,
                            logPath.string()
                        ),
                        "OK"
                    )->show();
                }
            });
        }).detach();
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

        constexpr auto cacheVersion = "v2";
        auto analysisPath = songDirectory / ("analysis-" + std::string(cacheVersion) + "-" + signature + ".json");
        auto previewPath = songDirectory / ("beats-" + std::string(cacheVersion) + "-" + signature + ".wav");
        auto logPath = songDirectory / ("analysis-" + std::string(cacheVersion) + "-" + signature + ".log");

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
