#include "BaselineGenerator.hpp"

#include <Geode/binding/DrawGridLayer.hpp>
#include <Geode/binding/EditorUI.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

using namespace geode::prelude;

namespace {
struct BeatSample {
    double time = 0.0;
    double onset = 0.0;
    double energy = 0.0;
};

float snapGridX(float x) {
    return std::round((x - 15.0f) / 30.0f) * 30.0f + 15.0f;
}

bool addGeneratedObject(
    EditorUI* editorUI,
    int objectID,
    CCPoint const& position
) {
    if (!editorUI) {
        return false;
    }

    editorUI->createObject(objectID, position);
    return true;
}

std::vector<BeatSample> readBeats(std::filesystem::path const& analysisPath, std::string& error) {
    std::ifstream input(analysisPath);
    if (!input) {
        error = "Could not open the cached song analysis.";
        return {};
    }

    auto parsed = matjson::parse(input);
    if (!parsed.isOk()) {
        error = "Could not parse the cached song analysis JSON.";
        return {};
    }

    auto root = parsed.unwrap();
    auto beatsResult = root["beats"].asArray();
    if (!beatsResult.isOk()) {
        error = "Song analysis does not contain a beats array.";
        return {};
    }

    std::vector<BeatSample> beats;

    for (auto const& value : beatsResult.unwrap()) {
        auto timeResult = value["time"].asDouble();
        if (!timeResult.isOk()) {
            continue;
        }

        BeatSample beat;
        beat.time = timeResult.unwrap();
        beat.onset = value["onset_strength"].asDouble().unwrapOr(0.0);
        beat.energy = value["energy"].asDouble().unwrapOr(0.0);
        beats.push_back(beat);
    }

    std::sort(
        beats.begin(),
        beats.end(),
        [](BeatSample const& left, BeatSample const& right) {
            return left.time < right.time;
        }
    );

    return beats;
}
}

BaselineGenerationResult generateBaselineLayout(
    EditorUI* editorUI,
    LevelEditorLayer* editorLayer,
    std::filesystem::path const& analysisPath
) {
    BaselineGenerationResult result;

    if (!editorUI || !editorLayer || !editorLayer->m_levelSettings || !editorLayer->m_drawGridLayer) {
        result.error = "The editor is missing timing/settings data.";
        return result;
    }

    if (editorLayer->m_levelSettings->m_platformerMode) {
        result.error = "The first generator preview currently supports classic mode only.";
        return result;
    }

    if (editorLayer->m_levelSettings->m_startMode != 0) {
        result.error = "The first generator preview currently supports cube start mode only.";
        return result;
    }

    if (editorLayer->m_objects && editorLayer->m_objects->count() > 200) {
        result.error =
            "Use a mostly empty level for the generator preview. This safety check prevents "
            "accidentally writing hundreds of objects into a finished level.";
        return result;
    }

    std::string readError;
    auto allBeats = readBeats(analysisPath, readError);

    if (!readError.empty()) {
        result.error = readError;
        return result;
    }

    if (allBeats.size() < 12) {
        result.error = "Not enough detected beats to generate a preview.";
        return result;
    }

    double songOffset = editorLayer->m_levelSettings->m_songOffset;
    double minimumAudioTime = songOffset + 1.25;

    auto first = std::lower_bound(
        allBeats.begin(),
        allBeats.end(),
        minimumAudioTime,
        [](BeatSample const& beat, double value) {
            return beat.time < value;
        }
    );

    if (first == allBeats.end()) {
        result.error = "No detected beats occur after the current song offset.";
        return result;
    }

    std::vector<BeatSample> beats;
    for (auto iterator = first; iterator != allBeats.end() && beats.size() < 32; ++iterator) {
        beats.push_back(*iterator);
    }

    if (beats.size() < 12) {
        result.error = "Not enough usable beats remain after the song offset.";
        return result;
    }

    auto positionForBeat = [&](BeatSample const& beat) {
        float levelTime = static_cast<float>(std::max(0.0, beat.time - songOffset));
        return editorLayer->m_drawGridLayer->posForTime(levelTime);
    };

    float firstX = positionForBeat(beats.front()).x;
    float lastX = positionForBeat(beats.back()).x;

    if (!std::isfinite(firstX) || !std::isfinite(lastX) || lastX <= firstX) {
        result.error = "Geometry Dash returned an invalid beat-to-position mapping.";
        return result;
    }

    std::size_t createdObjects = 0;

    // The built-in Geometry Dash ground stays untouched. The first visible
    // baseline only places gameplay events on top of it, so the generated
    // section starts playable instead of forcing the cube into a block wall.

    // One rhythm event per four-beat phrase. The strongest detected beat in
    // each phrase receives the obstacle so the preview visibly follows the song
    // without turning into an unreadable wall of spikes.
    std::size_t phraseIndex = 0;
    for (std::size_t start = 4; start + 3 < beats.size(); start += 4, ++phraseIndex) {
        std::size_t strongest = start;

        for (std::size_t index = start + 1; index < start + 4; ++index) {
            double currentScore = beats[index].onset * 0.70 + beats[index].energy * 0.30;
            double strongestScore =
                beats[strongest].onset * 0.70 + beats[strongest].energy * 0.30;

            if (currentScore > strongestScore) {
                strongest = index;
            }
        }

        float obstacleX = snapGridX(positionForBeat(beats[strongest]).x);

        if (phraseIndex % 4 == 3) {
            // Give some phrases a non-lethal automatic jump instead of another
            // spike so the baseline is visibly more than a metronome of hazards.
            createdObjects += addGeneratedObject(editorUI, 35, {obstacleX, 15.0f}) ? 1 : 0;
        } else {
            // Standard ground spike. Every third phrase gets a second adjacent
            // spike when there is enough room before the next detected beat.
            createdObjects += addGeneratedObject(editorUI, 8, {obstacleX, 15.0f}) ? 1 : 0;

            if (phraseIndex % 3 == 2 && strongest + 1 < beats.size()) {
                float nextBeatX = positionForBeat(beats[strongest + 1]).x;
                if (nextBeatX - obstacleX >= 105.0f) {
                    createdObjects += addGeneratedObject(editorUI, 8, {obstacleX + 30.0f, 15.0f}) ? 1 : 0;
                }
            }
        }

        // Add an optional yellow orb to alternating phrases. It is intentionally
        // not required for survival yet; this is a visible music-sync baseline,
        // not the learned gameplay generator.
        if (phraseIndex % 2 == 1) {
            createdObjects += addGeneratedObject(editorUI, 36, {obstacleX + 60.0f, 90.0f}) ? 1 : 0;
        }
    }

    if (createdObjects == 0) {
        result.error = "The generator did not create any objects.";
        return result;
    }

    result.success = true;
    result.createdObjects = createdObjects;
    result.usedBeats = beats.size();
    result.firstBeatTime = beats.front().time;
    result.lastBeatTime = beats.back().time;
    return result;
}
