#pragma once

#include <Geode/Geode.hpp>

#include <cstddef>
#include <filesystem>
#include <string>

struct BaselineGenerationResult {
    bool success = false;
    std::string error;
    std::size_t createdObjects = 0;
    std::size_t usedBeats = 0;
    std::size_t usedMicroOnsets = 0;
    std::size_t gameplayEvents = 0;
    std::size_t structuredBlocks = 0;
    std::size_t phraseCount = 0;
    std::size_t learnedSourceLevels = 0;
    std::size_t learnedSourcePhrases = 0;
    bool learnedProfileLoaded = false;
    double firstBeatTime = 0.0;
    double lastBeatTime = 0.0;
};

BaselineGenerationResult generateBaselineLayout(
    LevelEditorLayer* editorLayer,
    std::filesystem::path const& analysisPath
);
