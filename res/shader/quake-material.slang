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

extern static const float quake_fullbright_scale = 2.f;
extern static const float quake_fullbright_gamma = 0.5f;
extern static const float quake_fullbright_exponent = 0.1f;
extern static const float quake_fullbright_max_level = 0.99f;
extern static const float quake_fullbright_red = 0.f;
extern static const float quake_fullbright_yellow = 0.f;
extern static const float quake_fullbright_blue = 0.f;
extern static const float quake_waterfall_emission = 1.f;
extern static const float quake_fixture = 1.f;
extern static const float quake_switched_lights = 1.f;

float hue_weight(const float hue, const float center, const float width) {
    return saturate(1 - abs(fmod(hue - center + 540, 360) - 180) / width);
}

float fullbright_hue_gain(const float3 color) {
    const float v = max(color.r, max(color.g, color.b));
    const float d = v - min(color.r, min(color.g, color.b));
    if (d <= 0)
        return 1;
    float hue;
    if (v == color.r)
        hue = 60 * (color.g - color.b) / d;
    else if (v == color.g)
        hue = 60 * (2 + (color.b - color.r) / d);
    else
        hue = 60 * (4 + (color.r - color.g) / d);
    hue = fmod(hue + 360, 360);
    return max(0.f, 1 + d / v *
                            (quake_fullbright_red * hue_weight(hue, 0, 40) +
                             quake_fullbright_yellow * hue_weight(hue, 50, 50) +
                             quake_fullbright_blue * hue_weight(hue, 220, 80)));
}

float3 ldr_to_hdr(const float3 color) {
    const float l = min(pow((color.r + color.g + color.b) / 3, quake_fullbright_exponent),
                        quake_fullbright_max_level);
    return quake_fullbright_scale * pow(color, quake_fullbright_gamma) * (l / (1.0 - l)) *
           fullbright_hue_gain(color);
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
    uint16_t emission_scale;

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
        return f16tof32(uint(emission_scale)) *
               (get_flags() == MAT_TYPE_WATERFALL ? min(1.f, coverage * QUAKE_WATERFALL_EMISSION_GAIN)
                                                  : coverage);
    }

    float3 fullbright_emission(const float3 color) {
        return get_flags() == MAT_TYPE_FIXTURE ? quake_fixture * color : ldr_to_hdr(color);
    }

    float3 get_light_entity_emission(const ShadingData sd, const TextureManager tm) {
        const float4 radiance = tm.get_sampler2D(fullbright_tex).SampleLevel(sd.uv, 0);
        const float4 spot = tm.get_sampler2D(normal_tex).SampleLevel(sd.uv, 0);
        if (spot.a > 0 && dot(sd.wi, spot.rgb * 2 - 1) < cos(radians(spot.a * 90))) {
            return float3(0);
        }
        return quake_switched_lights * radiance.rgb *
               exp2((radiance.a - 0.5) * LIGHT_ENTITY_LOG2_RADIANCE_RANGE);
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

        float3 emission = float3(0);
        if (get_flags() == MAT_TYPE_LIGHT_ENTITY) {
            emission = get_light_entity_emission(sd, tm);
        } else if (get_flags() == MAT_TYPE_WATERFALL) {
            emission = float3(1, 0, 1);
            if (let albedo_tex_id = header.get_alpha_texture_id()) {
                emission = quake_waterfall_emission *
                           pow(lod.sample(tm.get_sampler2D(albedo_tex_id), uv).rgb, 1. / 1.2);
            }
        } else if (fullbright_tex != TextureID(-1)) {
            emission = fullbright_emission(lod.sample(tm.get_sampler2D(fullbright_tex), uv).rgb);
        }
        return min(get_emission_scale(sd) * emission, QUAKE_MAX_RADIANCE);
    }

    QuakeMaterialSample get_sample(const ShadingData sd,
                                   const TextureManager tm,
                                   const LODSampler2D lod,
                                   out float3 out_emission,
                                   out OrthonormalFrame out_shading_frame) {
        // Normal
        QuakeMaterialSample s;
        if (normal_tex != TextureID(-1) && get_flags() != MAT_TYPE_LIGHT_ENTITY) {
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

        if (get_flags() == MAT_TYPE_LIGHT_ENTITY) {
            albedo = float3(0);
            out_emission = get_light_entity_emission(sd, tm);
        } else if (get_flags() == MAT_TYPE_WATERFALL) {
            out_emission = quake_waterfall_emission * albedo;
            // } else if (get_flags() == MAT_TYPE_TELE || get_flags() == MAT_TYPE_SPRITE) {
            //     TODO: Fix on host side!
            //     out_emission = ldr_to_hdr(albedo);
        } else if (fullbright_tex != TextureID(-1)) {
            out_emission =
                fullbright_emission(lod.sample(tm.get_sampler2D(fullbright_tex), uv).rgb);
        } else {
            out_emission = float3(0);
        }

        const float coverage = get_coverage(sd);
        out_emission = min(out_emission * get_emission_scale(sd), QUAKE_MAX_RADIANCE);

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
