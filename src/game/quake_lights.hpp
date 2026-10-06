#pragma once

#include "merian-shaders/scene/scene-data.slangh"
#include "merian/utils/vector_matrix.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "quakedef.h"
}

namespace merian_quake {

enum class LightFalloff : uint8_t {
    LINEAR,
    INVERSE,
    INVERSE_SQUARE,
    NONE,
    LOCAL_MINLIGHT,
    INVERSE_SQUARE_OFFSET,
};

struct LightEntity {
    merian::float3 origin{0};
    float light = 300;
    merian::float3 color{1};
    uint8_t style = 0;
    LightFalloff falloff = LightFalloff::LINEAR;
    float wait = 1;
    float deviance = 0;
    merian::float3 spot_direction{0};
    float spot_half_angle = 0;
    bool switched = false;
    bool has_model = false;
    std::string surface;
};

struct SunLight {
    merian::float3 direction{0, 0, 1};
    float light = 300;
    merian::float3 color{1};
};

merian::float3 parse_color(const char* value);

std::optional<SunLight> parse_sun_light(const char* entities);

float light_entity_intensity(const LightEntity& light);

float light_emitter_area(const LightEntity& light, float radius);

void append_light_emitter(std::vector<merian::PackedVertexData>& vertices,
                          std::vector<merian::uint3>& indices,
                          const LightEntity& light,
                          float radius,
                          const merian::float2& uv);

float model_projected_area(const aliashdr_t* hdr, int skin);

float sprite_projected_area(const mspriteframe_t* frame);

struct SwitchedEmitter {
    size_t light_index;
    merian::float3 intensity;
};

class MapLights {
  public:
    MapLights() = default;
    MapLights(const char* entities, qmodel_t* world_model);
    MapLights(const MapLights&) = delete;
    MapLights& operator=(const MapLights&) = delete;
    MapLights(MapLights&&) = default;
    MapLights& operator=(MapLights&&) = default;

    const std::vector<LightEntity>& get_lights() const {
        return lights;
    }

    struct SurfaceLight {
        uint32_t tint = 0;
        uint8_t style = 0;
        float radiance = 0;
    };
    SurfaceLight surface_light(const msurface_t& surf) const;

    float model_intensity(const merian::float3& origin) const;

    std::vector<SwitchedEmitter> switched_emitters(float radius) const;

    gltexture_t* ensure_emission_texture(texture_t* tex, uint32_t tint);

    void release();

  private:
    using LightCells = std::unordered_map<uint64_t, std::vector<size_t>>;
    std::vector<size_t>
    lights_near(const LightCells& cells, const merian::float3& lo, const merian::float3& hi) const;
    const LightEntity* surface_owner(const msurface_t& surf) const;
    const LightEntity* light_in_front(const msurface_t& surf) const;

    struct EmissionTexture {
        std::vector<uint8_t> rgba;
        gltexture_t* texture = nullptr;
    };

    qmodel_t* world = nullptr;
    std::vector<LightEntity> lights;
    std::vector<size_t> lamp_candidates;
    LightCells lamp_candidate_cells;
    std::unordered_map<const texture_t*, size_t> template_lights;
    std::vector<float> intensity;
    std::vector<size_t> model_lights;
    LightCells model_light_cells;
    std::vector<bool> near_emissive_surface;
    std::unordered_map<const msurface_t*, SurfaceLight> surface_lights;
    std::unordered_map<texture_t*, std::unordered_map<uint32_t, EmissionTexture>> emissions;
};

} // namespace merian_quake
