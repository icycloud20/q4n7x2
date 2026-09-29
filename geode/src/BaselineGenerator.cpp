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


enum class MotifKind {
    Solid = 0,
    Slope = 1,
    Hazard = 2,
    Orb = 3,
    Pad = 4,
    Portal = 5,
};

struct MotifEvent {
    MotifKind kind = MotifKind::Solid;
    int objectID = 1;
    double beatOffset = 0.0;
    float relativeY = 0.0f;
    float rotation = 0.0f;
};

struct StructuralMotif {
    bool entryInverted = false;
    bool exitInverted = false;
    bool entryMini = false;
    bool exitMini = false;
    float exitDeltaY = 0.0f;
    float minRelativeY = 0.0f;
    float maxRelativeY = 0.0f;
    double intensity = 0.0;
    std::vector<MotifEvent> events;
};

struct StructuralProfile {
    bool loaded = false;
    std::size_t sourceLevels = 0;
    std::vector<StructuralMotif> motifs;
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

std::size_t addPlatformSpan(
    LevelEditorLayer* editorLayer,
    float startX,
    float endX,
    int height,
    std::size_t& createdObjects
) {
    if (height <= 0 || endX <= startX) {
        return 0;
    }

    std::size_t added = 0;
    float x = startX;

    while (x <= endX + 1.0f) {
        for (int layer = 0; layer < height; ++layer) {
            float y = kGroundY + static_cast<float>(layer * 30);
            if (addGeneratedObject(editorLayer, 1, {x, y})) {
                ++added;
                ++createdObjects;
            }
        }
        x += 30.0f;
    }

    return added;
}

float spikeYForHeight(int height) {
    return kGroundY + static_cast<float>(std::max(0, height) * 30);
}

float padYForHeight(int height) {
    return kGroundPadY + static_cast<float>(std::max(0, height) * 30);
}

float orbYForHeight(int height, float extra = 90.0f) {
    return kGroundSurfaceY + static_cast<float>(std::max(0, height) * 30) + extra;
}


bool addGeneratedObjectWithRotation(
    LevelEditorLayer* editorLayer,
    int objectID,
    CCPoint const& position,
    float rotation
) {
    if (!editorLayer) {
        return false;
    }

    auto* object = editorLayer->createObject(objectID, position, true);
    if (!object) {
        return false;
    }

    object->setRotation(rotation);
    return true;
}

std::size_t addHorizontalBridge(
    LevelEditorLayer* editorLayer,
    float startX,
    float endX,
    float y,
    std::size_t& createdObjects
) {
    if (!editorLayer || endX <= startX) {
        return 0;
    }

    std::size_t added = 0;
    float x = startX;

    while (x <= endX + 1.0f && added < 18) {
        if (addGeneratedObject(editorLayer, 1, {x, y})) {
            ++added;
            ++createdObjects;
        }
        x += 30.0f;
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


StructuralProfile readStructuralProfile() {
    StructuralProfile profile;
    auto profilePath = Mod::get()->getResourcesDir() / "learned-motifs-v2.json";

    std::ifstream input(profilePath);
    if (!input) {
        log::warn("Learned structural motif profile is missing: {}", profilePath.string());
        return profile;
    }

    auto parsed = matjson::parse(input);
    if (!parsed.isOk()) {
        log::warn("Could not parse learned structural motif profile: {}", profilePath.string());
        return profile;
    }

    auto root = parsed.unwrap();
    profile.sourceLevels = static_cast<std::size_t>(
        std::max(0.0, root["n"].asDouble().unwrapOr(0.0))
    );

    auto motifsResult = root["motifs"].asArray();
    if (!motifsResult.isOk()) {
        return profile;
    }

    for (auto const& value : motifsResult.unwrap()) {
        StructuralMotif motif;
        motif.entryInverted = value["g0"].asDouble().unwrapOr(0.0) > 0.5;
        motif.exitInverted = value["g1"].asDouble().unwrapOr(0.0) > 0.5;
        motif.entryMini = value["m0"].asDouble().unwrapOr(0.0) > 0.5;
        motif.exitMini = value["m1"].asDouble().unwrapOr(0.0) > 0.5;
        motif.exitDeltaY = static_cast<float>(value["dy"].asDouble().unwrapOr(0.0));
        motif.minRelativeY = static_cast<float>(value["mn"].asDouble().unwrapOr(0.0));
        motif.maxRelativeY = static_cast<float>(value["mx"].asDouble().unwrapOr(0.0));
        motif.intensity = value["q"].asDouble().unwrapOr(0.0);

        auto eventsResult = value["e"].asArray();
        if (!eventsResult.isOk()) {
            continue;
        }

        for (auto const& eventValue : eventsResult.unwrap()) {
            auto fieldsResult = eventValue.asArray();
            if (!fieldsResult.isOk()) {
                continue;
            }

            auto const& fields = fieldsResult.unwrap();
            if (fields.size() < 5) {
                continue;
            }

            MotifEvent event;
            event.kind = static_cast<MotifKind>(
                std::clamp(
                    static_cast<int>(fields[0].asDouble().unwrapOr(0.0)),
                    0,
                    5
                )
            );
            event.objectID = std::max(
                1,
                static_cast<int>(fields[1].asDouble().unwrapOr(1.0))
            );
            event.beatOffset = fields[2].asDouble().unwrapOr(0.0);
            event.relativeY = static_cast<float>(fields[3].asDouble().unwrapOr(0.0));
            event.rotation = static_cast<float>(fields[4].asDouble().unwrapOr(0.0));
            motif.events.push_back(event);
        }

        if (motif.events.size() >= 4) {
            profile.motifs.push_back(std::move(motif));
        }
    }

    profile.loaded = !profile.motifs.empty();

    if (profile.loaded) {
        log::info(
            "Loaded {} learned 8-beat structural motifs from {} source levels",
            profile.motifs.size(),
            profile.sourceLevels
        );
    }

    return profile;
}

StructuralMotif const* chooseStructuralMotif(
    StructuralProfile const& profile,
    std::size_t chunkIndex,
    double targetIntensity,
    float anchorY,
    bool inverted,
    bool mini,
    StructuralMotif const* previous,
    StructuralMotif const* twoBack
) {
    StructuralMotif const* best = nullptr;
    double bestScore = std::numeric_limits<double>::infinity();

    for (std::size_t index = 0; index < profile.motifs.size(); ++index) {
        auto const& motif = profile.motifs[index];

        if (motif.entryInverted != inverted || motif.entryMini != mini) {
            continue;
        }

        // Path-first v3 deliberately rejects state-changing / extreme motifs
        // until their entire transition can be solved as a verified path. A
        // partial gravity/mini/portal sequence is much worse than a simpler
        // playable connector.
        if (motif.exitInverted != inverted || motif.exitMini != mini) {
            continue;
        }

        if (
            std::abs(motif.exitDeltaY) > 120.0f
            || motif.maxRelativeY - motif.minRelativeY > 270.0f
        ) {
            continue;
        }

        bool containsPortal = false;
        std::size_t actionCount = 0;

        for (auto const& event : motif.events) {
            if (event.kind == MotifKind::Portal) {
                containsPortal = true;
                break;
            }

            if (event.kind == MotifKind::Orb || event.kind == MotifKind::Pad) {
                ++actionCount;
            }
        }

        if (containsPortal || actionCount > 8) {
            continue;
        }

        float adjustedAnchor = anchorY;
        if (adjustedAnchor + motif.minRelativeY < kGroundY) {
            adjustedAnchor = kGroundY - motif.minRelativeY;
        }
        if (adjustedAnchor + motif.maxRelativeY > 900.0f) {
            adjustedAnchor = 900.0f - motif.maxRelativeY;
        }

        if (adjustedAnchor < kGroundY - 1.0f) {
            continue;
        }

        double heightShiftPenalty =
            std::abs(static_cast<double>(adjustedAnchor - anchorY)) / 450.0;
        double deterministicJitter = std::fmod(
            static_cast<double>(chunkIndex + 1) * 0.61803398875
                + static_cast<double>(index + 1) * 0.41421356237,
            1.0
        ) * 0.16;

        double repeatPenalty = 0.0;
        if (&motif == previous) {
            repeatPenalty += 3.0;
        }
        if (&motif == twoBack) {
            repeatPenalty += 1.25;
        }

        double score =
            std::abs(motif.intensity - targetIntensity)
            + heightShiftPenalty
            + deterministicJitter
            + repeatPenalty;

        if (score < bestScore) {
            bestScore = score;
            best = &motif;
        }
    }

    return best;
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

    // Debug beat / micro-onset object lanes were removed. They were useful
    // while validating timing, but they interfered with actual playtesting.

    // Structural motif generation v2. Rather than asking a four-beat phrase for
    // a bag of object categories, this adapts complete eight-beat relative
    // structures learned from clean human layout exports.
    auto structuralProfile = readStructuralProfile();
    result.learnedStructuralProfileLoaded = structuralProfile.loaded;
    result.learnedMotifCount = structuralProfile.motifs.size();

    float pathAnchorY = kGroundY;
    bool currentInverted = false;
    bool currentMini = editorLayer->m_levelSettings->m_startMini;
    std::size_t generatedMotifs = 0;
    StructuralMotif const* previousMotif = nullptr;
    StructuralMotif const* twoBackMotif = nullptr;

    struct PlacedStructure {
        CCPoint position;
        MotifKind kind;
    };
    std::vector<PlacedStructure> placedStructures;
    std::vector<CCPoint> placedActions;

    auto snappedStructurePosition = [](CCPoint position) {
        position.x = std::round(position.x / 15.0f) * 15.0f;
        position.y = std::round(position.y / 15.0f) * 15.0f;
        return position;
    };

    auto structureConflicts = [&](CCPoint const& position, MotifKind kind) {
        for (auto const& placed : placedStructures) {
            float dx = std::abs(position.x - placed.position.x);
            float dy = std::abs(position.y - placed.position.y);

            // Block centers closer than one grid cell overlap. Slopes get a
            // little more breathing room because their collision spans wider.
            float xLimit =
                (kind == MotifKind::Slope || placed.kind == MotifKind::Slope)
                    ? 34.0f
                    : 29.0f;
            float yLimit =
                (kind == MotifKind::Slope || placed.kind == MotifKind::Slope)
                    ? 24.0f
                    : 29.0f;

            if (dx < xLimit && dy < yLimit) {
                return true;
            }
        }
        return false;
    };

    auto addCleanStructure = [&](
        int objectID,
        MotifKind kind,
        CCPoint position,
        float rotation
    ) {
        position = snappedStructurePosition(position);

        if (structureConflicts(position, kind)) {
            return false;
        }

        if (!addGeneratedObjectWithRotation(
            editorLayer,
            objectID,
            position,
            rotation
        )) {
            return false;
        }

        placedStructures.push_back({position, kind});
        ++createdObjects;
        ++structuredBlocks;
        return true;
    };

    auto addCleanBridge = [&](float startX, float endX, float y) {
        std::size_t added = 0;
        for (float x = startX; x <= endX + 1.0f && added < 18; x += 30.0f) {
            if (addCleanStructure(1, MotifKind::Solid, {x, y}, 0.0f)) {
                ++added;
            }
        }
        return added;
    };

    auto addCleanAction = [&](
        int objectID,
        CCPoint position,
        float rotation
    ) {
        position.x = std::round(position.x / 15.0f) * 15.0f;
        position.y = std::round(position.y / 15.0f) * 15.0f;

        for (auto const& placed : placedActions) {
            if (
                std::abs(position.x - placed.x) < 24.0f
                && std::abs(position.y - placed.y) < 30.0f
            ) {
                return false;
            }
        }

        if (!addGeneratedObjectWithRotation(
            editorLayer,
            objectID,
            position,
            rotation
        )) {
            return false;
        }

        placedActions.push_back(position);
        ++createdObjects;
        ++gameplayEvents;
        return true;
    };

    auto addLandingPlatform = [&](
        float centerX,
        float y,
        int blockCount
    ) {
        blockCount = std::clamp(blockCount, 2, 4);
        float startX = centerX - static_cast<float>(blockCount - 1) * 15.0f;
        std::size_t added = 0;

        for (int index = 0; index < blockCount; ++index) {
            float x = startX + static_cast<float>(index * 30);
            if (addCleanStructure(1, MotifKind::Solid, {x, y}, 0.0f)) {
                ++added;
            }
        }

        return added;
    };

    auto positionForBeatOffset = [&](std::size_t startIndex, double beatOffset) {
        beatOffset = std::clamp(beatOffset, 0.0, 8.0);

        auto whole = static_cast<std::size_t>(std::floor(beatOffset));
        double fraction = beatOffset - static_cast<double>(whole);
        std::size_t leftIndex = std::min(startIndex + whole, beats.size() - 1);

        if (fraction <= 0.000001 || leftIndex + 1 >= beats.size()) {
            return positionForAudioTime(beats[leftIndex].time);
        }

        double time =
            beats[leftIndex].time
            + (beats[leftIndex + 1].time - beats[leftIndex].time) * fraction;
        return positionForAudioTime(time);
    };

    if (structuralProfile.loaded && !currentMini) {
        for (
            std::size_t start = 4;
            start + 8 < beats.size();
            start += 8, ++generatedMotifs
        ) {
            double chunkEnergy = 0.0;
            double chunkOnset = 0.0;

            for (std::size_t index = start; index < start + 8; ++index) {
                chunkEnergy += beats[index].energy;
                chunkOnset += beats[index].onset;
            }

            chunkEnergy /= 8.0;
            chunkOnset /= 8.0;
            double targetIntensity = std::clamp(
                chunkEnergy * 0.68 + chunkOnset * 0.32,
                0.0,
                1.0
            );

            auto const* motif = chooseStructuralMotif(
                structuralProfile,
                generatedMotifs,
                targetIntensity,
                pathAnchorY,
                currentInverted,
                currentMini,
                previousMotif,
                twoBackMotif
            );

            if (!motif) {
                // If the current state has no matching motif, keep a safe short
                // connector instead of inventing a random object combination.
                float x0 = positionForBeat(beats[start]).x;
                float x8 = positionForBeat(beats[start + 8]).x;
                addCleanBridge(x0, x8, pathAnchorY);
                continue;
            }

            float motifAnchorY = pathAnchorY;

            // Path-first v3.1: learn the *kind* of gameplay from the motif,
            // then build a fresh, conservative cube path around real landings.
            // Raw source coordinates and gravity-changing interactions are not
            // copied because they only make sense inside their original level.
            int learnedHazards = 0;
            int learnedOrbs = 0;
            int learnedPads = 0;

            for (auto const& event : motif->events) {
                if (event.kind == MotifKind::Hazard) {
                    ++learnedHazards;
                } else if (event.kind == MotifKind::Orb) {
                    ++learnedOrbs;
                } else if (event.kind == MotifKind::Pad) {
                    ++learnedPads;
                }
            }

            float requestedDeltaY = std::clamp(
                motif->exitDeltaY,
                -120.0f,
                120.0f
            );
            float snappedDeltaY =
                std::round(requestedDeltaY / 30.0f) * 30.0f;

            // Do not repeatedly ask for downward movement while already on the
            // floor. Turn that intent into a small climb so the layout develops
            // actual shape instead of becoming one endless flat trampoline row.
            if (motifAnchorY <= kGroundY + 1.0f && snappedDeltaY < 0.0f) {
                snappedDeltaY = targetIntensity > 0.68 ? 60.0f : 30.0f;
            }

            if (motifAnchorY >= 690.0f && snappedDeltaY > 0.0f) {
                snappedDeltaY = -std::max(30.0f, snappedDeltaY);
            }

            float targetY = std::clamp(
                motifAnchorY + snappedDeltaY,
                kGroundY,
                720.0f
            );

            float chunkStartX = positionForBeatOffset(start, 0.0).x;
            float chunkEndX = positionForBeatOffset(start, 8.0).x;
            float chunkWidth = chunkEndX - chunkStartX;

            if (
                !std::isfinite(chunkStartX)
                || !std::isfinite(chunkEndX)
                || chunkWidth <= 0.0f
            ) {
                continue;
            }

            int horizontalSegments = std::max(
                4,
                static_cast<int>(std::ceil(chunkWidth / 175.0f))
            );
            int verticalSegments = static_cast<int>(
                std::ceil(std::abs(targetY - motifAnchorY) / 30.0f)
            );
            int pathSegments = std::clamp(
                std::max(horizontalSegments, verticalSegments),
                4,
                10
            );

            struct PlannedLanding {
                float x = 0.0f;
                float y = kGroundY;
                int width = 3;
            };

            std::vector<PlannedLanding> landings;
            landings.reserve(static_cast<std::size_t>(pathSegments + 1));

            float previousY = motifAnchorY;
            float bumpAmplitude = targetIntensity > 0.72 ? 60.0f : 30.0f;
            float bumpDirection = motifAnchorY > 540.0f ? -1.0f : 1.0f;

            for (int segment = 0; segment <= pathSegments; ++segment) {
                double progress =
                    static_cast<double>(segment)
                    / static_cast<double>(pathSegments);
                float x =
                    chunkStartX
                    + chunkWidth * static_cast<float>(progress);

                float linearY =
                    motifAnchorY
                    + (targetY - motifAnchorY) * static_cast<float>(progress);
                float arcY =
                    std::sin(static_cast<float>(progress) * 3.14159265f)
                    * bumpAmplitude
                    * bumpDirection;
                float candidateY =
                    std::round((linearY + arcY) / 30.0f) * 30.0f;

                if (segment == 0) {
                    candidateY = motifAnchorY;
                } else if (segment == pathSegments) {
                    candidateY = targetY;
                } else {
                    candidateY = std::clamp(
                        candidateY,
                        previousY - 30.0f,
                        previousY + 30.0f
                    );

                    int remaining = pathSegments - segment;
                    float minimumReachable =
                        targetY - static_cast<float>(remaining) * 30.0f;
                    float maximumReachable =
                        targetY + static_cast<float>(remaining) * 30.0f;
                    candidateY = std::clamp(
                        candidateY,
                        minimumReachable,
                        maximumReachable
                    );
                }

                candidateY = std::clamp(candidateY, kGroundY, 720.0f);

                int width = 3;
                if (segment == 0 || segment == pathSegments) {
                    width = 4;
                } else if ((segment + static_cast<int>(generatedMotifs)) % 3 == 0) {
                    width = 4;
                }

                landings.push_back({x, candidateY, width});
                previousY = candidateY;
            }

            // Use one learned orb/pad idea at most per chunk. Their *positions*
            // are designed here around the solved path rather than copied from
            // the source song.
            int orbTransition = -1;
            if (learnedOrbs > 0 && pathSegments >= 4) {
                orbTransition = std::clamp(pathSegments / 2, 1, pathSegments - 2);
            }

            int padTransition = -1;
            if (learnedPads > 0 && pathSegments >= 3) {
                int preferred =
                    generatedMotifs % 2 == 0 ? 1 : pathSegments - 2;

                if (preferred == orbTransition) {
                    preferred =
                        preferred == 1
                            ? std::min(2, pathSegments - 1)
                            : std::max(0, pathSegments - 3);
                }

                if (preferred != orbTransition) {
                    padTransition = preferred;
                }
            }

            if (orbTransition >= 0) {
                // Give the orb transition a real gap and a generous landing.
                landings[static_cast<std::size_t>(orbTransition)].width = 2;
                landings[static_cast<std::size_t>(orbTransition + 1)].width = 4;
            }

            for (auto const& landing : landings) {
                addLandingPlatform(landing.x, landing.y, landing.width);
            }

            int hazardBudget = std::clamp(
                learnedHazards + (targetIntensity > 0.62 ? 1 : 0),
                1,
                3
            );
            int hazardsPlaced = 0;

            for (int transition = 0; transition < pathSegments; ++transition) {
                auto const& current =
                    landings[static_cast<std::size_t>(transition)];
                auto const& next =
                    landings[static_cast<std::size_t>(transition + 1)];

                if (transition == orbTransition) {
                    float orbX = current.x + (next.x - current.x) * 0.48f;
                    float orbY = std::max(current.y, next.y) + 60.0f;

                    // Normalize every learned orb to a normal yellow jump orb.
                    // Blue/green/spider/etc. effects require their own physics
                    // solver and are rejected from this cube-only path pass.
                    addCleanAction(36, {orbX, orbY}, 0.0f);
                    continue;
                }

                if (transition == padTransition) {
                    int padObjectID =
                        next.y > current.y + 1.0f ? 35 : 140;
                    float padX = current.x + 15.0f;
                    float padY = current.y - 13.0f;

                    // Yellow or pink only: both preserve normal gravity.
                    addCleanAction(padObjectID, {padX, padY}, 0.0f);
                    continue;
                }

                if (
                    hazardsPlaced < hazardBudget
                    && current.width >= 3
                    && (
                        transition % 2 == 0
                        || targetIntensity > 0.72
                    )
                ) {
                    float spikeX =
                        current.x
                        + (current.width >= 4 ? 30.0f : 15.0f);

                    // Small spike: enough to demand a jump without turning each
                    // island into a blind or frame-perfect landing.
                    if (addCleanAction(8, {spikeX, current.y}, 0.0f)) {
                        ++hazardsPlaced;
                    }
                }
            }

            twoBackMotif = previousMotif;
            previousMotif = motif;

            // Advance from the path we actually built, never from discarded raw
            // geometry. This keeps the next chunk anchored to a real landing.
            pathAnchorY = targetY;
            currentInverted = motif->exitInverted;
            currentMini = motif->exitMini;
            ++result.phraseCount;
        }
    } else {
        // Small fallback only for a missing/corrupt motif resource. Normal builds
        // should never take this path.
        for (std::size_t start = 4; start + 3 < beats.size(); start += 4) {
            float x0 = positionForBeat(beats[start]).x;
            float x2 = positionForBeat(beats[start + 2]).x;

            addCleanBridge(x0 - 30.0f, x2, kGroundY);

            if (addCountedObject(
                editorLayer,
                8,
                {x2 + 30.0f, kGroundY},
                createdObjects,
                gameplayEvents
            )) {
                ++result.phraseCount;
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
