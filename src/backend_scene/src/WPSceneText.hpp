#pragma once

#include <functional>
#include <memory>

#include <nlohmann/json_fwd.hpp>

namespace wallpaper
{
class SceneNode;
class SceneShader;
class WPTexImageParser;
struct WPTextScript;

namespace fs
{
class VFS;
}

std::function<void()> AddSceneText(SceneNode&, const nlohmann::json&, WPTextScript, fs::VFS&,
                                   WPTexImageParser&, std::shared_ptr<SceneShader>&);
} // namespace wallpaper
