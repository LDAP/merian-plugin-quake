// Quake material model: registered with merian::MaterialSystem from QuakeScene.
// Payload layout mirrors QuakeMaterialPayload in src/game/quake_material.hpp.

import merian_shaders.shading.materials.material;
import merian_shaders.shading.bsdfs.brdf_ggx;
import merian_shaders.shading.bsdfs.brdf_lambert_diffuse;
import merian_shaders.shading.bsdfs.bsdf_fresnel_mix;
import merian_shaders.shading.bsdfs.bsdf_tint;
import merian_shaders.utils.normal_map;

#include "shader/quake-material.slangh"

namespace merian {

// this took way to long to get right...
float3 ldr_to_hdr(float3 color) {
    const float l = clamp(pow((color.r + color.g + color.b) / 3, .1), 0, 0.99);
    return sqrt(color) * 2 * (l / (1.0 - l));
}

float2 quake_warp(const float2 st, const float time) {
    return 2 * ((st) + 0.125 * sin(float2(3 * (st).yx + 0.5 * (time))));
}

float2 quake_waves(const float2 st, const float time) {
    return (-float2(.1, 0) * cos((st).x * 5 + (time) * 2) *
                pow(max(sin((st).x * 5 + (time) * 2), 0), 5) -
            float2(.07, 0) * cos(-(st).x * 5 + -(st).y * 3 + (time) * 3) *
                pow(max(sin(-(st).x * 5 + -(st).y * 3 + (time) * 3), 0), 5));
}

// albedo tints both lobes (matches the old bsdf_ggx_diffuse_mix; keeps demodulation exact)
typealias QuakeBSDF = TintBSDF<FresnelMixBSDF<GGXBRDF, LambertDiffuseBRDF>>;

struct QuakeMaterial : MaterialModel {
    typealias MaterialSampleImpl = QuakeMaterialSample;

    MaterialHeader header;

    // albedo texture lives in header.alpha_texture_id;
    TextureID fullbright_tex;
    TextureID normal_tex;
    TextureID gloss_tex;
    uint16_t flags;     // MAT_TYPE_*
    uint8_t alpha_mode; // 0: use texture alpha; [1,15] map to [0,1] opaque
    uint8_t _pad;

    uint16_t get_flags() {
        return flags;
    }

    // texture coordinates after the per-surface warp; roughness follows the surface type
    float2 get_uv(const ShadingData sd, out float roughness) {
        roughness = 0.6f;
        float2 uv = sd.uv;
        if ((flags & MAT_TYPE_WARP) != 0u) {
            uv = quake_warp(uv, sd.time);
            if (flags == MAT_TYPE_WATER) {
                uv += quake_waves(uv, sd.time);
                roughness = 0.4f;
            }
        }
        return uv;
    }

    float3 get_albedo(const float2 uv, const TextureManager tm, const LODSampler2D lod) {
        float3 albedo = float3(1, 0, 1);
        if (let albedo_tex_id = header.get_alpha_texture_id()) {
            albedo = lod.sample(tm.get_sampler2D(albedo_tex_id), uv).rgb;
            // desaturate for a better look
            albedo = pow(albedo, 1. / 1.2);
        }
        return albedo;
    }

    float3 get_emission(const float2 uv,
                        const float3 albedo,
                        const TextureManager tm,
                        const LODSampler2D lod) {
        if (get_flags() == MAT_TYPE_WATERFALL) {
            return albedo;
        }
        if (fullbright_tex != TextureID(-1)) {
            return ldr_to_hdr(lod.sample(tm.get_sampler2D(fullbright_tex), uv).rgb);
        }
        return float3(0);
    }

    override float3 get_emission(const ShadingData sd,
                                 const TextureManager tm,
                                 const LODSampler2D lod) {
        float roughness;
        const float2 uv = get_uv(sd, roughness);
        const bool needs_albedo = get_flags() == MAT_TYPE_WATERFALL;
        return get_emission(uv, needs_albedo ? get_albedo(uv, tm, lod) : float3(0), tm, lod);
    }

    QuakeMaterialSample get_sample(const ShadingData sd,
                                   const TextureManager tm,
                                   const LODSampler2D lod,
                                   out float3 out_emission,
                                   out OrthonormalFrame out_shading_frame) {
        // Normal (isotropic BSDF: only the normal matters, the frame is rebuilt on use)
        if (normal_tex != TextureID(-1)) {
            const float3 n = (tm.get_sampler2D(normal_tex).SampleLevel(sd.uv, 0).rgb - 0.5) * 2;
            const float3 mapped_n = sd.frame.apply_normal_map(n).get_normal();
            out_shading_frame = OrthonormalFrame(
                merian_hint_clamp_normals ? keller_clamp_normal(mapped_n, sd.face_normal, sd.wi)
                                          : mapped_n);
        } else {
            out_shading_frame = sd.frame;
        }

        float roughness;
        const float2 uv = get_uv(sd, roughness);
        const float3 albedo = get_albedo(uv, tm, lod);
        out_emission = get_emission(uv, albedo, tm, lod);

        if (gloss_tex != TextureID(-1)) {
            roughness = tm.get_sampler2D(gloss_tex).SampleLevel(uv, 0).r;
        }

        QuakeMaterialSample s;
        s.alpha = half(ggx_roughness_to_alpha(max(roughness, merian_hint_min_roughness)));
        s.tint = half3(albedo);
        return s;
    }
}

struct QuakeMaterialSample : MaterialSample {
    typealias BSDFImpl = QuakeBSDF;

    // fp16 storage (evaluation stays fp32 through get_bsdf); halves the sample's live registers
    half3 tint;
    half alpha; // GGX alpha

    BSDFImpl get_bsdf() {
        return QuakeBSDF(FresnelMixBSDF<GGXBRDF, LambertDiffuseBRDF>(
                             GGXBRDF(float(alpha)), LambertDiffuseBRDF(float3(1.0f)), 0.02f),
                         float3(tint));
    }
}

} // namespace merian
