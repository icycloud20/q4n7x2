#include "LlmGenerator.hpp"

#include <Geode/binding/DrawGridLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {
constexpr float kGroundY = 105.0f;
constexpr float kMaximumY = 825.0f;
constexpr std::size_t kMaximumPreviewBeats = 128;

struct BeatTime {
    double time = 0.0;
};

struct PlannedAction {
    double beat = 0.0;
    std::string action;
    bool required = true;
    int heightDelta = 0;
    double intensity = 0.8;
};

struct PlannedSection {
    std::size_t index = 0;
    std::string mode = "cube";
    double intensity = 0.8;
    std::vector<PlannedAction> actions;
};

struct RoutePoint {
    float x = 0.0f;
    float y = 0.0f;
};

int portalObjectID(std::string const& mode) {
    if (mode == "ship") {
        return 13;
    }
    if (mode == "ball") {
        return 47;
    }
    if (mode == "ufo") {
        return 111;
    }
    if (mode == "wave") {
        return 660;
    }
    return 12;
}

int orbObjectID(std::string const& action) {
    if (action == "orb_pink") {
        return 141;
    }
    if (action == "orb_blue") {
        return 84;
    }
    if (action == "orb_green") {
        return 1022;
    }
    return 36;
}

int padObjectID(std::string const& action) {
    return action == "pad_pink" ? 140 : 35;
}

bool isOrb(std::string const& action) {
    return action == "orb_yellow"
        || action == "orb_pink"
        || action == "orb_blue"
        || action == "orb_green";
}

bool isPad(std::string const& action) {
    return action == "pad_yellow" || action == "pad_pink";
}

std::vector<BeatTime> readBeatTimes(
    std::filesystem::path const& analysisPath,
    std::string& error
) {
    std::ifstream input(analysisPath);
    if (!input) {
        error = "Could not open the cached song analysis.";
        return {};
    }

    auto parsed = matjson::parse(input);
    if (!parsed.isOk()) {
        error = "Could not parse the cached song analysis.";
        return {};
    }

    auto beatsResult = parsed.unwrap()["beats"].asArray();
    if (!beatsResult.isOk()) {
        error = "Song analysis does not contain beats.";
        return {};
    }

    std::vector<BeatTime> beats;
    for (auto const& value : beatsResult.unwrap()) {
        auto timeResult = value["time"].asDouble();
        if (!timeResult.isOk()) {
            continue;
        }

        beats.push_back({timeResult.unwrap()});
    }

    std::sort(
        beats.begin(),
        beats.end(),
        [](BeatTime const& left, BeatTime const& right) {
            return left.time < right.time;
        }
    );

    return beats;
}

std::vector<PlannedSection> readPlan(
    std::filesystem::path const& planPath,
    std::string& error
) {
    std::ifstream input(planPath);
    if (!input) {
        error = "Could not open the LLM gameplay plan.";
        return {};
    }

    auto parsed = matjson::parse(input);
    if (!parsed.isOk()) {
        error = "Could not parse the LLM gameplay plan.";
        return {};
    }

    auto root = parsed.unwrap();
    auto plan = root["plan"];
    auto sectionsResult = plan["sections"].asArray();
    if (!sectionsResult.isOk()) {
        error = "LLM plan does not contain sections.";
        return {};
    }

    std::vector<PlannedSection> sections;

    for (auto const& sectionValue : sectionsResult.unwrap()) {
        PlannedSection section;
        section.index = static_cast<std::size_t>(
            std::max(0.0, sectionValue["index"].asDouble().unwrapOr(0.0))
        );
        section.mode = sectionValue["mode"].asString().unwrapOr("cube");
        section.intensity = std::clamp(
            sectionValue["intensity"].asDouble().unwrapOr(0.8),
            0.0,
            1.0
        );

        auto actionsResult = sectionValue["actions"].asArray();
        if (actionsResult.isOk()) {
            for (auto const& actionValue : actionsResult.unwrap()) {
                PlannedAction action;
                action.beat = std::clamp(
                    actionValue["beat"].asDouble().unwrapOr(0.0),
                    0.0,
                    8.0
                );
                action.action = actionValue["action"].asString().unwrapOr("jump");
                action.required = actionValue["required"].asBool().unwrapOr(true);
                action.heightDelta = std::clamp(
                    static_cast<int>(
                        actionValue["height_delta"].asDouble().unwrapOr(0.0)
                    ),
                    -6,
                    6
                );
                action.intensity = std::clamp(
                    actionValue["intensity"].asDouble().unwrapOr(section.intensity),
                    0.0,
                    1.0
                );
                section.actions.push_back(std::move(action));
            }
        }

        std::sort(
            section.actions.begin(),
            section.actions.end(),
            [](PlannedAction const& left, PlannedAction const& right) {
                return left.beat < right.beat;
            }
        );

        sections.push_back(std::move(section));
    }

    std::sort(
        sections.begin(),
        sections.end(),
        [](PlannedSection const& left, PlannedSection const& right) {
            return left.index < right.index;
        }
    );

    return sections;
}

float interpolateRouteY(
    std::vector<RoutePoint> const& route,
    float x
) {
    if (route.empty()) {
        return 360.0f;
    }

    if (x <= route.front().x) {
        return route.front().y;
    }
    if (x >= route.back().x) {
        return route.back().y;
    }

    for (std::size_t index = 1; index < route.size(); ++index) {
        auto const& left = route[index - 1];
        auto const& right = route[index];

        if (x > right.x) {
            continue;
        }

        float width = std::max(1.0f, right.x - left.x);
        float alpha = std::clamp((x - left.x) / width, 0.0f, 1.0f);
        return left.y + (right.y - left.y) * alpha;
    }

    return route.back().y;
}
}

LlmGenerationResult generateLlmLayout(
    LevelEditorLayer* editorLayer,
    std::filesystem::path const& analysisPath,
    std::filesystem::path const& planPath
) {
    LlmGenerationResult result;

    if (!editorLayer || !editorLayer->m_levelSettings || !editorLayer->m_drawGridLayer) {
        result.error = "The editor is missing timing/settings data.";
        return result;
    }

    if (editorLayer->m_levelSettings->m_platformerMode) {
        result.error = "LLM generation currently supports classic mode only.";
        return result;
    }

    if (editorLayer->m_objects && editorLayer->m_objects->count() > 250) {
        result.error =
            "Use a mostly empty level for LLM generation so existing gameplay is not overwritten.";
        return result;
    }

    std::string readError;
    auto analysisBeats = readBeatTimes(analysisPath, readError);
    if (!readError.empty()) {
        result.error = readError;
        return result;
    }

    auto sections = readPlan(planPath, readError);
    if (!readError.empty()) {
        result.error = readError;
        return result;
    }

    double songOffset = editorLayer->m_levelSettings->m_songOffset;
    double minimumAudioTime = songOffset + 1.25;

    auto first = std::lower_bound(
        analysisBeats.begin(),
        analysisBeats.end(),
        minimumAudioTime,
        [](BeatTime const& beat, double value) {
            return beat.time < value;
        }
    );

    if (first == analysisBeats.end()) {
        result.error = "No usable beats occur after the current song offset.";
        return result;
    }

    std::vector<BeatTime> beats;
    for (
        auto iterator = first;
        iterator != analysisBeats.end() && beats.size() < kMaximumPreviewBeats;
        ++iterator
    ) {
        beats.push_back(*iterator);
    }

    if (beats.size() < 16) {
        result.error = "Not enough usable beats remain for LLM generation.";
        return result;
    }

    auto positionForAudioTime = [&](double audioTime) {
        float levelTime = static_cast<float>(std::max(0.0, audioTime - songOffset));
        return editorLayer->m_drawGridLayer->posForTime(levelTime);
    };

    auto positionForSectionBeat = [&](
        std::size_t sectionIndex,
        double beatOffset
    ) {
        std::size_t startIndex = 4 + sectionIndex * 8;
        startIndex = std::min(startIndex, beats.size() - 1);
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

    std::set<std::pair<int, int>> placedBlocks;
    std::set<std::tuple<int, int, int>> placedActions;

    auto snap = [](CCPoint point) {
        point.x = std::round(point.x / 15.0f) * 15.0f;
        point.y = std::round(point.y / 15.0f) * 15.0f;
        return point;
    };

    auto addBlock = [&](float x, float y) {
        CCPoint position = snap({
            x,
            std::clamp(y, kGroundY, kMaximumY),
        });
        auto key = std::make_pair(
            static_cast<int>(std::round(position.x)),
            static_cast<int>(std::round(position.y))
        );
        if (!placedBlocks.insert(key).second) {
            return false;
        }

        if (!editorLayer->createObject(1, position, true)) {
            return false;
        }

        ++result.createdObjects;
        return true;
    };

    auto addActionObject = [&](
        int objectID,
        float x,
        float y,
        float rotation = 0.0f
    ) {
        CCPoint position = snap({
            x,
            std::clamp(y, kGroundY, kMaximumY),
        });
        auto key = std::make_tuple(
            objectID,
            static_cast<int>(std::round(position.x)),
            static_cast<int>(std::round(position.y))
        );
        if (!placedActions.insert(key).second) {
            return false;
        }

        auto* object = editorLayer->createObject(objectID, position, true);
        if (!object) {
            return false;
        }

        object->setRotation(rotation);
        ++result.createdObjects;
        ++result.gameplayEvents;
        return true;
    };

    auto addPlatform = [&](float centerX, float blockY, int width) {
        width = std::clamp(width, 2, 5);
        float startX = centerX - static_cast<float>(width - 1) * 15.0f;

        for (int index = 0; index < width; ++index) {
            addBlock(startX + static_cast<float>(index * 30), blockY);
        }
    };

    auto addRailSegment = [&](
        RoutePoint const& left,
        RoutePoint const& right,
        float gap
    ) {
        float startX = std::min(left.x, right.x);
        float endX = std::max(left.x, right.x);

        for (float x = startX; x <= endX + 1.0f; x += 30.0f) {
            float y = interpolateRouteY({left, right}, x);
            addBlock(x, y - gap);
            addBlock(x, y + gap);
        }
    };

    auto addRouteRails = [&](
        std::vector<RoutePoint> const& route,
        float gap
    ) {
        if (route.size() < 2) {
            return;
        }

        for (std::size_t index = 1; index < route.size(); ++index) {
            addRailSegment(route[index - 1], route[index], gap);
        }
    };

    float pathY = 135.0f;
    std::string currentMode = "cube";

    for (auto const& section : sections) {
        std::size_t beatStartIndex = 4 + section.index * 8;
        if (beatStartIndex + 8 >= beats.size()) {
            break;
        }

        float sectionStartX = positionForSectionBeat(section.index, 0.0).x;
        float sectionEndX = positionForSectionBeat(section.index, 8.0).x;

        if (
            !std::isfinite(sectionStartX)
            || !std::isfinite(sectionEndX)
            || sectionEndX <= sectionStartX
        ) {
            continue;
        }

        if (section.mode != currentMode) {
            addActionObject(
                portalObjectID(section.mode),
                sectionStartX,
                std::clamp(pathY, 135.0f, 690.0f)
            );
            currentMode = section.mode;
            ++result.modeTransitions;
        }

        if (section.mode == "cube") {
            pathY = std::clamp(pathY, 135.0f, 690.0f);
            addPlatform(sectionStartX + 30.0f, pathY - 30.0f, 4);

            int actionIndex = 0;
            for (auto const& action : section.actions) {
                float x = positionForSectionBeat(section.index, action.beat).x;

                if (action.action == "land") {
                    pathY = std::clamp(
                        pathY + static_cast<float>(action.heightDelta * 30),
                        135.0f,
                        690.0f
                    );
                    addPlatform(x, pathY - 30.0f, action.intensity > 0.82 ? 2 : 3);
                } else if (action.action == "jump") {
                    addPlatform(x - 30.0f, pathY - 30.0f, 3);
                    addActionObject(8, x, pathY);
                } else if (isOrb(action.action)) {
                    addActionObject(
                        orbObjectID(action.action),
                        x,
                        pathY + 60.0f
                    );
                    pathY = std::clamp(
                        pathY + static_cast<float>(action.heightDelta * 30),
                        135.0f,
                        690.0f
                    );
                } else if (isPad(action.action)) {
                    addPlatform(x, pathY - 30.0f, 3);
                    addActionObject(
                        padObjectID(action.action),
                        x,
                        pathY - 15.0f
                    );
                    pathY = std::clamp(
                        pathY + static_cast<float>(action.heightDelta * 30),
                        135.0f,
                        690.0f
                    );
                }

                ++actionIndex;
            }

            addPlatform(sectionEndX - 45.0f, pathY - 30.0f, 4);
        } else if (section.mode == "ship" || section.mode == "wave") {
            pathY = std::clamp(pathY, 240.0f, 585.0f);
            std::vector<RoutePoint> route = {
                {sectionStartX, pathY},
            };

            int direction = 1;
            for (auto const& action : section.actions) {
                bool routeAction =
                    action.action == "hold"
                    || action.action == "release"
                    || action.action == "wave_hold"
                    || action.action == "wave_release";

                if (!routeAction) {
                    continue;
                }

                float x = positionForSectionBeat(section.index, action.beat).x;
                int steps = action.heightDelta;
                if (steps == 0) {
                    steps = direction * (
                        section.mode == "wave" ? 2 : 1
                    );
                }

                pathY = std::clamp(
                    pathY + static_cast<float>(steps * 30),
                    195.0f,
                    630.0f
                );
                route.push_back({x, pathY});
                direction *= -1;
            }

            route.push_back({sectionEndX, pathY});
            std::sort(
                route.begin(),
                route.end(),
                [](RoutePoint const& left, RoutePoint const& right) {
                    return left.x < right.x;
                }
            );

            float gap = section.mode == "wave"
                ? std::clamp(
                    125.0f - static_cast<float>(section.intensity) * 25.0f,
                    90.0f,
                    115.0f
                )
                : std::clamp(
                    175.0f - static_cast<float>(section.intensity) * 40.0f,
                    125.0f,
                    165.0f
                );
            addRouteRails(route, gap);

            int gateIndex = 0;
            for (auto const& action : section.actions) {
                bool gateAction =
                    action.action == "hold"
                    || action.action == "release"
                    || action.action == "wave_hold"
                    || action.action == "wave_release";
                if (!gateAction) {
                    continue;
                }

                float x = positionForSectionBeat(section.index, action.beat).x;
                float centerY = interpolateRouteY(route, x);
                bool fromFloor = gateIndex % 2 == 0;
                int blocks = section.mode == "wave" ? 1 : 2;

                for (int layer = 1; layer <= blocks; ++layer) {
                    float y = fromFloor
                        ? centerY - gap + static_cast<float>(layer * 30)
                        : centerY + gap - static_cast<float>(layer * 30);
                    addBlock(x, y);
                }

                ++gateIndex;
            }
        } else if (section.mode == "ufo") {
            pathY = std::clamp(pathY, 240.0f, 585.0f);
            std::vector<RoutePoint> route = {
                {sectionStartX, pathY},
            };

            int direction = 1;
            for (auto const& action : section.actions) {
                if (action.action != "ufo_click") {
                    continue;
                }

                float x = positionForSectionBeat(section.index, action.beat).x;
                int steps = action.heightDelta != 0
                    ? action.heightDelta
                    : direction * 2;
                pathY = std::clamp(
                    pathY + static_cast<float>(steps * 30),
                    225.0f,
                    600.0f
                );
                route.push_back({x, pathY});
                direction *= -1;
            }
            route.push_back({sectionEndX, pathY});
            std::sort(
                route.begin(),
                route.end(),
                [](RoutePoint const& left, RoutePoint const& right) {
                    return left.x < right.x;
                }
            );

            float gap = std::clamp(
                180.0f - static_cast<float>(section.intensity) * 35.0f,
                135.0f,
                170.0f
            );
            addRouteRails(route, gap);

            int clickIndex = 0;
            for (auto const& action : section.actions) {
                if (action.action != "ufo_click") {
                    continue;
                }

                float x = positionForSectionBeat(section.index, action.beat).x;
                float centerY = interpolateRouteY(route, x);
                bool fromFloor = clickIndex % 2 == 0;
                int height = action.intensity > 0.82 ? 3 : 2;

                for (int layer = 1; layer <= height; ++layer) {
                    float y = fromFloor
                        ? centerY - gap + static_cast<float>(layer * 30)
                        : centerY + gap - static_cast<float>(layer * 30);
                    addBlock(x, y);
                }

                ++clickIndex;
            }
        } else if (section.mode == "ball") {
            float centerY = std::clamp(pathY, 270.0f, 555.0f);
            float halfGap = std::clamp(
                165.0f - static_cast<float>(section.intensity) * 30.0f,
                120.0f,
                155.0f
            );
            float floorY = std::max(kGroundY, centerY - halfGap);
            float ceilingY = std::min(kMaximumY, centerY + halfGap);

            for (float x = sectionStartX; x <= sectionEndX + 1.0f; x += 30.0f) {
                addBlock(x, floorY);
                addBlock(x, ceilingY);
            }

            bool inverted = false;
            for (auto const& action : section.actions) {
                if (action.action != "gravity_flip") {
                    continue;
                }

                float x = positionForSectionBeat(section.index, action.beat).x;
                float spikeY = inverted
                    ? ceilingY - 30.0f
                    : floorY + 30.0f;
                addActionObject(
                    8,
                    x,
                    spikeY,
                    inverted ? 180.0f : 0.0f
                );
                inverted = !inverted;
            }

            pathY = centerY;
        }

        ++result.sections;
    }

    if (result.createdObjects == 0) {
        result.error = "The LLM plan did not compile into any gameplay objects.";
        return result;
    }

    result.success = true;
    return result;
}
