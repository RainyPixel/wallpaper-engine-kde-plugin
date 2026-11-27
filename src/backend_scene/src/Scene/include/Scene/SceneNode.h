#pragma once
#include <list>
#include <vector>
#include <memory>
#include <Eigen/Dense>
#include "SceneMesh.h"
#include "SceneCamera.h"

#include "Core/Literals.hpp"
#include "Core/NoCopyMove.hpp"

namespace wallpaper
{

class SceneNode : NoCopy, NoMove {
public:
    SceneNode()
        : m_name(),
          m_dirty(true),
          m_translate(Eigen::Vector3f::Zero()),
          m_scale { 1.0f, 1.0f, 1.0f },
          m_rotation(Eigen::Vector3f::Zero()) {}
    SceneNode(const Eigen::Vector3f& translate, const Eigen::Vector3f& scale,
              const Eigen::Vector3f& rotation, const std::string& name = "")
        : m_name(name),
          m_dirty(true),
          m_translate(translate),
          m_scale(scale),
          m_rotation(rotation) {};

    const auto& Camera() const { return m_cameraName; }
    void        SetCamera(const std::string& name) { m_cameraName = name; }
    void        AddMesh(std::shared_ptr<SceneMesh> mesh) { m_mesh = mesh; }
    void        AppendChild(std::shared_ptr<SceneNode> sub) {
        sub->SetTransformParent(this);
        m_children.push_back(std::move(sub));
    }
    // The transform parent can differ from the draw tree parent. Set it during scene construction.
    void SetTransformParent(SceneNode* parent) {
        const bool was_animated = AnimatedTransform();
        m_parent                = parent;
        m_animatedAncestor      = parent && parent->AnimatedTransform();
        m_dirty                 = true;
        if (was_animated != AnimatedTransform()) {
            for (auto& child : m_children) {
                if (child->m_parent == this) child->SetTransformParent(this);
            }
        }
    }
    Eigen::Matrix4d GetLocalTrans() const;

    const auto& Translate() const { return m_translate; }
    const auto& Rotation() const { return m_rotation; }
    void        SetRotation(Eigen::Vector3f v) { m_rotation = v; }
    void        SetTranslate(Eigen::Vector3f v) { m_translate = v; }
    void        SetAnimatedScale(float value) {
        m_scale.setConstant(value);
        m_animatedScale = true;
    }
    bool AnimatedTransform() const { return m_animatedScale || m_animatedAncestor; }

    // update self modle trans (will update parent before)
    void            UpdateTrans();
    Eigen::Matrix4d ModelTrans() const { return m_trans; };

    SceneMesh* Mesh() { return m_mesh.get(); }
    bool       HasMaterial() const { return m_mesh && m_mesh->Material() != nullptr; };

    const auto& GetChildren() const { return m_children; }
    auto&       GetChildren() { return m_children; }

    i32& ID() { return m_id; }

    bool IsOffscreen() const { return m_offscreen; }
    void SetOffscreen(bool v) { m_offscreen = v; }

private:
    // mark self and all children
    void MarkTransDirty();

    i32         m_id { 0 };
    bool        m_offscreen { false };
    std::string m_name;

    bool            m_dirty;
    bool            m_animatedScale { false };
    bool            m_animatedAncestor { false };
    Eigen::Matrix4d m_trans;

    Eigen::Vector3f m_translate { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f m_scale { 1.0f, 1.0f, 1.0f };
    Eigen::Vector3f m_rotation { 0.0f, 0.0f, 0.0f };

    std::shared_ptr<SceneMesh> m_mesh;

    // specific a camera not active, used for image effect
    std::string m_cameraName;

    SceneNode* m_parent { nullptr };

    std::list<std::shared_ptr<SceneNode>> m_children;
};
} // namespace wallpaper
