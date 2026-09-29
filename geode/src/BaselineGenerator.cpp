#include "BaselineGenerator.hpp"

#include <Geode/binding/DrawGridLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <vector>

using namespace geode::prelude;

namespace {
constexpr float kGroundY = 105.0f;
constexpr std::size_t kMaximumPreviewBeats = 96;

struct BeatSample {
    double time = 0.0;
    double onset = 0.0;
    double energy = 0.0;
};

struct OnsetSample {
    double time = 0.0;
    double strength = 0.0;
};

struct AnalysisData {
    std::vector<BeatSample> beats;
    std::vector<OnsetSample> onsets;
};

bool addGeneratedObject(
    LevelEditorLayer* editorLayer,
    int objectID,
    CCPoint const& position
) {
    if (!editorLayer) {
        return false;
    }

    return editorLayer->createObject(objectID, position, true) != nullptr;
}

AnalysisData readAnalysis(std::filesystem::path const& analysisPath, std::string& error) {
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

    AnalysisData analysis;

    for (auto const& value : beatsResult.unwrap()) {
        auto timeResult = value["time"].asDouble();
        if (!timeResult.isOk()) {
            continue;
        }

        BeatSample beat;
        beat.time = timeResult.unwrap();
        beat.onset = value["onset_strength"].asDouble().unwrapOr(0.0);
        beat.energy = value["energy"].asDouble().unwrapOr(0.0);
        analysis.beats.push_back(beat);
    }

    auto onsetsResult = root["onsets"].asArray();
    if (onsetsResult.isOk()) {
        for (auto const& value : onsetsResult.unwrap()) {
            auto timeResult = value["time"].asDouble();
            if (!timeResult.isOk()) {
                continue;
            }

            OnsetSample onset;
            onset.time = timeResult.unwrap();
            onset.strength = value["strength"].asDouble().unwrapOr(0.0);
            analysis.onsets.push_back(onset);
        }
    }

    std::sort(
        analysis.beats.begin(),
        analysis.beats.end(),
        [](BeatSample const& left, BeatSample const& right) {
            return left.time < right.time;
        }
    );

    std::sort(
        analysis.onsets.begin(),
        analysis.onsets.end(),
        [](OnsetSample const& left, OnsetSample const& right) {
            return left.time < right.time;
        }
    );

    return analysis;
}

double beatScore(BeatSample const& beat) {
    return beat.onset * 0.72 + beat.energy * 0.28;
}

double nearestBeatDistance(std::vector<BeatSample> const& beats, double time) {
    if (beats.empty()) {
        return std::numeric_limits<double>::infinity();
    }

    auto right = std::lower_bound(
        beats.begin(),
        beats.end(),
        time,
        [](BeatSample const& beat, double value) {
            return beat.time < value;
        }
    );

    double distance = std::numeric_limits<double>::infinity();

    if (right != beats.end()) {
        distance = std::min(distance, std::abs(right->time - time));
    }

    if (right != beats.begin()) {
        auto left = std::prev(right);
        distance = std::min(distance, std::abs(left->time - time));
    }

    return distance;
}

double onsetThreshold(std::vector<OnsetSample> const& onsets) {
    if (onsets.empty()) {
        return 1.0;
    }

    std::vector<double> strengths;
    strengths.reserve(onsets.size());

    for (auto const& onset : onsets) {
        strengths.push_back(onset.strength);
    }

    std::size_t percentileIndex = strengths.size() * 65 / 100;
    percentileIndex = std::min(percentileIndex, strengths.size() - 1);
    std::nth_element(
        strengths.begin(),
        strengths.begin() + static_cast<std::ptrdiff_t>(percentileIndex),
        strengths.end()
    );

    return std::max(0.12, strengths[percentileIndex]);
}
}

BaselineGenerationResult generateBaselineLayout(
    LevelEditorLayer* editorLayer,
    std::filesystem::path const& analysisPath
) {
    BaselineGenerationResult result;

    if (!editorLayer || !editorLayer->m_levelSettings || !editorLayer->m_drawGridLayer) {
        result.error = "The editor is missing timing/settings data.";
        return result;
    }

    if (editorLayer->m_levelSettings->m_platformerMode) {
        result.error = "The generator preview currently supports classic mode only.";
        return result;
    }

    if (editorLayer->m_levelSettings->m_startMode != 0) {
        result.error = "The generator preview currently supports cube start mode only.";
        return result;
    }

    if (editorLayer->m_objects && editorLayer->m_objects->count() > 200) {
        result.error =
            "Use a mostly empty level for the generator preview. This safety check prevents "
            "accidentally writing hundreds of objects into a finished level.";
        return result;
    }

    std::string readError;
    auto analysis = readAnalysis(analysisPath, readError);

    if (!readError.empty()) {
        result.error = readError;
        return result;
    }

    if (analysis.beats.size() < 12) {
        result.error = "Not enough detected beats to generate a preview.";
        return result;
    }

    double songOffset = editorLayer->m_levelSettings->m_songOffset;
    double minimumAudioTime = songOffset + 1.25;

    auto first = std::lower_bound(
        analysis.beats.begin(),
        analysis.beats.end(),
        minimumAudioTime,
        [](BeatSample const& beat, double value) {
            return beat.time < value;
        }
    );

    if (first == analysis.beats.end()) {
        result.error = "No detected beats occur after the current song offset.";
        return result;
    }

    std::vector<BeatSample> beats;
    for (
        auto iterator = first;
        iterator != analysis.beats.end() && beats.size() < kMaximumPreviewBeats;
        ++iterator
    ) {
        beats.push_back(*iterator);
    }

    if (beats.size() < 12) {
        result.error = "Not enough usable beats remain after the song offset.";
        return result;
    }

    auto positionForAudioTime = [&](double audioTime) {
        float levelTime = static_cast<float>(std::max(0.0, audioTime - songOffset));
        return editorLayer->m_drawGridLayer->posForTime(levelTime);
    };

    auto positionForBeat = [&](BeatSample const& beat) {
        return positionForAudioTime(beat.time);
    };

    float firstX = positionForBeat(beats.front()).x;
    float lastX = positionForBeat(beats.back()).x;

    if (!std::isfinite(firstX) || !std::isfinite(lastX) || lastX <= firstX) {
        result.error = "Geometry Dash returned an invalid beat-to-position mapping.";
        return result;
    }

    std::size_t createdObjects = 0;
    std::size_t gameplayEvents = 0;

    // The editor's visible gameplay baseline is 90 units above the raw y=15
    // row used by some old level-string examples. The previous preview exposed
    // this clearly by placing everything three grid blocks too low.
    constexpr std::array<float, 4> beatMarkerY = {
        kGroundY + 60.0f,
        kGroundY + 75.0f,
        kGroundY + 90.0f,
        kGroundY + 75.0f,
    };

    // Yellow orb = main detected beat. This intentionally marks EVERY beat so
    // timing drift is obvious instead of hidden behind a sparse obstacle pattern.
    for (std::size_t index = 0; index < beats.size(); ++index) {
        float beatX = positionForBeat(beats[index]).x;
        float markerY = beatMarkerY[index % beatMarkerY.size()];

        createdObjects += addGeneratedObject(editorLayer, 36, {beatX, markerY}) ? 1 : 0;
    }

    // Pink orb = strong onset that is NOT already represented by a main beat.
    // This surfaces smaller kicks, snares, taps, and "micro-bumps" detected by
    // librosa without pretending they are full beats.
    std::vector<OnsetSample> usableOnsets;
    for (auto const& onset : analysis.onsets) {
        if (onset.time < beats.front().time || onset.time > beats.back().time) {
            continue;
        }
        usableOnsets.push_back(onset);
    }

    double threshold = onsetThreshold(usableOnsets);
    double previousMicroTime = -1000.0;
    std::size_t microIndex = 0;

    for (auto const& onset : usableOnsets) {
        if (onset.strength < threshold) {
            continue;
        }

        // Do not draw a second marker when an onset is effectively the same hit
        // as a tracked beat.
        if (nearestBeatDistance(beats, onset.time) < 0.095) {
            continue;
        }

        // Prevent dense transient clusters from becoming an unreadable orb wall.
        if (onset.time - previousMicroTime < 0.085) {
            continue;
        }

        float onsetX = positionForAudioTime(onset.time).x;
        float onsetY = kGroundY + 120.0f + static_cast<float>((microIndex % 3) * 12);

        if (addGeneratedObject(editorLayer, 141, {onsetX, onsetY})) {
            ++createdObjects;
            ++result.usedMicroOnsets;
            ++microIndex;
            previousMicroTime = onset.time;
        }

        if (result.usedMicroOnsets >= 96) {
            break;
        }
    }

    // Gameplay density follows local energy. Quiet phrases get a light event;
    // energetic phrases get multiple hazards or pads. This is still deliberately
    // deterministic, but it is now dense enough to visibly react to song changes.
    std::size_t phraseIndex = 0;

    for (std::size_t start = 4; start + 3 < beats.size(); start += 4, ++phraseIndex) {
        std::array<std::size_t, 4> ranked = {
            start,
            start + 1,
            start + 2,
            start + 3,
        };

        std::sort(
            ranked.begin(),
            ranked.end(),
            [&](std::size_t left, std::size_t right) {
                return beatScore(beats[left]) > beatScore(beats[right]);
            }
        );

        double phraseEnergy = 0.0;
        for (std::size_t index = start; index < start + 4; ++index) {
            phraseEnergy += beats[index].energy;
        }
        phraseEnergy /= 4.0;

        int eventCount = 1;
        if (phraseEnergy >= 0.70) {
            eventCount = 2;
        } else if (phraseEnergy < 0.24 && phraseIndex % 2 == 1) {
            eventCount = 0;
        }

        for (int event = 0; event < eventCount; ++event) {
            std::size_t beatIndex = ranked[static_cast<std::size_t>(event)];
            float eventX = positionForBeat(beats[beatIndex]).x;
            int pattern = static_cast<int>((phraseIndex + event) % 5);

            if (pattern == 0 || pattern == 3) {
                if (addGeneratedObject(editorLayer, 8, {eventX, kGroundY})) {
                    ++createdObjects;
                    ++gameplayEvents;
                }
            } else if (pattern == 1) {
                // Two-spike accent on a strong phrase, but only when the next
                // tracked beat leaves enough horizontal room.
                if (addGeneratedObject(editorLayer, 8, {eventX, kGroundY})) {
                    ++createdObjects;
                    ++gameplayEvents;
                }

                if (beatIndex + 1 < beats.size()) {
                    float nextBeatX = positionForBeat(beats[beatIndex + 1]).x;
                    if (nextBeatX - eventX >= 110.0f) {
                        if (addGeneratedObject(editorLayer, 8, {eventX + 30.0f, kGroundY})) {
                            ++createdObjects;
                            ++gameplayEvents;
                        }
                    }
                }
            } else if (pattern == 2) {
                if (addGeneratedObject(editorLayer, 35, {eventX, kGroundY})) {
                    ++createdObjects;
                    ++gameplayEvents;
                }
            } else {
                // Phrase accent orb. It sits above the main beat contour and is
                // optional gameplay rather than a required survival input.
                if (addGeneratedObject(editorLayer, 36, {eventX, kGroundY + 135.0f})) {
                    ++createdObjects;
                    ++gameplayEvents;
                }
            }
        }
    }

    if (createdObjects == 0) {
        result.error = "The generator did not create any objects.";
        return result;
    }

    result.success = true;
    result.createdObjects = createdObjects;
    result.gameplayEvents = gameplayEvents;
    result.usedBeats = beats.size();
    result.firstBeatTime = beats.front().time;
    result.lastBeatTime = beats.back().time;
    return result;
}
