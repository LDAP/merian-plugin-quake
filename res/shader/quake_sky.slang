#include "merian-shaders/utils/math-constants.slangh"
#include "shader/quake-material.slangh"

__exported import merian_shaders.scene.environment_map;

import merian_shaders.utils.envmap;
import merian_shaders.distributions.von_mises_fisher;

namespace merian {

extern static const float quake_classic_sky_exp_scale = 10.f;
extern static const float quake_classic_sky_exp_rate = 3.5f;
extern static const float quake_classic_sky_pow_scale = 0.f;
extern static const float quake_classic_sky_pow_gamma = 1.f;
extern static const float quake_cube_sky_exp_scale = 0.f;
extern static const float quake_cube_sky_exp_rate = 3.5f;
extern static const float quake_cube_sky_pow_scale = 1.f;
extern static const float quake_cube_sky_pow_gamma = 1.f;
extern static const float quake_sun_lobe = 1.f;
extern static const float quake_sun_disc = 1.f;
extern static const float quake_sun_kappa = 3000.f;

float3 sky_emission(const float3 tex,
                    const float exp_scale,
                    const float exp_rate,
                    const float pow_scale,
                    const float pow_gamma) {
    return exp_scale * (exp2(exp_rate * tex) - 1.0) + pow_scale * pow(tex, pow_gamma);
}

float3 quake_sun(const float3 dir, const float3 sun_dir, const float3 sun_color) {
    const float lobe = 0.5 * pow(0.5 * (1.0 + dot(sun_dir, dir)), 4.0);
    const float disc_peak = 5.0 * quake_sun_disc * max(sun_color.r, max(sun_color.g, sun_color.b));
    const float kappa =
        disc_peak > 0.0 ? min(quake_sun_kappa, MERIAN_TWO_PI * QUAKE_MAX_RADIANCE / disc_peak)
                        : quake_sun_kappa;
    const float disc = 5.0 * vmf_pdf_tokuyoshi(dir, sun_dir, kappa);
    return min((quake_sun_lobe * lobe + quake_sun_disc * disc) * sun_color, QUAKE_MAX_RADIANCE);
}

struct QuakeClassicSkyEnvMap : IEnvMap {
    TextureID solid;
    TextureID alpha;
    float3 sun_dir;
    float3 sun_color;

    float3 eval(const ShadingData sd, const TextureManager tm, const LODSampler2D lod) {
        const float3 dir = -sd.wi;
        const float2 st = 0.5 + float2(dir.x, dir.y) / abs(dir.z);
        const float2 t = sd.time * float2(0.12, 0.12);
        const float4 bck = lod.sample(tm.get_sampler2D(solid), st + 0.5 * t);
        const float4 fnt = lod.sample(tm.get_sampler2D(alpha), st + t);
        const float3 tex = lerp(bck.rgb, fnt.rgb, fnt.a);
        float3 emission = sky_emission(tex, quake_classic_sky_exp_scale, quake_classic_sky_exp_rate,
                                       quake_classic_sky_pow_scale, quake_classic_sky_pow_gamma);
        if (sd.bounce > 0) {
            emission += quake_sun(dir, sun_dir, sun_color);
        }
        return emission;
    }

}

struct QuakeCubemapSkyEnvMap : IEnvMap {
    TextureID rt;
    TextureID bk;
    TextureID lf;
    TextureID ft;
    TextureID up;
    TextureID dn;
    float3 sun_dir;
    float3 sun_color;

    float3 eval(const ShadingData sd, const TextureManager tm, const LODSampler2D lod) {
        const float3 dir = -sd.wi;
        TextureID id = rt;
        float2 uv;

        switch (cubemap_side(dir)) {
        case 0: {
            id = rt;
            uv = 0.5 + 0.5 * float2(-dir.y, -dir.z) / abs(dir.x);
            break;
        }
        case 1: {
            id = lf;
            uv = 0.5 + 0.5 * float2(dir.y, -dir.z) / abs(dir.x);
            break;
        }
        case 2: {
            id = bk;
            uv = 0.5 + 0.5 * float2(dir.x, -dir.z) / abs(dir.y);
            break;
        }
        case 3: {
            id = ft;
            uv = 0.5 + 0.5 * float2(-dir.x, -dir.z) / abs(dir.y);
            break;
        }
        case 4: {
            id = up;
            uv = 0.5 + 0.5 * float2(-dir.y, dir.x) / abs(dir.z);
            break;
        }
        case 5: {
            id = dn;
            uv = 0.5 + 0.5 * float2(-dir.y, -dir.x) / abs(dir.z);
            break;
        }
        }

        float3 emission = sky_emission(lod.sample(tm.get_sampler2D(id), uv).rgb,
                                       quake_cube_sky_exp_scale, quake_cube_sky_exp_rate,
                                       quake_cube_sky_pow_scale, quake_cube_sky_pow_gamma);
        if (sd.bounce > 0) {
            emission += quake_sun(dir, sun_dir, sun_color);
        }
        return emission;
    }

}

} // namespace merian
