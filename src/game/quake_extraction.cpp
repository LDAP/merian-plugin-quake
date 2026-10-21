#include "quake_extraction.hpp"

#include "merian/utils/normal_encoding.hpp"
#include "merian/utils/vector_matrix.hpp"
#include "merian/utils/xorshift.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>

extern "C" {
#include "quakedef.h"

extern particle_t *active_particles, *particles;
extern cvar_t scr_fov, cl_gun_fovscale;
}

namespace merian_quake {

std::vector<merian::float3> bake_alias_pose_normals(aliashdr_t* hdr) {
    const auto* indexes = (const int16_t*)((uint8_t*)hdr + hdr->indexes);
    const auto* desc = (const aliasmesh_t*)((uint8_t*)hdr + hdr->meshdesc);
    const auto* trivertexes = (const trivertx_t*)((uint8_t*)hdr + hdr->vertexes);
    const uint32_t numverts = static_cast<uint32_t>(hdr->numverts);
    const uint32_t numposes = static_cast<uint32_t>(hdr->numposes);
    const uint32_t prim_count = static_cast<uint32_t>(hdr->numindexes / 3);

    // Quake winds CW so the outward face normal is cross(v2-v0, v1-v0). Positions stay
    // in byte-coord space — the per-axis scale cancels in the shader's inv-transpose.
    std::vector<merian::float3> normals(static_cast<size_t>(numposes) * numverts,
                                        merian::float3(0.f));
    for (uint32_t pose = 0; pose < numposes; pose++) {
        merian::float3* pose_normals = normals.data() + pose * numverts;
        const trivertx_t* pose_verts = trivertexes + pose * numverts;
        for (uint32_t t = 0; t < prim_count; t++) {
            const int vi0 = desc[indexes[t * 3 + 0]].vertindex;
            const int vi1 = desc[indexes[t * 3 + 1]].vertindex;
            const int vi2 = desc[indexes[t * 3 + 2]].vertindex;
            const merian::float3 p0(pose_verts[vi0].v[0], pose_verts[vi0].v[1],
                                    pose_verts[vi0].v[2]);
            const merian::float3 p1(pose_verts[vi1].v[0], pose_verts[vi1].v[1],
                                    pose_verts[vi1].v[2]);
            const merian::float3 p2(pose_verts[vi2].v[0], pose_verts[vi2].v[1],
                                    pose_verts[vi2].v[2]);
            const merian::float3 face_n = merian::cross(p2 - p0, p1 - p0);
            pose_normals[vi0] += face_n;
            pose_normals[vi1] += face_n;
            pose_normals[vi2] += face_n;
        }
        for (uint32_t v = 0; v < numverts; v++) {
            const float len2 = merian::dot(pose_normals[v], pose_normals[v]);
            pose_normals[v] =
                (len2 > 0.f) ? pose_normals[v] / std::sqrt(len2) : merian::float3(0.f, 0.f, 1.f);
        }
    }
    return normals;
}

bool sprite_world_basis(entity_t* ent,
                        msprite_t* psprite,
                        merian::float3& s_up,
                        merian::float3& s_right) {
    if (psprite == nullptr)
        return false;

    merian::float3 vpn, vright, vup, r_origin;
    VectorCopy(r_refdef.vieworg, r_origin);
    AngleVectors(r_refdef.viewangles, &vpn.x, &vright.x, &vup.x);

    merian::float3 v_forward;
    merian::float3 v_right;
    merian::float3 v_up;

    switch (psprite->type) {
    case SPR_VP_PARALLEL_UPRIGHT:
        v_up = merian::float3(0, 0, 1);
        v_right = merian::normalize(merian::cross(vpn, v_up));
        s_up = v_up;
        s_right = v_right;
        break;
    case SPR_FACING_UPRIGHT:
        VectorSubtract(ent->origin, &r_origin.x, &v_forward.x);
        v_forward.z = 0;
        VectorNormalizeFast(&v_forward.x);
        v_right = merian::float3(v_forward.y, -v_forward.x, 0);
        v_up = merian::float3(0, 0, 1);
        s_up = v_up;
        s_right = v_right;
        break;
    case SPR_VP_PARALLEL:
        s_up = vup;
        s_right = vright;
        break;
    case SPR_ORIENTED:
        AngleVectors(ent->angles, &v_forward.x, &v_right.x, &v_up.x);
        s_up = v_up;
        s_right = v_right;
        break;
    case SPR_VP_PARALLEL_ORIENTED: {
        const float angle = ent->angles[ROLL] * M_PI_DIV_180;
        const float sr = std::sin(angle);
        const float cr = std::cos(angle);
        v_right = (vright * cr) + (vup * sr);
        v_up = (vright * -sr) + (vup * cr);
        s_up = v_up;
        s_right = v_right;
        break;
    }
    default:
        return false;
    }
    s_up = merian::normalize(s_up);
    s_right = merian::normalize(s_right);
    return true;
}

namespace {

constexpr uint32_t PARTICLE_ATLAS_TILES = 16;
constexpr uint32_t PARTICLE_TILE_SIZE = PARTICLE_ATLAS_SIZE / PARTICLE_ATLAS_TILES;
constexpr float PARTICLE_TILE_RADIUS = 0.5f * PARTICLE_TILE_SIZE - 0.5f;
constexpr float QUAKESPASM_PARTICLE_RADIUS = 0.25f * 1.27f;

merian::float2 particle_uv(const uint32_t index, const merian::float2 disc) {
    const merian::float2 tile(static_cast<float>(index % PARTICLE_ATLAS_TILES),
                              static_cast<float>(index / PARTICLE_ATLAS_TILES));
    return (tile * static_cast<float>(PARTICLE_TILE_SIZE) + 0.5f * PARTICLE_TILE_SIZE +
            disc * PARTICLE_TILE_RADIUS) /
           static_cast<float>(PARTICLE_ATLAS_SIZE);
}

// x = NaN makes the triangles inactive (Vulkan spec, "Inactive Primitives and Instances")
constexpr merian::PackedVertexData INACTIVE_VERTEX = {
    {std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f}, 0, {}, 0};

} // namespace

std::vector<uint32_t> particle_atlas(const uint32_t* palette) {
    std::vector<uint32_t> atlas(static_cast<size_t>(PARTICLE_ATLAS_SIZE) * PARTICLE_ATLAS_SIZE);
    for (uint32_t y = 0; y < PARTICLE_ATLAS_SIZE; y++) {
        for (uint32_t x = 0; x < PARTICLE_ATLAS_SIZE; x++) {
            const uint32_t index =
                (y / PARTICLE_TILE_SIZE) * PARTICLE_ATLAS_TILES + x / PARTICLE_TILE_SIZE;
            const merian::float2 disc =
                (merian::float2(static_cast<float>(x % PARTICLE_TILE_SIZE),
                                static_cast<float>(y % PARTICLE_TILE_SIZE)) +
                 0.5f - 0.5f * PARTICLE_TILE_SIZE) /
                PARTICLE_TILE_RADIUS;
            const float r2 = merian::dot(disc, disc);
            const float alpha = r2 < 1.f ? 0.5f + 0.5f * (1.f - r2) * (1.f - r2) : 0.f;
            atlas[y * PARTICLE_ATLAS_SIZE + x] =
                (palette[index] & 0x00ffffffu) | static_cast<uint32_t>(std::lround(alpha * 255))
                                                     << 24u;
        }
    }
    return atlas;
}

namespace {

void grow_particle_slots(std::vector<merian::PackedVertexData>& vertices,
                         std::vector<merian::uint3>& indices,
                         const size_t slot_count) {
    const size_t first_plane = indices.size() / 2;
    vertices.resize(slot_count * PARTICLE_SLOT_VERTICES, INACTIVE_VERTEX);
    indices.resize(slot_count * PARTICLE_PLANES * 2);
    for (size_t plane = first_plane; plane < slot_count * PARTICLE_PLANES; plane++) {
        const uint32_t base = static_cast<uint32_t>(plane * 4);
        indices[2 * plane] = merian::uint3(base, base + 1, base + 2);
        indices[2 * plane + 1] = merian::uint3(base, base + 2, base + 3);
    }
}

} // namespace

void init_particle_geo(std::vector<merian::PackedVertexData>& vertices,
                       std::vector<merian::uint3>& indices) {
    vertices.clear();
    indices.clear();
    grow_particle_slots(vertices, indices, PARTICLE_SLOT_CHUNK);
}

bool extract_particle_geo(std::vector<merian::PackedVertexData>& vertices,
                          std::vector<merian::uint3>& indices,
                          std::vector<uint32_t>& active_slots,
                          const float size) {
    static constexpr merian::float2 corners[4] = {
        {-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f}, {-1.f, 1.f}};
    static const std::array<std::array<merian::half2, 4>, 256> corner_uvs = [] {
        std::array<std::array<merian::half2, 4>, 256> uvs;
        for (uint32_t color = 0; color < uvs.size(); color++)
            for (uint32_t corner = 0; corner < 4; corner++)
                uvs[color][corner] = merian::half2(particle_uv(color, corners[corner]));
        return uvs;
    }();
    for (const uint32_t slot : active_slots)
        std::fill_n(vertices.data() + static_cast<size_t>(slot) * PARTICLE_SLOT_VERTICES,
                    PARTICLE_SLOT_VERTICES, INACTIVE_VERTEX);
    active_slots.clear();
    size_t slot_end = 0;
    for (const particle_t* p = active_particles; p != nullptr; p = p->next) {
        const auto slot = static_cast<uint32_t>(p - particles);
        active_slots.push_back(slot);
        slot_end = std::max(slot_end, static_cast<size_t>(slot) + 1);
    }
    const bool grown = slot_end * PARTICLE_SLOT_VERTICES > vertices.size();
    if (grown)
        grow_particle_slots(vertices, indices,
                            (slot_end + PARTICLE_SLOT_CHUNK - 1) / PARTICLE_SLOT_CHUNK *
                                PARTICLE_SLOT_CHUNK);

    vec3_t vpn, vright, vup;
    AngleVectors(r_refdef.viewangles, vpn, vright, vup);
    const merian::float3 eye = merian::as_float3(r_refdef.vieworg);
    const merian::float3 view_dir = merian::as_float3(vpn);

    for (const uint32_t slot : active_slots) {
        const particle_t* p = particles + slot;
        merian::XORShift32 xrand{(slot + 1) * 2654435761u};
        const float jitter = 2.f * (xrand.next_float() - 0.5f) + 2.f * (xrand.next_float() - 0.5f);
        const merian::float3 center = merian::as_float3(p->org) + jitter;

        const float depth = merian::dot(center - eye, view_dir);
        const float radius =
            size * QUAKESPASM_PARTICLE_RADIUS * (depth < 20.f ? 1.08f : 1.f + depth * 0.004f);

        const merian::float3 to_eye = eye - center;
        const float eye_distance = merian::length(to_eye);
        const merian::float3 facing = eye_distance > 1e-3f ? to_eye / eye_distance : -view_dir;
        const merian::float3 right = merian::normalize(
            merian::cross(std::abs(facing.z) < 0.999f ? merian::float3(0.f, 0.f, 1.f)
                                                      : merian::float3(1.f, 0.f, 0.f),
                          facing));
        const merian::float3 up = merian::cross(facing, right);
        const merian::float3 planes[PARTICLE_PLANES][3] = {
            {right, up, facing}, {facing, up, -right}, {facing, right, up}};

        const auto& uvs = corner_uvs[static_cast<uint32_t>(static_cast<int>(p->color) & 0xff)];
        merian::PackedVertexData* vertex = vertices.data() + slot * PARTICLE_SLOT_VERTICES;
        for (const auto& [u, v, normal] : planes) {
            const uint32_t encoded_normal = merian::encode_normal(normal);
            const merian::float3 radius_u = radius * u;
            const merian::float3 radius_v = radius * v;
            for (uint32_t corner = 0; corner < 4; corner++) {
                const merian::float3 offset =
                    corners[corner].x * radius_u + corners[corner].y * radius_v;
                vertex->position = center + offset;
                vertex->encoded_normal = encoded_normal;
                vertex->uv = uvs[corner];
                vertex->encoded_tangent = 0;
                vertex++;
            }
        }
    }
    return grown;
}

void write_alias_uvs(const aliashdr_t* hdr, merian::PackedVertexData* vertices_dst) {
    const auto* desc =
        reinterpret_cast<const aliasmesh_t*>(reinterpret_cast<const uint8_t*>(hdr) + hdr->meshdesc);
    const float skin_w = static_cast<float>(hdr->skinwidth);
    const float skin_h = static_cast<float>(hdr->skinheight);
    for (int v = 0; v < hdr->numverts_vbo; v++) {
        vertices_dst[v].uv =
            merian::half2((desc[v].st[0] + 0.5f) / skin_w, (desc[v].st[1] + 0.5f) / skin_h);
    }
}

void lerp_alias_vertices(aliashdr_t* hdr,
                         const merian::float3* baked_normals,
                         const int pose1,
                         const int pose2,
                         const float blend,
                         merian::PackedVertexData* vertices_dst) {
    const auto* desc = (aliasmesh_t*)((uint8_t*)hdr + hdr->meshdesc);
    const auto* trivertexes = (trivertx_t*)((uint8_t*)hdr + hdr->vertexes);

    const merian::float3* normals_pose1 = baked_normals + hdr->numverts * pose1;
    const merian::float3* normals_pose2 = baked_normals + hdr->numverts * pose2;

    for (int v = 0; v < hdr->numverts_vbo; v++) {
        const int vi = desc[v].vertindex;

        const auto& tv1 = trivertexes[hdr->numverts * pose1 + vi];
        const auto& tv2 = trivertexes[hdr->numverts * pose2 + vi];
        const merian::float3 p1{float(tv1.v[0]), float(tv1.v[1]), float(tv1.v[2])};
        const merian::float3 p2{float(tv2.v[0]), float(tv2.v[1]), float(tv2.v[2])};

        vertices_dst[v].position = merian::lerp(p1, p2, blend);
        vertices_dst[v].encoded_normal =
            merian::encode_normal(merian::lerp(normals_pose1[vi], normals_pose2[vi], blend));
    }
}

} // namespace merian_quake
