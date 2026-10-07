#include "game/quake_lights.hpp"

#include "merian/utils/aabb.hpp"
#include "merian/utils/colors.hpp"
#include "merian/utils/math.hpp"
#include "merian/utils/normal_encoding.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_set>

extern "C" {
#include "gl_texmgr.h"
}

namespace merian_quake {

namespace {

constexpr uint8_t BRIGHT_TEXEL_THRESHOLD = 215;
constexpr uint8_t LAMP_TEXEL_THRESHOLD = 180;
constexpr float TINTED_SATURATION = 0.1f;
constexpr float UNTINTED_SATURATION = 0.25f;
constexpr uint8_t FIRST_FULLBRIGHT_INDEX = 224;
constexpr uint8_t TRANSPARENT_INDEX = 255;
constexpr float MIN_EMITTING_TEXEL_FRACTION = 0.02f;
constexpr float SELF_LIT_FULLBRIGHT_FRACTION = 0.9f;
constexpr float LAMP_LIGHT_DISTANCE = 48.f;
constexpr float MODEL_EXCLUSION_DISTANCE = 32.f;
constexpr float MODEL_ORIGIN_TOLERANCE = 16.f;
constexpr float DONOR_DISTANCE = 64.f;
constexpr uint32_t DISC_SEGMENTS = 8;
constexpr float MAX_RASTER_TEXELS = 262144.f;
constexpr float CELL_SIZE = 64.f;

merian::float3 parse_float3(const char* value) {
    merian::float3 v(0);
    sscanf(value, "%f %f %f", &v.x, &v.y, &v.z);
    return v;
}

std::string lowercase(const std::string_view name) {
    std::string lower(name);
    std::ranges::transform(lower, lower.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower;
}

using Entity = std::unordered_map<std::string, std::string>;

std::vector<Entity> parse_entities(const char* entities) {
    std::vector<Entity> parsed;
    const char* data = entities;
    while ((data = COM_Parse(data)) != nullptr && com_token[0] == '{') {
        Entity& entity = parsed.emplace_back();
        while ((data = COM_Parse(data)) != nullptr && com_token[0] != '}') {
            const std::string key = com_token;
            if ((data = COM_Parse(data)) == nullptr)
                break;
            entity[key] = com_token;
        }
    }
    return parsed;
}

std::unordered_map<std::string, merian::float3> target_origins(const std::vector<Entity>& parsed) {
    std::unordered_map<std::string, merian::float3> targets;
    for (const Entity& entity : parsed)
        if (entity.contains("targetname") && entity.contains("origin"))
            targets[entity.at("targetname")] = parse_float3(entity.at("origin").c_str());
    return targets;
}

std::optional<merian::float3>
light_direction(const Entity& entity,
                const merian::float3& origin,
                const std::unordered_map<std::string, merian::float3>& targets) {
    if (const auto it = entity.find("mangle"); it != entity.end()) {
        const merian::float3 mangle =
            parse_float3(it->second.c_str()) * (std::numbers::pi_v<float> / 180.f);
        return merian::float3(std::cos(mangle.x) * std::cos(mangle.y),
                              std::sin(mangle.x) * std::cos(mangle.y), std::sin(mangle.y));
    }
    if (const auto it = entity.find("target"); it != entity.end() && targets.contains(it->second)) {
        const merian::float3 to_target = targets.at(it->second) - origin;
        if (merian::length(to_target) > 0)
            return merian::normalize(to_target);
    }
    return std::nullopt;
}

std::vector<LightEntity> parse_lights(const std::vector<Entity>& parsed) {
    const std::unordered_map<std::string, merian::float3> targets = target_origins(parsed);
    std::unordered_set<std::string> targeted;
    for (const Entity& entity : parsed)
        for (const char* key : {"target", "target2", "killtarget"})
            if (const auto it = entity.find(key); it != entity.end())
                targeted.insert(it->second);

    std::vector<LightEntity> lights;
    for (const Entity& entity : parsed) {
        const auto value = [&](const char* key) -> const char* {
            const auto it = entity.find(key);
            return it != entity.end() ? it->second.c_str() : nullptr;
        };
        const auto first_value = [&](const char* key, const char* alternative) {
            const char* v = value(key);
            return v != nullptr ? v : value(alternative);
        };
        const std::string_view classname = value("classname") != nullptr ? value("classname") : "";
        if (!classname.starts_with("light") || value("origin") == nullptr ||
            (value("_sun") != nullptr && atoi(value("_sun")) != 0))
            continue;

        LightEntity& light = lights.emplace_back();
        light.origin = parse_float3(value("origin"));
        light.has_model = classname != "light" && !classname.starts_with("light_fluoro");
        if (const char* v = first_value("light", "_light"))
            light.light = static_cast<float>(atof(v));
        if (const char* v = first_value("_color", "color"))
            light.color = parse_color(v);
        if (const char* v = value("style"))
            light.style = static_cast<uint8_t>(atoi(v));
        if (const char* v = value("delay"))
            light.falloff = static_cast<LightFalloff>(atoi(v));
        if (const char* v = value("wait"))
            light.wait = static_cast<float>(atof(v));
        if (const char* v = value("_deviance"))
            light.deviance = static_cast<float>(atof(v));
        if (const char* v = value("_surface"))
            light.surface = v;
        light.switched = value("targetname") != nullptr && targeted.contains(value("targetname"));

        if (const std::optional<merian::float3> direction =
                light_direction(entity, light.origin, targets)) {
            const float cone =
                value("angle") != nullptr ? static_cast<float>(atof(value("angle"))) : 0;
            light.spot_direction = *direction;
            light.spot_half_angle = (cone > 0 ? cone : 40.f) / 2;
        }
    }
    return lights;
}

std::array<uint8_t, 4> palette_rgba(const uint8_t index) {
    std::array<uint8_t, 4> rgba;
    std::memcpy(rgba.data(), &d_8to24table[index], 4);
    return rgba;
}

uint8_t max_channel(const uint8_t index) {
    const std::array<uint8_t, 4> rgba = palette_rgba(index);
    return std::max({rgba[0], rgba[1], rgba[2]});
}

bool is_fullbright(const uint8_t index) {
    return index >= FIRST_FULLBRIGHT_INDEX;
}

bool is_emitting(const uint8_t index) {
    return is_fullbright(index) || max_channel(index) >= BRIGHT_TEXEL_THRESHOLD;
}

bool is_lamp_texel(const uint8_t index) {
    return is_fullbright(index) || max_channel(index) >= LAMP_TEXEL_THRESHOLD;
}

uint32_t pack_tint(const merian::float3& color) {
    const float max_color = std::max({color.r, color.g, color.b});
    if (max_color <= 0)
        return 0xffffffffu;
    const auto unorm = [&](const float c) {
        return static_cast<uint32_t>(std::lround(std::clamp(c / max_color, 0.f, 1.f) * 255));
    };
    return unorm(color.r) | unorm(color.g) << 8u | unorm(color.b) << 16u | 0xffu << 24u;
}

std::array<uint8_t, 4>
tinted(std::array<uint8_t, 4> rgba, const uint32_t tint, const float amount = 1.f) {
    const auto tint_rgba = std::bit_cast<std::array<uint8_t, 4>>(tint);
    for (size_t c = 0; c < 4; c++)
        rgba[c] = static_cast<uint8_t>(std::lround(
            rgba[c] * (255.f + amount * (static_cast<float>(tint_rgba[c]) - 255.f)) / 255.f));
    return rgba;
}

float saturation(const std::array<uint8_t, 4>& rgba) {
    const uint8_t high = std::max({rgba[0], rgba[1], rgba[2]});
    const uint8_t low = std::min({rgba[0], rgba[1], rgba[2]});
    return high > 0 ? static_cast<float>(high - low) / static_cast<float>(high) : 0.f;
}

float luminance(const std::array<uint8_t, 4>& rgba) {
    static const std::array<float, 256> SRGB_TO_LINEAR = [] {
        std::array<float, 256> table;
        for (size_t i = 0; i < table.size(); i++)
            table[i] = merian::srgb_to_linear(static_cast<float>(i) / 255);
        return table;
    }();
    return merian::yuv_luminance(
        merian::float3(SRGB_TO_LINEAR[rgba[0]], SRGB_TO_LINEAR[rgba[1]], SRGB_TO_LINEAR[rgba[2]]));
}

const uint8_t* texture_pixels(const texture_t* tex) {
    return reinterpret_cast<const uint8_t*>(tex + 1);
}

bool has_pixels(const texture_t* tex) {
    return tex != r_notexture_mip && tex != r_notexture_mip2 && tex->width > 0 && tex->height > 0 &&
           !std::string_view(tex->name).starts_with("sky");
}

bool is_liquid(const texture_t* tex) {
    return tex->name[0] == '*';
}

bool is_waterfall(const texture_t* tex) {
    return strstr(tex->name, "wfall") != nullptr;
}

bool is_fence(const texture_t* tex) {
    return tex->name[0] == '{';
}

bool is_fence_lamp_texel(const uint8_t index) {
    return index != TRANSPARENT_INDEX && is_lamp_texel(index);
}

using TexelPredicate = bool (*)(uint8_t);

TexelPredicate emissive_texels(const texture_t* tex) {
    return is_liquid(tex) ? is_fullbright : is_emitting;
}

TexelPredicate lamp_texels(const texture_t* tex) {
    if (is_liquid(tex))
        return is_fullbright;
    return is_fence(tex) ? is_fence_lamp_texel : is_lamp_texel;
}

uint64_t lamp_texel_kind(const texture_t* tex) {
    return is_liquid(tex) ? 1 : is_fence(tex) ? 2 : 0;
}

template <typename Counts> float texel_fraction(const texture_t* tex, const Counts& counts) {
    std::array<bool, 256> counted_index;
    for (size_t i = 0; i < counted_index.size(); i++)
        counted_index[i] = counts(static_cast<uint8_t>(i));
    const bool fence = is_fence(tex);
    const uint8_t* pixels = texture_pixels(tex);
    uint32_t opaque = 0;
    uint32_t counted = 0;
    for (uint32_t i = 0; i < tex->width * tex->height; i++) {
        if (fence && pixels[i] == TRANSPARENT_INDEX)
            continue;
        opaque++;
        counted += counted_index[pixels[i]] ? 1 : 0;
    }
    return opaque > 0 ? static_cast<float>(counted) / static_cast<float>(opaque) : 0.f;
}

bool is_emissive_texture(const texture_t* tex) {
    return has_pixels(tex) &&
           texel_fraction(tex, emissive_texels(tex)) >= MIN_EMITTING_TEXEL_FRACTION;
}

enum class TextureRole : uint8_t { DARK, SELF_LIT, LAMP };

TextureRole texture_role(const texture_t* tex) {
    if (!is_emissive_texture(tex))
        return TextureRole::DARK;
    if (tex->gltexture == nullptr || is_waterfall(tex) ||
        (is_liquid(tex) && texel_fraction(tex, is_fullbright) >= SELF_LIT_FULLBRIGHT_FRACTION))
        return TextureRole::SELF_LIT;
    return TextureRole::LAMP;
}

template <typename TexelLuminance, typename TileMean>
float mean_luminance(const std::vector<merian::float2>& polygon,
                     const uint32_t width,
                     const uint32_t height,
                     const TexelLuminance& texel_luminance,
                     const TileMean& tile_mean) {
    merian::float2 lo = polygon[0];
    merian::float2 hi = polygon[0];
    for (const merian::float2& vertex : polygon) {
        lo = merian::min(lo, vertex);
        hi = merian::max(hi, vertex);
    }
    if (hi.x - lo.x >= static_cast<float>(width) && hi.y - lo.y >= static_cast<float>(height))
        return tile_mean();
    const int stride = std::max(
        1,
        static_cast<int>(std::ceil(std::sqrt((hi.x - lo.x) * (hi.y - lo.y) / MAX_RASTER_TEXELS))));
    float sum = 0;
    int count = 0;
    for (int y = static_cast<int>(std::floor(lo.y)); y < static_cast<int>(std::ceil(hi.y));
         y += stride)
        for (int x = static_cast<int>(std::floor(lo.x)); x < static_cast<int>(std::ceil(hi.x));
             x += stride)
            if (merian::convex_polygon_contains(polygon, merian::float2(x + 0.5f, y + 0.5f))) {
                sum += texel_luminance(merian::wrap(x, width), merian::wrap(y, height));
                count++;
            }
    if (count > 0)
        return sum / static_cast<float>(count);
    const merian::float2 center = (lo + hi) * 0.5f;
    return texel_luminance(merian::wrap(static_cast<int>(std::floor(center.x)), width),
                           merian::wrap(static_cast<int>(std::floor(center.y)), height));
}

using LuminanceTable = std::array<float, 256>;

std::array<uint8_t, 4> lamp_rgba(const uint8_t index, const uint32_t tint) {
    const std::array<uint8_t, 4> rgba = palette_rgba(index);
    if (!is_fullbright(index))
        return tinted(rgba, tint);
    return tinted(rgba, tint,
                  std::clamp((UNTINTED_SATURATION - saturation(rgba)) /
                                 (UNTINTED_SATURATION - TINTED_SATURATION),
                             0.f, 1.f));
}

LuminanceTable emitted_luminance(const uint32_t tint, const TexelPredicate emits) {
    LuminanceTable table;
    for (size_t i = 0; i < table.size(); i++) {
        const auto index = static_cast<uint8_t>(i);
        table[i] = emits(index) ? luminance(lamp_rgba(index, tint)) : 0.f;
    }
    return table;
}

float texture_mean(const uint8_t* pixels, const uint32_t texel_count, const LuminanceTable& table) {
    float sum = 0;
    for (uint32_t i = 0; i < texel_count; i++)
        sum += table[pixels[i]];
    return sum / static_cast<float>(std::max(texel_count, 1u));
}

template <typename TileMean>
float surface_projected_area(const msurface_t& surf,
                             const LuminanceTable& table,
                             const TileMean& tile_mean) {
    const glpoly_t* poly = surf.polys;
    if (poly == nullptr)
        return 0;
    const texture_t* tex = surf.texinfo->texture;
    const uint8_t* pixels = texture_pixels(tex);
    const auto position = [&](const int v) {
        return merian::float3(poly->verts[v][0], poly->verts[v][1], poly->verts[v][2]);
    };

    float area = 0;
    std::vector<merian::float2> texels;
    for (int v = 0; v < poly->numverts; v++) {
        texels.emplace_back(poly->verts[v][3] * static_cast<float>(tex->width),
                            poly->verts[v][4] * static_cast<float>(tex->height));
        if (v >= 2)
            area += merian::triangle_area(position(0), position(v - 1), position(v));
    }
    return area / 4 *
           mean_luminance(
               texels, tex->width, tex->height,
               [&](const uint32_t x, const uint32_t y) {
                   return table[pixels[y * tex->width + x]];
               },
               tile_mean);
}

merian::AABB bounds(const msurface_t& surf) {
    return {merian::as_float3(surf.mins), merian::as_float3(surf.maxs)};
}

bool in_front_of(const msurface_t& surf, const merian::float3& p) {
    const vec3_t point = {p.x, p.y, p.z};
    const float distance = DotProduct(point, surf.plane->normal) - surf.plane->dist;
    return (surf.flags & SURF_PLANEBACK) != 0 ? distance < 0 : distance > 0;
}

float own_intensity(const LightEntity& light) {
    return light_entity_intensity(light) * merian::yuv_luminance(light.color);
}

std::array<int, 3> cell_of(const merian::float3& p) {
    return {static_cast<int>(std::floor(p.x / CELL_SIZE)),
            static_cast<int>(std::floor(p.y / CELL_SIZE)),
            static_cast<int>(std::floor(p.z / CELL_SIZE))};
}

uint64_t cell_key(const std::array<int, 3>& cell) {
    const auto bits = [](const int v) { return static_cast<uint64_t>(v + (1 << 20)) & 0x1FFFFFu; };
    return bits(cell[0]) | bits(cell[1]) << 21u | bits(cell[2]) << 42u;
}

std::unordered_map<uint64_t, std::vector<size_t>>
build_cells(const std::vector<LightEntity>& lights, const std::vector<size_t>& indices) {
    std::unordered_map<uint64_t, std::vector<size_t>> cells;
    for (const size_t i : indices)
        cells[cell_key(cell_of(lights[i].origin))].push_back(i);
    return cells;
}

std::vector<bool> trigger_surfaces(const std::vector<Entity>& parsed, const qmodel_t& world) {
    std::vector<bool> triggers(world.numsurfaces, false);
    for (const Entity& entity : parsed) {
        const auto classname = entity.find("classname");
        const auto model = entity.find("model");
        if (classname == entity.end() || !classname->second.starts_with("trigger_") ||
            model == entity.end() || !model->second.starts_with('*'))
            continue;
        const int index = atoi(model->second.c_str() + 1);
        if (index <= 0 || index >= world.numsubmodels)
            continue;
        const dmodel_t& submodel = world.submodels[index];
        std::fill_n(triggers.begin() + submodel.firstface, submodel.numfaces, true);
    }
    return triggers;
}

size_t find_root(std::vector<size_t>& parents, size_t i) {
    while (parents[i] != i)
        i = parents[i] = parents[parents[i]];
    return i;
}

} // namespace

merian::float3 parse_color(const char* value) {
    const merian::float3 color = parse_float3(value);
    return color.x > 1 || color.y > 1 || color.z > 1 ? color / 255.f : color;
}

std::optional<SunLight> parse_sun_light(const char* entities) {
    const std::vector<Entity> parsed = parse_entities(entities);
    const std::unordered_map<std::string, merian::float3> targets = target_origins(parsed);
    for (const Entity& entity : parsed) {
        const auto sun = entity.find("_sun");
        const auto origin = entity.find("origin");
        if (sun == entity.end() || atoi(sun->second.c_str()) == 0 || origin == entity.end())
            continue;
        const std::optional<merian::float3> direction =
            light_direction(entity, parse_float3(origin->second.c_str()), targets);
        if (!direction)
            continue;
        SunLight light{.direction = -*direction};
        if (const auto it = entity.find("light"); it != entity.end())
            light.light = static_cast<float>(atof(it->second.c_str()));
        if (const auto it = entity.find("_color"); it != entity.end())
            light.color = parse_color(it->second.c_str());
        return light;
    }
    return std::nullopt;
}

float light_entity_intensity(const LightEntity& light) {
    constexpr float REFERENCE_DISTANCE = 128;
    const float wait = std::max(light.wait, 0.01f);
    switch (light.falloff) {
    case LightFalloff::LINEAR: {
        const float half_range = light.light / (2 * wait);
        return (light.light / 2) * half_range * half_range;
    }
    case LightFalloff::INVERSE:
        return light.light / wait * REFERENCE_DISTANCE * REFERENCE_DISTANCE;
    case LightFalloff::INVERSE_SQUARE:
    case LightFalloff::INVERSE_SQUARE_OFFSET:
        return light.light / (wait * wait) * REFERENCE_DISTANCE * REFERENCE_DISTANCE;
    default:
        return light.light * REFERENCE_DISTANCE * REFERENCE_DISTANCE;
    }
}

float light_emitter_area(const LightEntity& light, const float radius) {
    constexpr float TAU = 2 * std::numbers::pi_v<float>;
    if (light.spot_half_angle > 0)
        return 0.5f * DISC_SEGMENTS * std::sin(TAU / DISC_SEGMENTS) * radius * radius;
    const float edge = radius / std::sin(TAU / 5);
    return 5 * std::numbers::sqrt3_v<float> * edge * edge / 4;
}

void append_light_emitter(std::vector<merian::PackedVertexData>& vertices,
                          std::vector<merian::uint3>& indices,
                          const LightEntity& light,
                          const float radius,
                          const merian::float2& uv) {
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    const auto add_vertex = [&](const merian::float3& position, const merian::float3& normal) {
        merian::PackedVertexData vertex{};
        vertex.position = position;
        vertex.encoded_normal = merian::encode_normal(normal);
        vertex.uv = merian::half2(uv.x, uv.y);
        vertices.push_back(vertex);
    };

    if (light.spot_half_angle > 0) {
        const merian::float3& n = light.spot_direction;
        const merian::float3x3 frame = merian::rotation_from_to(merian::float3(0, 0, 1), n);
        const merian::float3 tangent = merian::mul(frame, merian::float3(1, 0, 0));
        const merian::float3 bitangent = merian::mul(frame, merian::float3(0, 1, 0));
        add_vertex(light.origin, n);
        for (uint32_t i = 0; i < DISC_SEGMENTS; i++) {
            const float phi = 2 * std::numbers::pi_v<float> * static_cast<float>(i) / DISC_SEGMENTS;
            add_vertex(
                light.origin + radius * (std::cos(phi) * tangent + std::sin(phi) * bitangent), n);
        }
        for (uint32_t i = 0; i < DISC_SEGMENTS; i++)
            indices.emplace_back(base, base + 1 + i, base + 1 + (i + 1) % DISC_SEGMENTS);
        return;
    }

    constexpr float T = std::numbers::phi_v<float>;
    static constexpr std::array<std::array<float, 3>, 12> CORNERS = {{
        {-1, T, 0},
        {1, T, 0},
        {-1, -T, 0},
        {1, -T, 0},
        {0, -1, T},
        {0, 1, T},
        {0, -1, -T},
        {0, 1, -T},
        {T, 0, -1},
        {T, 0, 1},
        {-T, 0, -1},
        {-T, 0, 1},
    }};
    static constexpr std::array<std::array<uint32_t, 3>, 20> FACES = {{
        {0, 11, 5},  {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
        {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
        {3, 8, 9},   {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1},
    }};
    for (const auto& corner : CORNERS) {
        const merian::float3 n = merian::normalize(merian::float3(corner[0], corner[1], corner[2]));
        add_vertex(light.origin + radius * n, n);
    }
    for (const auto& face : FACES)
        indices.emplace_back(base + face[0], base + face[1], base + face[2]);
}

float model_projected_area(const aliashdr_t* hdr, const int skin) {
    const auto* base = reinterpret_cast<const uint8_t*>(hdr);
    const uint8_t* texels = base + hdr->texels[skin];
    const auto* indexes = reinterpret_cast<const uint16_t*>(base + hdr->indexes);
    const auto* desc = reinterpret_cast<const aliasmesh_t*>(base + hdr->meshdesc);
    const auto* poses = reinterpret_cast<const trivertx_t*>(base + hdr->vertexes);
    const merian::float3 scale = merian::as_float3(hdr->scale);
    const auto width = static_cast<uint32_t>(hdr->skinwidth);
    const auto height = static_cast<uint32_t>(hdr->skinheight);
    const auto position = [&](const int v) {
        const trivertx_t& t = poses[desc[v].vertindex];
        return merian::float3(t.v[0], t.v[1], t.v[2]) * scale;
    };
    const auto texel_luminance = [&](const uint32_t x, const uint32_t y) {
        const uint8_t index = texels[y * width + x];
        return index >= FIRST_FULLBRIGHT_INDEX ? luminance(palette_rgba(index)) : 0.f;
    };
    const auto skin_mean = [&] {
        float sum = 0;
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
                sum += texel_luminance(x, y);
        return sum / static_cast<float>(std::max(width * height, 1u));
    };

    float area = 0;
    for (int t = 0; t < hdr->numindexes / 3; t++) {
        const std::array<int, 3> v = {indexes[3 * t], indexes[3 * t + 1], indexes[3 * t + 2]};
        const std::vector<merian::float2> st = {
            {desc[v[0]].st[0], desc[v[0]].st[1]},
            {desc[v[1]].st[0], desc[v[1]].st[1]},
            {desc[v[2]].st[0], desc[v[2]].st[1]},
        };
        area += merian::triangle_area(position(v[0]), position(v[1]), position(v[2])) / 4 *
                mean_luminance(st, width, height, texel_luminance, skin_mean);
    }
    return area;
}

float sprite_projected_area(const mspriteframe_t* frame) {
    const gltexture_t* glt = frame->gltexture;
    if (glt == nullptr || glt->source_format != SRC_INDEXED)
        return 0;
    uint8_t* data = COM_LoadMallocFile(glt->source_file, nullptr);
    if (data == nullptr)
        return 0;
    const uint32_t texel_count = glt->source_width * glt->source_height;
    float sum = 0;
    if (glt->source_offset + texel_count <= static_cast<src_offset_t>(com_filesize)) {
        const uint8_t* pixels = data + glt->source_offset;
        for (uint32_t i = 0; i < texel_count; i++)
            if (pixels[i] != TRANSPARENT_INDEX)
                sum += luminance(palette_rgba(pixels[i]));
    }
    free(data);
    const float area = (frame->right - frame->left) * (frame->up - frame->down);
    return area / 2 * sum / static_cast<float>(std::max(texel_count, 1u));
}

MapLights::MapLights(const char* entities, qmodel_t* world_model) : world(world_model) {
    const std::vector<Entity> parsed = parse_entities(entities);
    lights = parse_lights(parsed);
    intensity.assign(lights.size(), 0.f);
    std::unordered_map<std::string, size_t> templates;
    std::vector<bool> receives(lights.size(), false);
    for (size_t i = 0; i < lights.size(); i++) {
        const LightEntity& light = lights[i];
        if (light.light <= 0)
            continue;
        if (!light.surface.empty()) {
            templates.try_emplace(lowercase(light.surface), i);
        } else if (light.has_model) {
            model_lights.push_back(i);
            receives[i] = true;
            intensity[i] = own_intensity(light);
        } else {
            lamp_candidates.push_back(i);
        }
    }
    lamp_candidate_cells = build_cells(lights, lamp_candidates);
    model_light_cells = build_cells(lights, model_lights);
    near_emissive_surface.assign(lights.size(), false);
    for (int i = 0; i < world->numtextures; i++)
        if (const texture_t* tex = world->textures[i]; tex != nullptr)
            if (const auto it = templates.find(lowercase(tex->name)); it != templates.end())
                template_lights.emplace(tex, it->second);

    std::unordered_map<const texture_t*, TextureRole> texture_roles;
    std::vector<std::pair<const msurface_t*, size_t>> lamps;
    std::unordered_map<const texture_t*, size_t> liquid_roots;
    std::unordered_map<const texture_t*, std::vector<const msurface_t*>> unowned_liquid_faces;
    std::vector<bool> liquid_owner(lights.size(), false);
    std::vector<float> projected_area(lights.size(), 0.f);
    std::unordered_map<uint64_t, LuminanceTable> tables;
    std::unordered_map<const texture_t*, std::unordered_map<uint32_t, float>> tile_means;
    const auto lamp_area = [&](const msurface_t& surf, const uint32_t tint) {
        const texture_t* tex = surf.texinfo->texture;
        const auto [table, table_inserted] = tables.try_emplace(tint | lamp_texel_kind(tex) << 32u);
        if (table_inserted)
            table->second = emitted_luminance(tint, lamp_texels(tex));
        return surface_projected_area(surf, table->second, [&] {
            const auto [mean, mean_inserted] = tile_means[tex].try_emplace(tint, 0.f);
            if (mean_inserted)
                mean->second =
                    texture_mean(texture_pixels(tex), tex->width * tex->height, table->second);
            return mean->second;
        });
    };
    const std::vector<bool> is_trigger = trigger_surfaces(parsed, *world);
    for (int i = 0; i < world->numsurfaces; i++) {
        if (is_trigger[i])
            continue;
        const msurface_t& surf = world->surfaces[i];
        const texture_t* tex = surf.texinfo->texture;
        const auto [role, role_inserted] = texture_roles.try_emplace(tex, TextureRole::DARK);
        if (role_inserted)
            role->second = texture_role(tex);
        if (role->second == TextureRole::DARK)
            continue;
        for (const size_t candidate :
             lights_near(lamp_candidate_cells, merian::as_float3(surf.mins) - LAMP_LIGHT_DISTANCE,
                         merian::as_float3(surf.maxs) + LAMP_LIGHT_DISTANCE))
            if (bounds(surf).distance_sq(lights[candidate].origin) <=
                    LAMP_LIGHT_DISTANCE * LAMP_LIGHT_DISTANCE &&
                in_front_of(surf, lights[candidate].origin))
                near_emissive_surface[candidate] = true;
        if (role->second == TextureRole::SELF_LIT)
            continue;

        const LightEntity* light = surface_owner(surf);
        if (light == nullptr) {
            if (is_liquid(tex))
                unowned_liquid_faces[tex].push_back(&surf);
            continue;
        }
        const size_t index = static_cast<size_t>(light - lights.data());
        if (is_liquid(tex)) {
            liquid_owner[index] = true;
            liquid_roots.try_emplace(tex, index);
        }
        lamps.emplace_back(&surf, index);
        projected_area[index] += lamp_area(surf, pack_tint(light->color));
        if (!light->surface.empty()) {
            intensity[index] += own_intensity(*light);
        } else {
            receives[index] = true;
            intensity[index] = own_intensity(*light);
        }
    }

    for (const auto& [tex, faces] : unowned_liquid_faces)
        if (const auto root = liquid_roots.find(tex); root != liquid_roots.end())
            for (const msurface_t* surf : faces) {
                lamps.emplace_back(surf, root->second);
                projected_area[root->second] +=
                    lamp_area(*surf, pack_tint(lights[root->second].color));
            }

    std::vector<size_t> receivers;
    std::vector<size_t> lamp_owners;
    for (size_t i = 0; i < lights.size(); i++) {
        if (receives[i] && !liquid_owner[i])
            receivers.push_back(i);
        if (receives[i] && !lights[i].has_model && !liquid_owner[i])
            lamp_owners.push_back(i);
    }
    for (const size_t i : lamp_candidates) {
        const LightEntity& light = lights[i];
        if (receives[i] || light.switched || receivers.empty())
            continue;
        const auto distance_sq = [&](const size_t r) {
            return merian::dot(lights[r].origin - light.origin, lights[r].origin - light.origin);
        };
        const size_t nearest = *std::ranges::min_element(receivers, {}, distance_sq);
        if (distance_sq(nearest) < DONOR_DISTANCE * DONOR_DISTANCE)
            intensity[nearest] += own_intensity(light);
    }

    std::vector<size_t> fixture_parents(lights.size());
    std::iota(fixture_parents.begin(), fixture_parents.end(), size_t{0});
    const LightCells lamp_owner_cells = build_cells(lights, lamp_owners);
    for (const size_t i : lamp_owners)
        for (const size_t j : lights_near(lamp_owner_cells, lights[i].origin - DONOR_DISTANCE,
                                          lights[i].origin + DONOR_DISTANCE))
            if (lights[j].style == lights[i].style &&
                merian::length(lights[j].origin - lights[i].origin) < DONOR_DISTANCE)
                fixture_parents[find_root(fixture_parents, j)] = find_root(fixture_parents, i);
    for (const auto& [surf, index] : lamps)
        if (const auto root = liquid_roots.find(surf->texinfo->texture); root != liquid_roots.end())
            fixture_parents[find_root(fixture_parents, index)] =
                find_root(fixture_parents, root->second);

    std::vector<float> fixture_intensity(lights.size(), 0.f);
    std::vector<float> fixture_area(lights.size(), 0.f);
    for (size_t i = 0; i < lights.size(); i++) {
        const size_t fixture = find_root(fixture_parents, i);
        fixture_intensity[fixture] += intensity[i];
        fixture_area[fixture] += projected_area[i];
    }
    for (const auto& [surf, index] : lamps) {
        const size_t fixture = find_root(fixture_parents, index);
        surface_lights[surf] = {
            pack_tint(lights[index].color), lights[index].style,
            fixture_area[fixture] > 0 ? fixture_intensity[fixture] / fixture_area[fixture] : 0.f};
    }
}

MapLights::SurfaceLight MapLights::surface_light(const msurface_t& surf) const {
    if (const auto it = surface_lights.find(&surf); it != surface_lights.end())
        return it->second;
    if (surf.texinfo->texture->fullbright == nullptr)
        return {};
    const LightEntity* light = surface_owner(surf);
    return light != nullptr ? SurfaceLight{.style = light->style} : SurfaceLight{};
}

float MapLights::model_intensity(const merian::float3& origin) const {
    for (const size_t i : lights_near(model_light_cells, origin - MODEL_ORIGIN_TOLERANCE,
                                      origin + MODEL_ORIGIN_TOLERANCE))
        if (merian::length(lights[i].origin - origin) < MODEL_ORIGIN_TOLERANCE)
            return intensity[i];
    return 0.f;
}

std::vector<size_t> MapLights::lights_near(const LightCells& cells,
                                           const merian::float3& lo,
                                           const merian::float3& hi) const {
    const std::array<int, 3> first = cell_of(lo);
    const std::array<int, 3> last = cell_of(hi);
    std::vector<size_t> near;
    for (int x = first[0]; x <= last[0]; x++)
        for (int y = first[1]; y <= last[1]; y++)
            for (int z = first[2]; z <= last[2]; z++)
                if (const auto it = cells.find(cell_key({x, y, z})); it != cells.end())
                    near.insert(near.end(), it->second.begin(), it->second.end());
    std::ranges::sort(near);
    return near;
}

std::vector<SwitchedEmitter> MapLights::switched_emitters(const float radius) const {
    const auto near_model = [&](const merian::float3& origin) {
        return std::ranges::any_of(lights_near(model_light_cells, origin - MODEL_EXCLUSION_DISTANCE,
                                               origin + MODEL_EXCLUSION_DISTANCE),
                                   [&](const size_t i) {
                                       return merian::length(lights[i].origin - origin) <
                                              MODEL_EXCLUSION_DISTANCE;
                                   });
    };
    const auto radius_of = [&](const LightEntity& light) {
        return light.deviance > 0 ? light.deviance : radius;
    };

    std::vector<SwitchedEmitter> emitters;
    for (const size_t i : lamp_candidates) {
        const LightEntity& light = lights[i];
        if (!light.switched || near_emissive_surface[i] || near_model(light.origin))
            continue;
        const merian::float3 light_intensity = light_entity_intensity(light) * light.color;
        const auto overlapping = std::ranges::find_if(emitters, [&](const SwitchedEmitter& other) {
            const LightEntity& other_light = lights[other.light_index];
            return other_light.style == light.style &&
                   merian::length(other_light.origin - light.origin) <
                       radius_of(other_light) + radius_of(light);
        });
        if (overlapping != emitters.end())
            overlapping->intensity += light_intensity;
        else
            emitters.push_back({i, light_intensity});
    }
    return emitters;
}

const LightEntity* MapLights::surface_owner(const msurface_t& surf) const {
    const auto it = template_lights.find(surf.texinfo->texture);
    return it != template_lights.end() ? &lights[it->second] : light_in_front(surf);
}

const LightEntity* MapLights::light_in_front(const msurface_t& surf) const {
    const LightEntity* closest_light = nullptr;
    float closest_sq = LAMP_LIGHT_DISTANCE * LAMP_LIGHT_DISTANCE;
    for (const size_t i :
         lights_near(lamp_candidate_cells, merian::as_float3(surf.mins) - LAMP_LIGHT_DISTANCE,
                     merian::as_float3(surf.maxs) + LAMP_LIGHT_DISTANCE)) {
        const LightEntity& light = lights[i];
        const float distance_sq = bounds(surf).distance_sq(light.origin);
        if (distance_sq < closest_sq && in_front_of(surf, light.origin)) {
            closest_sq = distance_sq;
            closest_light = &light;
        }
    }
    return closest_light;
}

gltexture_t* MapLights::ensure_emission_texture(texture_t* tex, const uint32_t tint) {
    if (const auto it = emissions[tex].find(tint); it != emissions[tex].end())
        return it->second.texture;
    texture_t* frame = tex;
    do {
        const auto [it, inserted] = emissions[frame].try_emplace(tint);
        if (inserted) {
            EmissionTexture& emission = it->second;
            const uint32_t texel_count = frame->width * frame->height;
            const uint8_t* pixels = texture_pixels(frame);
            const TexelPredicate emits = lamp_texels(frame);
            emission.rgba.resize(static_cast<size_t>(texel_count) * 4);
            for (uint32_t i = 0; i < texel_count; i++)
                if (emits(pixels[i]))
                    std::ranges::copy(lamp_rgba(pixels[i], tint),
                                      emission.rgba.begin() + 4 * static_cast<size_t>(i));
            char name[64];
            q_snprintf(name, sizeof(name), "%s:%s_fixture_%08x", world->name, frame->name, tint);
            emission.texture = TexMgr_LoadImage(
                world, name, static_cast<int>(frame->width), static_cast<int>(frame->height),
                SRC_RGBA, emission.rgba.data(), "",
                reinterpret_cast<src_offset_t>(emission.rgba.data()), TEXPREF_MIPMAP);
        }
        frame = frame->anim_next;
    } while (frame != nullptr && frame != tex);
    return emissions.at(tex).at(tint).texture;
}

void MapLights::release() {
    for (const auto& [tex, tinted_emissions] : emissions)
        for (const auto& [tint, emission] : tinted_emissions)
            TexMgr_FreeTexture(emission.texture);
    emissions.clear();
}

} // namespace merian_quake
