// Quake material model: registered with merian::MaterialSystem from QuakeScene.
// Payload layout mirrors QuakeMaterialPayload in src/game/quake_material.hpp.

import merian_shaders.shading.materials.material;
import merian_shaders.shading.bsdfs.brdf_ggx;
import merian_shaders.shading.bsdfs.brdf_lambert_diffuse;
import merian_shaders.shading.bsdfs.bsdf_fresnel_mix;
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

typealias QuakeBSDF = FresnelMixBSDF<GGXBRDF, LambertDiffuseBRDF>;

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

    QuakeMaterialSample
    get_sample(const ShadingData sd, const TextureManager tm, const LODSampler2D lod) {
        // Normal
        QuakeMaterialSample s;
        if (normal_tex != TextureID(-1)) {
            const float3 n = (tm.get_sampler2D(normal_tex).SampleLevel(sd.uv, 0).rgb - 0.5) * 2;
            const OrthonormalFrame mapped = sd.frame.apply_normal_map(n);
            if (merian_hint_clamp_normals) {
                const float3 clamped_n =
                    keller_clamp_normal(mapped.get_normal(), sd.face_normal, sd.wi);
                s.frame = OrthonormalFrame(clamped_n, mapped.get_tangent());
            } else {
                s.frame = mapped;
            }
        } else {
            s.frame = sd.frame;
        }

        // Albedo / Emission
        float roughness = 0.6f;
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
            albedo = pow(albedo, 1. / 1.2);
        }

        if (get_flags() == MAT_TYPE_WATERFALL) {
            s.emission = albedo;
            // } else if (get_flags() == MAT_TYPE_TELE || get_flags() == MAT_TYPE_SPRITE) {
            //     TODO: Fix on host side!
            //     s.emission = ldr_to_hdr(albedo);
        } else if (fullbright_tex != TextureID(-1)) {
            s.emission = ldr_to_hdr(lod.sample(tm.get_sampler2D(fullbright_tex), uv).rgb);
        } else {
            s.emission = float3(0);
        }

        if (gloss_tex != TextureID(-1)) {
            roughness = lod.sample(tm.get_sampler2D(gloss_tex), uv).r;
        }
        const float alpha = ggx_roughness_to_alpha(max(roughness, merian_hint_min_roughness));
        s.brdf = QuakeBSDF(GGXBRDF(alpha), LambertDiffuseBRDF(albedo), 0.02f);
        return s;
    }
}

struct QuakeMaterialSample : MaterialSample {
    typealias BSDFImpl = QuakeBSDF;

    QuakeBSDF brdf;
    OrthonormalFrame frame;
    float3 emission;

    BSDFImpl get_bsdf() {
        return brdf;
    }
    property OrthonormalFrame shading_frame {
        get { return frame; }
    }
    property float3 shading_normal {
        get { return frame.get_normal(); }
    }
}

}
