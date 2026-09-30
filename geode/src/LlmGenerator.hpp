#pragma once

#include <Geode/Geode.hpp>

#include <cstddef>
#include <filesystem>
#include <string>

struct LlmGenerationResult {
    bool success = false;
    std::string error;
    std::size_t createdObjects = 0;
    std::size_t gameplayEvents = 0;
    std::size_t sections = 0;
    std::size_t modeTransitions = 0;
    std::size_t humanReferenceSections = 0;
};

LlmGenerationResult generateLlmLayout(
    LevelEditorLayer* editorLayer,
    std::filesystem::path const& analysisPath,
    std::filesystem::path const& planPath
);
