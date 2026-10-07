#include "quake_extraction.hpp"

#include "merian/utils/normal_encoding.hpp"
#include "merian/utils/vector_matrix.hpp"
#include "merian/utils/xorshift.hpp"

#include <cmath>

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

void extract_particle_geo(std::vector<merian::PackedVertexData>& vertices,
                          std::vector<merian::float3>& prev_positions,
                          std::vector<merian::uint3>& indices,
                          const float size) {
    static const merian::float2 corners[4] = {{-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f}, {-1.f, 1.f}};

    vec3_t vpn, vright, vup;
    AngleVectors(r_refdef.viewangles, vpn, vright, vup);
    const merian::float3 eye = merian::as_float3(r_refdef.vieworg);

    for (particle_t* p = active_particles; p != nullptr; p = p->next) {
        merian::XORShift32 xrand{static_cast<uint32_t>(p - particles + 1) * 2654435761u};
        const float jitter = 2.f * (xrand.next_float() - 0.5f) + 2.f * (xrand.next_float() - 0.5f);
        const merian::float3 center = merian::as_float3(p->org) + jitter;
        const merian::float3 prev_center =
            (p->mv_prev_valid ? merian::as_float3(p->mv_prev_origin) : merian::as_float3(p->org)) +
            jitter;
        VectorCopy(p->org, p->mv_prev_origin);
        p->mv_prev_valid = true;

        const float depth = merian::dot(center - eye, merian::as_float3(vpn));
        const float radius =
            size * QUAKESPASM_PARTICLE_RADIUS * (depth < 20.f ? 1.08f : 1.f + depth * 0.004f);

        const merian::float3 to_eye = eye - center;
        const merian::float3 facing =
            merian::length(to_eye) > 1e-3f ? merian::normalize(to_eye) : -merian::as_float3(vpn);
        const merian::float3 right = merian::normalize(
            merian::cross(std::abs(facing.z) < 0.999f ? merian::float3(0.f, 0.f, 1.f)
                                                      : merian::float3(1.f, 0.f, 0.f),
                          facing));
        const merian::float3 up = merian::cross(facing, right);
        const merian::float3 planes[3][2] = {{right, up}, {facing, up}, {facing, right}};

        const uint32_t color = static_cast<uint32_t>(static_cast<int>(p->color) & 0xff);
        for (const auto& [u, v] : planes) {
            const uint32_t encoded_normal = merian::encode_normal(merian::cross(u, v));
            const uint32_t base = static_cast<uint32_t>(vertices.size());
            for (const merian::float2& corner : corners) {
                const merian::float3 offset = radius * (corner.x * u + corner.y * v);
                merian::PackedVertexData pv{};
                pv.position = center + offset;
                pv.uv = merian::half2(particle_uv(color, corner));
                pv.encoded_normal = encoded_normal;
                vertices.push_back(pv);
                prev_positions.push_back(prev_center + offset);
            }
            indices.push_back(merian::uint3(base + 0, base + 1, base + 2));
            indices.push_back(merian::uint3(base + 0, base + 2, base + 3));
        }
    }
}

void lerp_alias_vertices(aliashdr_t* hdr,
                         const merian::float3* baked_normals,
                         const int pose1,
                         const int pose2,
                         const float blend,
                         const int prev_pose1,
                         const int prev_pose2,
                         const float prev_blend,
                         merian::PackedVertexData* vertices_dst,
                         merian::PackedPrevVertexData* prev_dst) {
    const auto* desc = (aliasmesh_t*)((uint8_t*)hdr + hdr->meshdesc);
    const auto* trivertexes = (trivertx_t*)((uint8_t*)hdr + hdr->vertexes);

    const float skin_w = static_cast<float>(hdr->skinwidth);
    const float skin_h = static_cast<float>(hdr->skinheight);
    const merian::float3* normals_pose1 = baked_normals + hdr->numverts * pose1;
    const merian::float3* normals_pose2 = baked_normals + hdr->numverts * pose2;

    for (int v = 0; v < hdr->numverts_vbo; v++) {
        const int vi = desc[v].vertindex;

        const auto& tv1 = trivertexes[hdr->numverts * pose1 + vi];
        const auto& tv2 = trivertexes[hdr->numverts * pose2 + vi];
        const merian::float3 p1{float(tv1.v[0]), float(tv1.v[1]), float(tv1.v[2])};
        const merian::float3 p2{float(tv2.v[0]), float(tv2.v[1]), float(tv2.v[2])};

        vertices_dst[v].position = merian::lerp(p1, p2, blend);
        vertices_dst[v].encoded_normal = merian::encode_normal(
            merian::normalize(merian::lerp(normals_pose1[vi], normals_pose2[vi], blend)));
        vertices_dst[v].uv =
            merian::half2((desc[v].st[0] + 0.5f) / skin_w, (desc[v].st[1] + 0.5f) / skin_h);
        vertices_dst[v].encoded_tangent = 0;

        const auto& ptv1 = trivertexes[hdr->numverts * prev_pose1 + vi];
        const auto& ptv2 = trivertexes[hdr->numverts * prev_pose2 + vi];
        const merian::float3 pp1{float(ptv1.v[0]), float(ptv1.v[1]), float(ptv1.v[2])};
        const merian::float3 pp2{float(ptv2.v[0]), float(ptv2.v[1]), float(ptv2.v[2])};
        prev_dst[v].position = merian::lerp(pp1, pp2, prev_blend);
    }
}

} // namespace merian_quake
