#pragma once

#include <Geode/Geode.hpp>

#include <filesystem>
#include <string>

struct GameplayExportResult {
    bool success = false;
    std::string error;
    std::filesystem::path path;
    std::size_t totalObjects = 0;
    std::size_t exportedObjects = 0;
};

GameplayExportResult exportGameplayTimeline(
    LevelEditorLayer* editorLayer,
    std::filesystem::path const& outputPath
);
