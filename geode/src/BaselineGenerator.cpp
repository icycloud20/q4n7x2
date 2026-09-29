#include "BaselineGenerator.hpp"

#include <Geode/binding/DrawGridLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {
constexpr float kGroundY = 105.0f;
constexpr float kGroundSurfaceY = 90.0f;
constexpr float kGroundPadY = 92.0f;
constexpr std::size_t kMaximumPreviewBeats = 128;

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

enum class PhraseTemplate {
    HazardOrbPad,
    HazardPad,
    HazardOrb,
    HazardOnly,
    PadOnly,
    Quiet,
};

struct LearnedProfile {
    bool loaded = false;
    std::size_t sourceLevels = 0;
    std::size_t sourcePhrases = 0;
    int recommendedMaxEvents = 3;
    double denseSubdivisionWeight = 0.0;

    double hazardOrbPad = 0.48;
    double hazardPad = 0.16;
    double hazardOrb = 0.14;
    double hazardOnly = 0.16;
    double padOnly = 0.03;
    double quiet = 0.03;
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

bool addCountedObject(
    LevelEditorLayer* editorLayer,
    int objectID,
    CCPoint const& position,
    std::size_t& createdObjects,
    std::size_t& gameplayEvents
) {
    if (!addGeneratedObject(editorLayer, objectID, position)) {
        return false;
    }

    ++createdObjects;
    ++gameplayEvents;
    return true;
}

std::size_t addBlockColumn(
    LevelEditorLayer* editorLayer,
    float x,
    int height,
    std::size_t& createdObjects
) {
    std::size_t added = 0;

    for (int index = 0; index < height; ++index) {
        float y = kGroundY + static_cast<float>(index * 30);
        if (addGeneratedObject(editorLayer, 1, {x, y})) {
            ++added;
            ++createdObjects;
        }
    }

    return added;
}

std::size_t addLowPlatform(
    LevelEditorLayer* editorLayer,
    float centerX,
    int blockCount,
    std::size_t& createdObjects
) {
    blockCount = std::clamp(blockCount, 1, 6);
    float startX = centerX - static_cast<float>(blockCount - 1) * 15.0f;
    std::size_t added = 0;

    for (int index = 0; index < blockCount; ++index) {
        float x = startX + static_cast<float>(index * 30);
        if (addGeneratedObject(editorLayer, 1, {x, kGroundY})) {
            ++added;
            ++createdObjects;
        }
    }

    return added;
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

LearnedProfile readLearnedProfile() {
    LearnedProfile profile;
    auto profilePath = Mod::get()->getResourcesDir() / "learned-profile-v1.json";

    std::ifstream input(profilePath);
    if (!input) {
        log::warn("Learned gameplay profile is missing: {}", profilePath.string());
        return profile;
    }

    auto parsed = matjson::parse(input);
    if (!parsed.isOk()) {
        log::warn("Could not parse learned gameplay profile: {}", profilePath.string());
        return profile;
    }

    auto root = parsed.unwrap();
    auto weights = root["template_weights"];

    profile.sourceLevels = static_cast<std::size_t>(
        std::max(0.0, root["source_level_count"].asDouble().unwrapOr(0.0))
    );
    profile.sourcePhrases = static_cast<std::size_t>(
        std::max(0.0, root["cube_phrase_count"].asDouble().unwrapOr(0.0))
    );
    profile.recommendedMaxEvents = std::clamp(
        static_cast<int>(
            root["recommended_max_events_per_phrase"].asDouble().unwrapOr(3.0)
        ),
        2,
        6
    );

    profile.hazardOrbPad =
        weights["hazard_orb_pad"].asDouble().unwrapOr(profile.hazardOrbPad);
    profile.hazardPad =
        weights["hazard_pad"].asDouble().unwrapOr(profile.hazardPad);
    profile.hazardOrb =
        weights["hazard_orb"].asDouble().unwrapOr(profile.hazardOrb);
    profile.hazardOnly =
        weights["hazard_only"].asDouble().unwrapOr(profile.hazardOnly);
    profile.padOnly =
        weights["pad_only"].asDouble().unwrapOr(profile.padOnly);
    profile.quiet =
        weights["quiet"].asDouble().unwrapOr(profile.quiet);

    auto gapWeights = root["rhythm_gap_sixteenth_weights"];
    profile.denseSubdivisionWeight =
        gapWeights["1"].asDouble().unwrapOr(0.0)
        + gapWeights["2"].asDouble().unwrapOr(0.0);

    profile.loaded = profile.sourceLevels > 0 && profile.sourcePhrases > 0;

    if (profile.loaded) {
        log::info(
            "Loaded learned phrase profile from {} levels / {} cube phrases",
            profile.sourceLevels,
            profile.sourcePhrases
        );
    }

    return profile;
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

    std::size_t percentileIndex = strengths.size() * 62 / 100;
    percentileIndex = std::min(percentileIndex, strengths.size() - 1);
    std::nth_element(
        strengths.begin(),
        strengths.begin() + static_cast<std::ptrdiff_t>(percentileIndex),
        strengths.end()
    );

    return std::max(0.10, strengths[percentileIndex]);
}

PhraseTemplate choosePhraseTemplate(
    LearnedProfile const& profile,
    std::size_t phraseIndex,
    double phraseEnergy,
    double phraseOnset
) {
    if (phraseEnergy < 0.13 && phraseOnset < 0.18) {
        return PhraseTemplate::Quiet;
    }

    // Deterministic pseudo-random selector. It produces variety without making
    // repeated presses on the same song completely unpredictable.
    double selector = std::fmod(
        static_cast<double>(phraseIndex + 1) * 0.61803398875
        + phraseEnergy * 0.731
        + phraseOnset * 0.413,
        1.0
    );

    // Busy phrases lean toward the interaction combination that appeared most
    // frequently in the learned cube phrases.
    if (phraseEnergy > 0.74 && selector < 0.72) {
        return PhraseTemplate::HazardOrbPad;
    }

    double total =
        profile.hazardOrbPad
        + profile.hazardPad
        + profile.hazardOrb
        + profile.hazardOnly
        + profile.padOnly
        + profile.quiet;

    if (total <= 0.0) {
        return PhraseTemplate::HazardOrbPad;
    }

    selector *= total;

    if ((selector -= profile.hazardOrbPad) < 0.0) {
        return PhraseTemplate::HazardOrbPad;
    }
    if ((selector -= profile.hazardPad) < 0.0) {
        return PhraseTemplate::HazardPad;
    }
    if ((selector -= profile.hazardOrb) < 0.0) {
        return PhraseTemplate::HazardOrb;
    }
    if ((selector -= profile.hazardOnly) < 0.0) {
        return PhraseTemplate::HazardOnly;
    }
    if ((selector -= profile.padOnly) < 0.0) {
        return PhraseTemplate::PadOnly;
    }

    return PhraseTemplate::Quiet;
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
        result.error = "The phrase generator currently supports classic mode only.";
        return result;
    }

    if (editorLayer->m_levelSettings->m_startMode != 0) {
        result.error = "The phrase generator currently supports cube start mode only.";
        return result;
    }

    if (editorLayer->m_objects && editorLayer->m_objects->count() > 250) {
        result.error =
            "Use a mostly empty level for the generator preview. This safety check prevents "
            "accidentally writing a generated section into a finished level.";
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

    auto profile = readLearnedProfile();
    result.learnedProfileLoaded = profile.loaded;
    result.learnedSourceLevels = profile.sourceLevels;
    result.learnedSourcePhrases = profile.sourcePhrases;

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
    std::size_t structuredBlocks = 0;

    // Debug beat lane. Keeping it for now makes timing regressions obvious while
    // the actual gameplay below is generated in 4-beat phrases.
    constexpr std::array<float, 4> beatMarkerY = {
        kGroundY + 150.0f,
        kGroundY + 165.0f,
        kGroundY + 180.0f,
        kGroundY + 165.0f,
    };

    for (std::size_t index = 0; index < beats.size(); ++index) {
        float beatX = positionForBeat(beats[index]).x;
        float markerY = beatMarkerY[index % beatMarkerY.size()];

        if (addGeneratedObject(editorLayer, 36, {beatX, markerY})) {
            ++createdObjects;
        }
    }

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

    // Strong off-beat transient lane. Human-level exports showed interaction
    // clusters frequently separated by 1/16 and 1/8 beat, so the learned profile
    // lets the preview retain dense micro-onsets instead of forcing everything
    // onto quarter beats.
    double minimumMicroSpacing =
        profile.denseSubdivisionWeight >= 0.70 ? 0.060 : 0.085;

    for (auto const& onset : usableOnsets) {
        if (onset.strength < threshold) {
            continue;
        }

        if (nearestBeatDistance(beats, onset.time) < 0.090) {
            continue;
        }

        if (onset.time - previousMicroTime < minimumMicroSpacing) {
            continue;
        }

        float onsetX = positionForAudioTime(onset.time).x;
        float onsetY = kGroundY + 225.0f + static_cast<float>((microIndex % 3) * 12);

        if (addGeneratedObject(editorLayer, 141, {onsetX, onsetY})) {
            ++createdObjects;
            ++result.usedMicroOnsets;
            ++microIndex;
            previousMicroTime = onset.time;
        }

        if (result.usedMicroOnsets >= 128) {
            break;
        }
    }

    // Structured phrase generation. The template distribution is not arbitrary:
    // it is loaded from learned-profile-v1.json, currently produced from
    // Absolute Zero 2 + Sakura 2 aligned exports.
    std::size_t phraseIndex = 0;

    for (std::size_t start = 4; start + 3 < beats.size(); start += 4, ++phraseIndex) {
        float x0 = positionForBeat(beats[start]).x;
        float x1 = positionForBeat(beats[start + 1]).x;
        float x2 = positionForBeat(beats[start + 2]).x;
        float x3 = positionForBeat(beats[start + 3]).x;

        double phraseEnergy = 0.0;
        double phraseOnset = 0.0;

        for (std::size_t index = start; index < start + 4; ++index) {
            phraseEnergy += beats[index].energy;
            phraseOnset += beats[index].onset;
        }

        phraseEnergy /= 4.0;
        phraseOnset /= 4.0;

        auto phraseTemplate = choosePhraseTemplate(
            profile,
            phraseIndex,
            phraseEnergy,
            phraseOnset
        );

        ++result.phraseCount;

        // Cap complexity using the density learned from exported cube phrases.
        int eventBudget = std::clamp(
            phraseEnergy > 0.70
                ? profile.recommendedMaxEvents
                : profile.recommendedMaxEvents - 1,
            2,
            6
        );

        switch (phraseTemplate) {
            case PhraseTemplate::HazardOrbPad: {
                addCountedObject(
                    editorLayer,
                    35,
                    {x0, kGroundPadY},
                    createdObjects,
                    gameplayEvents
                );

                structuredBlocks += addBlockColumn(
                    editorLayer,
                    x1,
                    1,
                    createdObjects
                );
                structuredBlocks += addBlockColumn(
                    editorLayer,
                    x1 + 30.0f,
                    2,
                    createdObjects
                );
                structuredBlocks += addBlockColumn(
                    editorLayer,
                    x1 + 60.0f,
                    2,
                    createdObjects
                );

                addCountedObject(
                    editorLayer,
                    36,
                    {x2, kGroundSurfaceY + 105.0f},
                    createdObjects,
                    gameplayEvents
                );

                addCountedObject(
                    editorLayer,
                    8,
                    {x3, kGroundY},
                    createdObjects,
                    gameplayEvents
                );

                if (eventBudget >= 4 && phraseEnergy > 0.62) {
                    addCountedObject(
                        editorLayer,
                        141,
                        {(x2 + x3) * 0.5f, kGroundSurfaceY + 75.0f},
                        createdObjects,
                        gameplayEvents
                    );
                }
                break;
            }

            case PhraseTemplate::HazardPad: {
                addCountedObject(
                    editorLayer,
                    35,
                    {x0, kGroundPadY},
                    createdObjects,
                    gameplayEvents
                );

                structuredBlocks += addLowPlatform(
                    editorLayer,
                    x2,
                    3,
                    createdObjects
                );

                addCountedObject(
                    editorLayer,
                    8,
                    {x3, kGroundY},
                    createdObjects,
                    gameplayEvents
                );

                if (eventBudget >= 3) {
                    addCountedObject(
                        editorLayer,
                        8,
                        {x3 + 30.0f, kGroundY},
                        createdObjects,
                        gameplayEvents
                    );
                }
                break;
            }

            case PhraseTemplate::HazardOrb: {
                addCountedObject(
                    editorLayer,
                    8,
                    {x0, kGroundY},
                    createdObjects,
                    gameplayEvents
                );

                structuredBlocks += addLowPlatform(
                    editorLayer,
                    x2,
                    phraseEnergy > 0.58 ? 4 : 2,
                    createdObjects
                );

                addCountedObject(
                    editorLayer,
                    36,
                    {x2, kGroundSurfaceY + 90.0f},
                    createdObjects,
                    gameplayEvents
                );

                if (eventBudget >= 3) {
                    addCountedObject(
                        editorLayer,
                        8,
                        {x3, kGroundY},
                        createdObjects,
                        gameplayEvents
                    );
                }
                break;
            }

            case PhraseTemplate::HazardOnly: {
                addCountedObject(
                    editorLayer,
                    8,
                    {x1, kGroundY},
                    createdObjects,
                    gameplayEvents
                );

                if (eventBudget >= 3 || phraseEnergy > 0.52) {
                    addCountedObject(
                        editorLayer,
                        8,
                        {x1 + 30.0f, kGroundY},
                        createdObjects,
                        gameplayEvents
                    );
                }

                if (phraseIndex % 2 == 0) {
                    structuredBlocks += addLowPlatform(
                        editorLayer,
                        x3,
                        2,
                        createdObjects
                    );
                }
                break;
            }

            case PhraseTemplate::PadOnly: {
                addCountedObject(
                    editorLayer,
                    35,
                    {x1, kGroundPadY},
                    createdObjects,
                    gameplayEvents
                );

                structuredBlocks += addLowPlatform(
                    editorLayer,
                    x3,
                    3,
                    createdObjects
                );
                break;
            }

            case PhraseTemplate::Quiet: {
                // Quiet phrases deliberately breathe. A tiny safe platform gives
                // the section a visible contour without forcing a click.
                if (phraseIndex % 2 == 0) {
                    structuredBlocks += addLowPlatform(
                        editorLayer,
                        (x1 + x2) * 0.5f,
                        2,
                        createdObjects
                    );
                }
                break;
            }
        }

        // Busy phrases get one actual off-beat interaction near the strongest
        // micro-onset between the middle beats. This is the first step away from
        // "beat = object" toward subdivisions driving gameplay.
        if (
            phraseEnergy > 0.64
            && profile.denseSubdivisionWeight > 0.65
            && eventBudget >= 4
        ) {
            OnsetSample const* strongestMicro = nullptr;

            for (auto const& onset : usableOnsets) {
                if (onset.time <= beats[start + 1].time || onset.time >= beats[start + 3].time) {
                    continue;
                }
                if (nearestBeatDistance(beats, onset.time) < 0.090) {
                    continue;
                }
                if (!strongestMicro || onset.strength > strongestMicro->strength) {
                    strongestMicro = &onset;
                }
            }

            if (strongestMicro && strongestMicro->strength >= threshold) {
                float microX = positionForAudioTime(strongestMicro->time).x;
                addCountedObject(
                    editorLayer,
                    141,
                    {microX, kGroundSurfaceY + 75.0f},
                    createdObjects,
                    gameplayEvents
                );
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
    result.structuredBlocks = structuredBlocks;
    result.usedBeats = beats.size();
    result.firstBeatTime = beats.front().time;
    result.lastBeatTime = beats.back().time;
    return result;
}
