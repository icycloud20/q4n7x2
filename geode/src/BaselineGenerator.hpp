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
    double firstBeatTime = 0.0;
    double lastBeatTime = 0.0;
};

BaselineGenerationResult generateBaselineLayout(
    EditorUI* editorUI,
    LevelEditorLayer* editorLayer,
    std::filesystem::path const& analysisPath
);
