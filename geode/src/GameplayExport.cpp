#include "GameplayExport.hpp"

#include <Geode/Enums.hpp>
#include <Geode/binding/DrawGridLayer.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <vector>

using namespace geode::prelude;

namespace {
std::string objectTypeName(GameObjectType type) {
    switch (type) {
        case GameObjectType::Solid: return "solid";
        case GameObjectType::Hazard: return "hazard";
        case GameObjectType::InverseGravityPortal: return "inverse_gravity_portal";
        case GameObjectType::NormalGravityPortal: return "normal_gravity_portal";
        case GameObjectType::ShipPortal: return "ship_portal";
        case GameObjectType::CubePortal: return "cube_portal";
        case GameObjectType::Decoration: return "decoration";
        case GameObjectType::YellowJumpPad: return "yellow_pad";
        case GameObjectType::PinkJumpPad: return "pink_pad";
        case GameObjectType::GravityPad: return "gravity_pad";
        case GameObjectType::YellowJumpRing: return "yellow_orb";
        case GameObjectType::PinkJumpRing: return "pink_orb";
        case GameObjectType::GravityRing: return "gravity_orb";
        case GameObjectType::InverseMirrorPortal: return "mirror_on_portal";
        case GameObjectType::NormalMirrorPortal: return "mirror_off_portal";
        case GameObjectType::BallPortal: return "ball_portal";
        case GameObjectType::RegularSizePortal: return "normal_size_portal";
        case GameObjectType::MiniSizePortal: return "mini_portal";
        case GameObjectType::UfoPortal: return "ufo_portal";
        case GameObjectType::Modifier: return "modifier";
        case GameObjectType::Breakable: return "breakable";
        case GameObjectType::SecretCoin: return "secret_coin";
        case GameObjectType::DualPortal: return "dual_portal";
        case GameObjectType::SoloPortal: return "solo_portal";
        case GameObjectType::Slope: return "slope";
        case GameObjectType::WavePortal: return "wave_portal";
        case GameObjectType::RobotPortal: return "robot_portal";
        case GameObjectType::TeleportPortal: return "teleport_portal";
        case GameObjectType::GreenRing: return "green_orb";
        case GameObjectType::Collectible: return "collectible";
        case GameObjectType::UserCoin: return "user_coin";
        case GameObjectType::DropRing: return "drop_orb";
        case GameObjectType::SpiderPortal: return "spider_portal";
        case GameObjectType::RedJumpPad: return "red_pad";
        case GameObjectType::RedJumpRing: return "red_orb";
        case GameObjectType::CustomRing: return "custom_orb";
        case GameObjectType::DashRing: return "dash_orb";
        case GameObjectType::GravityDashRing: return "gravity_dash_orb";
        case GameObjectType::CollisionObject: return "collision_object";
        case GameObjectType::Special: return "special";
        case GameObjectType::SwingPortal: return "swing_portal";
        case GameObjectType::GravityTogglePortal: return "gravity_toggle_portal";
        case GameObjectType::SpiderOrb: return "spider_orb";
        case GameObjectType::SpiderPad: return "spider_pad";
        case GameObjectType::EnterEffectObject: return "enter_effect";
        case GameObjectType::TeleportOrb: return "teleport_orb";
        case GameObjectType::AnimatedHazard: return "animated_hazard";
    }

    return "unknown";
}

std::string objectCategory(GameObjectType type) {
    switch (type) {
        case GameObjectType::Solid:
        case GameObjectType::Breakable:
        case GameObjectType::Slope:
            return "solid";

        case GameObjectType::Hazard:
        case GameObjectType::AnimatedHazard:
            return "hazard";

        case GameObjectType::YellowJumpPad:
        case GameObjectType::PinkJumpPad:
        case GameObjectType::GravityPad:
        case GameObjectType::RedJumpPad:
        case GameObjectType::SpiderPad:
            return "pad";

        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::DropRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::CustomRing:
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing:
        case GameObjectType::SpiderOrb:
        case GameObjectType::TeleportOrb:
            return "orb";

        case GameObjectType::InverseGravityPortal:
        case GameObjectType::NormalGravityPortal:
        case GameObjectType::ShipPortal:
        case GameObjectType::CubePortal:
        case GameObjectType::InverseMirrorPortal:
        case GameObjectType::NormalMirrorPortal:
        case GameObjectType::BallPortal:
        case GameObjectType::RegularSizePortal:
        case GameObjectType::MiniSizePortal:
        case GameObjectType::UfoPortal:
        case GameObjectType::DualPortal:
        case GameObjectType::SoloPortal:
        case GameObjectType::WavePortal:
        case GameObjectType::RobotPortal:
        case GameObjectType::TeleportPortal:
        case GameObjectType::SpiderPortal:
        case GameObjectType::SwingPortal:
        case GameObjectType::GravityTogglePortal:
            return "portal";

        case GameObjectType::SecretCoin:
        case GameObjectType::Collectible:
        case GameObjectType::UserCoin:
            return "collectible";

        case GameObjectType::CollisionObject:
            return "collision";

        case GameObjectType::Modifier:
        case GameObjectType::Special:
            return "modifier";

        case GameObjectType::EnterEffectObject:
            return "effect";

        case GameObjectType::Decoration:
            return "decoration";
    }

    return "other";
}

bool shouldExport(GameObject* object) {
    if (!object) {
        return false;
    }

    // The first gameplay-learning dataset intentionally ignores pure decoration.
    // Everything else is retained, including modifiers/triggers, so later passes
    // can model moving platforms and gameplay-affecting editor logic.
    return object->m_objectType != GameObjectType::Decoration;
}

float levelTimeForObject(LevelEditorLayer* editorLayer, GameObject* object) {
    if (!editorLayer || !editorLayer->m_drawGridLayer || !object) {
        return 0.0f;
    }

    int order = 0;
    int channel = 0;

    if (auto* effectObject = typeinfo_cast<EffectGameObject*>(object)) {
        order = effectObject->m_ordValue;
        channel = effectObject->m_channelValue;
    }

    return editorLayer->m_drawGridLayer->timeForPos(
        object->getPosition(),
        order,
        channel,
        false,
        true,
        false,
        0
    );
}

matjson::Value groupsForObject(GameObject* object) {
    auto groups = matjson::Value::array();

    if (!object || object->m_groupCount <= 0) {
        return groups;
    }

    int count = std::max(0, static_cast<int>(object->m_groupCount));
    for (int index = 0; index < count; ++index) {
        groups.push(object->getGroupID(index));
    }

    return groups;
}
}

GameplayExportResult exportGameplayTimeline(
    LevelEditorLayer* editorLayer,
    std::filesystem::path const& outputPath
) {
    GameplayExportResult result;
    result.path = outputPath;

    if (!editorLayer || !editorLayer->m_level || !editorLayer->m_objects) {
        result.error = "The current editor does not expose a valid level/object list.";
        return result;
    }

    auto* level = editorLayer->m_level;
    auto* settings = editorLayer->m_levelSettings;

    std::vector<GameObject*> objects;
    objects.reserve(editorLayer->m_objects->count());

    for (auto* object : CCArrayExt<GameObject*>(editorLayer->m_objects)) {
        ++result.totalObjects;

        if (shouldExport(object)) {
            objects.push_back(object);
        }
    }

    std::sort(
        objects.begin(),
        objects.end(),
        [](GameObject* left, GameObject* right) {
            if (left->getPositionX() != right->getPositionX()) {
                return left->getPositionX() < right->getPositionX();
            }

            if (left->getPositionY() != right->getPositionY()) {
                return left->getPositionY() < right->getPositionY();
            }

            return left->m_uniqueID < right->m_uniqueID;
        }
    );

    auto root = matjson::Value::object();
    root["schema_version"] = 1;
    root["source"] = "gd-ai-editor-geode";

    auto levelJson = matjson::Value::object();
    levelJson["id"] = static_cast<int>(level->m_levelID);
    levelJson["name"] = std::string(level->m_levelName);
    levelJson["song_id"] = level->m_songID;
    levelJson["audio_track"] = level->m_audioTrack;
    levelJson["song_offset_seconds"] = settings ? settings->m_songOffset : 0.0f;
    levelJson["platformer"] = settings ? settings->m_platformerMode : false;
    levelJson["start_mode"] = settings ? settings->m_startMode : 0;
    levelJson["start_speed"] = settings ? static_cast<int>(settings->m_startSpeed) : 0;
    levelJson["start_mini"] = settings ? settings->m_startMini : false;
    levelJson["start_dual"] = settings ? settings->m_startDual : false;
    levelJson["start_mirror"] = settings ? settings->m_mirrorMode : false;
    levelJson["reverse_gameplay"] = settings ? settings->m_reverseGameplay : false;
    root["level"] = levelJson;

    auto summary = matjson::Value::object();
    summary["total_editor_objects"] = static_cast<int>(result.totalObjects);
    summary["exported_gameplay_objects"] = static_cast<int>(objects.size());

    auto categoryCounts = matjson::Value::object();
    std::map<std::string, int> categoryCountValues;

    auto objectArray = matjson::Value::array();

    float songOffset = settings ? settings->m_songOffset : 0.0f;

    for (auto* object : objects) {
        auto type = object->m_objectType;
        auto category = objectCategory(type);
        categoryCountValues[category] += 1;

        float x = object->getPositionX();
        float y = object->getPositionY();
        float levelTime = levelTimeForObject(editorLayer, object);
        float audioTime = levelTime + songOffset;

        auto objectJson = matjson::Value::object();
        objectJson["unique_id"] = object->m_uniqueID;
        objectJson["object_id"] = object->m_objectID;
        objectJson["object_type_id"] = static_cast<int>(type);
        objectJson["object_type"] = objectTypeName(type);
        objectJson["category"] = category;
        objectJson["x"] = x;
        objectJson["y"] = y;
        objectJson["rotation"] = object->getRotation();
        objectJson["scale_x"] = object->getScaleX();
        objectJson["scale_y"] = object->getScaleY();
        objectJson["level_time_seconds"] = levelTime;
        objectJson["audio_time_seconds"] = audioTime;
        objectJson["no_touch"] = object->m_isNoTouch;
        objectJson["passable"] = object->m_isPassable;
        objectJson["hidden"] = object->m_isHide;
        objectJson["high_detail"] = object->m_isHighDetail;
        objectJson["editor_layer"] = static_cast<int>(object->m_editorLayer);
        objectJson["editor_layer_2"] = static_cast<int>(object->m_editorLayer2);
        objectJson["groups"] = groupsForObject(object);

        auto const& hitbox = object->getObjectRect();
        auto hitboxJson = matjson::Value::object();
        hitboxJson["x"] = hitbox.origin.x;
        hitboxJson["y"] = hitbox.origin.y;
        hitboxJson["width"] = hitbox.size.width;
        hitboxJson["height"] = hitbox.size.height;
        objectJson["hitbox"] = hitboxJson;

        objectArray.push(objectJson);
    }

    for (auto const& [category, count] : categoryCountValues) {
        categoryCounts[category] = count;
    }

    summary["category_counts"] = categoryCounts;
    root["summary"] = summary;
    root["objects"] = objectArray;

    std::error_code directoryError;
    std::filesystem::create_directories(outputPath.parent_path(), directoryError);

    if (directoryError) {
        result.error = "Could not create the gameplay export directory.";
        return result;
    }

    std::ofstream output(outputPath, std::ios::out | std::ios::trunc);
    if (!output) {
        result.error = "Could not open the gameplay export file for writing.";
        return result;
    }

    output << root.dump();
    output << "\n";
    output.close();

    if (!output) {
        result.error = "Writing the gameplay export failed.";
        return result;
    }

    result.exportedObjects = objects.size();
    result.success = true;
    return result;
}
