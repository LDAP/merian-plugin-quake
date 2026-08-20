#pragma once

#include "merian-shaders/scene/scene-data.slangh"
#include "merian/utils/vector_matrix.hpp"

#include <vector>

extern "C" {
#include "quakedef.h"
}

namespace merian_quake {

// Smooth per-pose normals indexed by (pose * hdr->numverts + vertindex).
std::vector<merian::float3> bake_alias_pose_normals(aliashdr_t* hdr);

// Lerp alias vertices in raw byte-coord space; scale/scale_origin live on the
// SceneNode transform. Output buffers must hold hdr->numverts_vbo entries.
void lerp_alias_vertices(aliashdr_t* hdr,
                         const merian::float3* baked_normals,
                         int pose1,
                         int pose2,
                         float blend,
                         int prev_pose1,
                         int prev_pose2,
                         float prev_blend,
                         merian::PackedVertexData* vertices_dst,
                         merian::PackedPrevVertexData* prev_dst);

// World-space (up, right) basis for a sprite. Returns false if psprite->type
// is unsupported.
bool sprite_world_basis(entity_t* ent,
                        msprite_t* psprite,
                        merian::float3& s_up,
                        merian::float3& s_right);

// Particle billboards, one tetrahedron per slot. A particle keeps its slot while it lives, so
// its primitive ids stay consistent across frames; slots of dead particles collapse to zero-area
// triangles until reused. `no_random` makes the per-particle jitter reproducible.
class ParticleSlots {
  public:
    void extract(std::vector<merian::PackedVertexData>& vertices,
                 std::vector<merian::float3>& prev_positions,
                 std::vector<merian::uint3>& indices,
                 bool no_random,
                 double prev_cl_time);

    void clear();

  private:
    static constexpr uint32_t SLOT_NONE = UINT32_MAX;

    struct Slot {
        uint32_t particle_index = SLOT_NONE; // index into Quake's particle array
        uint32_t last_frame = 0;
        merian::float3 last_pos{0.f};
    };
    std::vector<Slot> slots;
    std::vector<uint32_t> slot_of_particle; // indexed by particle array index
    std::vector<uint32_t> free_slots;
    uint32_t frame = 0;
};

} // namespace merian_quake
