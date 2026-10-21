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

void write_alias_uvs(const aliashdr_t* hdr, merian::PackedVertexData* vertices_dst);

// raw byte-coord space (scale on the node); vertices_dst holds hdr->numverts_vbo entries
void lerp_alias_vertices(aliashdr_t* hdr,
                         const merian::float3* baked_normals,
                         int pose1,
                         int pose2,
                         float blend,
                         merian::PackedVertexData* vertices_dst);

// false if psprite->type is unsupported
bool sprite_world_basis(entity_t* ent,
                        msprite_t* psprite,
                        merian::float3& s_up,
                        merian::float3& s_right);

constexpr uint32_t PARTICLE_ATLAS_SIZE = 256;

std::vector<uint32_t> particle_atlas(const uint32_t* palette);

constexpr uint32_t PARTICLE_PLANES = 3;
constexpr uint32_t PARTICLE_SLOT_VERTICES = 4 * PARTICLE_PLANES;
constexpr uint32_t PARTICLE_SLOT_CHUNK = 1024;

// slots in chunks of PARTICLE_SLOT_CHUNK; the triangles of free slots are inactive
void init_particle_geo(std::vector<merian::PackedVertexData>& vertices,
                       std::vector<merian::uint3>& indices);

// deactivates the previous `active_slots`, then lists the slots written; true if they grew
bool extract_particle_geo(std::vector<merian::PackedVertexData>& vertices,
                          std::vector<merian::uint3>& indices,
                          std::vector<uint32_t>& active_slots,
                          float size);

} // namespace merian_quake
