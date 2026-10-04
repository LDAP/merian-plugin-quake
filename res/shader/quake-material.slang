// Quake material model: registered with merian::MaterialSystem from QuakeScene.
// Payload layout mirrors QuakeMaterialPayload in src/game/quake_material.hpp.

import merian_shaders.shading.materials.material;
import merian_shaders.shading.bsdfs.brdf_ggx;
import merian_shaders.shading.bsdfs.bsdf_diffuse_transmission;
import merian_shaders.shading.bsdfs.bsdf_fresnel_mix;
import merian_shaders.shading.bsdfs.bsdf_mix;
import merian_shaders.shading.bsdfs.bsdf_thin_dielectric;
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
typealias QuakeSurfaceBSDF = TintBSDF<FresnelMixBSDF<GGXBRDF, DiffuseTransmissionBSDF>>;

typealias QuakeBSDF = MixBSDF<ThinDielectricBSDF, QuakeSurfaceBSDF>;

extern static const bool quake_enable_transparency = false;

static const float QUAKE_GLASS_IOR = 1.5f;
static const float QUAKE_LIQUID_IOR = 1.33f;
static const float QUAKE_WATERFALL_EMISSION_GAIN = 4.f;

static const float QUAKE_BASE_ROUGHNESS = 0.5f;
static const float QUAKE_BASE_F0 = 0.2f;

struct QuakeMaterial : MaterialModel {
    typealias MaterialSampleImpl = QuakeMaterialSample;

    MaterialHeader header;

    // albedo texture lives in header.alpha_texture_id;
    TextureID fullbright_tex;
    TextureID normal_tex;
    TextureID gloss_tex;
    uint16_t flags; // MAT_TYPE_*
    uint8_t pane;
    uint8_t opacity;

    uint16_t get_flags() {
        return flags;
    }

    bool is_translucent() {
        return quake_enable_transparency && opacity < 255;
    }

    float get_pane_ior() {
        switch (pane) {
        case PANE_GLASS:
            return QUAKE_GLASS_IOR;
        case PANE_LIQUID:
            return QUAKE_LIQUID_IOR;
        default:
            return 1.f;
        }
    }

    float get_coverage(const ShadingData sd) {
        if (!is_translucent()) {
            return 1.f;
        }
        return dot(sd.wi, sd.face_normal) < 0.f ? 0.f : float(opacity) / 255.f;
    }

    float get_emission_scale(const ShadingData sd) {
        const float coverage = get_coverage(sd);
        return get_flags() == MAT_TYPE_WATERFALL
                   ? min(1.f, coverage * QUAKE_WATERFALL_EMISSION_GAIN)
                   : coverage;
    }

    override float3 get_emission(const ShadingData sd,
                                 const TextureManager tm,
                                 const LODSampler2D lod) {
        float2 uv = sd.uv;
        if ((flags & MAT_TYPE_WARP) != 0u) {
            uv = quake_warp(uv, sd.time);
            if (flags == MAT_TYPE_WATER) {
                uv += quake_waves(uv, sd.time);
            }
        }

        const float emission_scale = get_emission_scale(sd);
        if (get_flags() == MAT_TYPE_WATERFALL) {
            if (let albedo_tex_id = header.get_alpha_texture_id()) {
                return emission_scale *
                       pow(lod.sample(tm.get_sampler2D(albedo_tex_id), uv).rgb, 1. / 1.2);
            }
            return emission_scale * float3(1, 0, 1);
        }
        if (fullbright_tex != TextureID(-1)) {
            return emission_scale *
                   ldr_to_hdr(lod.sample(tm.get_sampler2D(fullbright_tex), uv).rgb);
        }
        return float3(0);
    }

    QuakeMaterialSample get_sample(const ShadingData sd,
                                   const TextureManager tm,
                                   const LODSampler2D lod,
                                   out float3 out_emission,
                                   out OrthonormalFrame out_shading_frame) {
        // Normal
        QuakeMaterialSample s;
        if (normal_tex != TextureID(-1)) {
            const float3 n = (tm.get_sampler2D(normal_tex).SampleLevel(sd.uv, 0).rgb - 0.5) * 2;
            const OrthonormalFrame mapped = sd.frame.apply_normal_map(n);
            if (merian_hint_clamp_normals) {
                const float3 clamped_n =
                    keller_clamp_normal(mapped.get_normal(), sd.face_normal, sd.wi);
                out_shading_frame = OrthonormalFrame(clamped_n, mapped.get_tangent());
            } else {
                out_shading_frame = mapped;
            }
        } else {
            out_shading_frame = sd.frame;
        }

        // Albedo / Emission
        float roughness = QUAKE_BASE_ROUGHNESS;
        float2 uv = sd.uv;
        if ((flags & MAT_TYPE_WARP) != 0u) {
            uv = quake_warp(uv, sd.time);
            if (flags == MAT_TYPE_WATER) {
                uv += quake_waves(uv, sd.time);
                roughness = 0.4f;
            }
        }

        float3 albedo = float3(1, 0, 1);
        if (let albedo_tex_id = header.get_alpha_texture_id()) {
            albedo = lod.sample(tm.get_sampler2D(albedo_tex_id), uv).rgb;
            // desaturate for a better look
            albedo = pow(max(float3(0.0003), albedo), 1. / 1.2);
        }

        if (get_flags() == MAT_TYPE_WATERFALL) {
            out_emission = albedo;
            // } else if (get_flags() == MAT_TYPE_TELE || get_flags() == MAT_TYPE_SPRITE) {
            //     TODO: Fix on host side!
            //     out_emission = ldr_to_hdr(albedo);
        } else if (fullbright_tex != TextureID(-1)) {
            out_emission = ldr_to_hdr(lod.sample(tm.get_sampler2D(fullbright_tex), uv).rgb);
        } else {
            out_emission = float3(0);
        }

        const float coverage = get_coverage(sd);
        out_emission *= get_emission_scale(sd);

        float f0 = QUAKE_BASE_F0;
        if (gloss_tex != TextureID(-1)) {
            const float gloss = tm.get_sampler2D(gloss_tex).SampleLevel(uv, 0).r;
            f0 = lerp(QUAKE_BASE_F0, 1.0f, gloss);
            roughness = lerp(QUAKE_BASE_ROUGHNESS, 0.3f, gloss);
        }
        const float alpha = ggx_roughness_to_alpha(roughness);
        const float pane_ior = coverage > 0.f ? get_pane_ior() : 1.f;
        const ThinDielectricBSDF clear_pane = ThinDielectricBSDF(1.f / pane_ior, float3(1.0f));
        const float3 transmittance = pane == PANE_LIQUID && is_translucent()
                                         ? saturate((1.f - albedo) / max(albedo, 1e-4f))
                                         : float3(0.f);
        s.brdf =
            QuakeBSDF(clear_pane,
                      QuakeSurfaceBSDF(FresnelMixBSDF<GGXBRDF, DiffuseTransmissionBSDF>(
                                           GGXBRDF(alpha),
                                           DiffuseTransmissionBSDF(float3(1.f), transmittance), f0),
                                       albedo),
                      coverage);
        return s;
    }
}

struct QuakeMaterialSample : MaterialSample {
    typealias BSDFImpl = QuakeBSDF;

    QuakeBSDF brdf;

    BSDFImpl get_bsdf() {
        return brdf;
    }
}

} // namespace merian
