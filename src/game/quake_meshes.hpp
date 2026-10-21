#pragma once

#include "merian-shaders/scene/scene-data.slangh"
#include "merian-shaders/scene/scene.hpp"

#include <vector>

namespace merian_quake {

// Static brush geometry in world space, HostPacked.
class QuakeBrushMesh : public merian::Scene::Mesh {
  public:
    std::vector<merian::PackedVertexData> vertices;
    std::vector<merian::uint3> indices;

    uint32_t get_vertex_count() const override {
        return static_cast<uint32_t>(vertices.size());
    }
    uint32_t get_primitive_count() const override {
        return static_cast<uint32_t>(indices.size());
    }

    MeshVertexData get_vertices() const override {
        return HostPacked<merian::PackedVertexData>{vertices.data()};
    }
    MeshIndexData get_indices() const override {
        return HostPacked<void>{indices.data()};
    }
};

// Shared sprite quad (6 sequential verts, no index buffer); motion comes from the node.
class QuakeSpriteFrameMesh : public merian::Scene::Mesh {
  public:
    std::vector<merian::PackedVertexData> vertices;

    QuakeSpriteFrameMesh() {
        index_type = vk::IndexType::eNoneKHR;
    }

    uint32_t get_vertex_count() const override {
        return static_cast<uint32_t>(vertices.size());
    }
    uint32_t get_primitive_count() const override {
        return static_cast<uint32_t>(vertices.size()) / 3;
    }

    MeshVertexData get_vertices() const override {
        return HostPacked<merian::PackedVertexData>{vertices.data()};
    }
    MeshIndexData get_indices() const override {
        return std::monostate{};
    }
};

class AliasInstanceMesh : public merian::Scene::Mesh {
  public:
    std::vector<merian::PackedVertexData> vertices;
    merian::BufferHandle ib_shared; // borrowed from AliasModelInfo
    uint32_t vertex_count = 0;
    uint32_t primitive_count = 0;

    AliasInstanceMesh() {
        index_type = vk::IndexType::eUint16; // mdl indices are int16
    }

    uint32_t get_vertex_count() const override {
        return vertex_count;
    }
    uint32_t get_primitive_count() const override {
        return primitive_count;
    }

    MeshVertexData get_vertices() const override {
        return HostPacked<merian::PackedVertexData>{vertices.data()};
    }
    MeshIndexData get_indices() const override {
        return DeviceLocal{ib_shared};
    }
};

// Per (brush entity, material partition); device-local geometry built once, transform on the node.
class BrushEntityMesh : public merian::Scene::Mesh {
  public:
    merian::BufferHandle vb;
    merian::BufferHandle ib;
    uint32_t vertex_count = 0;
    uint32_t primitive_count = 0;

    uint32_t get_vertex_count() const override {
        return vertex_count;
    }
    uint32_t get_primitive_count() const override {
        return primitive_count;
    }

    MeshVertexData get_vertices() const override {
        return DeviceLocal{vb};
    }
    MeshIndexData get_indices() const override {
        return DeviceLocal{ib};
    }
};

} // namespace merian_quake
