#pragma once

#include "game/quake_draw.hpp"
#include "game/quake_lights.hpp"
#include "game/quake_material.hpp"

#include "merian-shaders/scene/scene.hpp"
#include "merian/shader/shader_compile_context.hpp"
#include "merian/utils/concurrent/concurrent_queue.hpp"
#include "merian/utils/input_controller.hpp"
#include "merian/utils/input_controller_dummy.hpp"
#include "merian/utils/input_listener.hpp"
#include "merian/utils/properties.hpp"
#include "merian/vk/memory/resource_allocator.hpp"
#include "merian/vk/utils/profiler.hpp"
#include "merian/vk/window/window.hpp"

#include <atomic>
#include <queue>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "quakedef.h"
}

namespace merian_quake {

// Texture-manager capacity; must match MAX_GLTEXTURES in quakespasm's gl_texmgr.c.
constexpr uint32_t MAX_GLTEXTURES = 4096;
constexpr auto PARTICLE_TEXTURE = static_cast<merian::TextureID>(MAX_GLTEXTURES);
constexpr auto FULLBRIGHT_PARTICLE_TEXTURE = static_cast<merian::TextureID>(MAX_GLTEXTURES + 1);
constexpr auto LIGHT_ENTITY_RADIANCE_TEXTURE = static_cast<merian::TextureID>(MAX_GLTEXTURES + 2);
constexpr auto LIGHT_ENTITY_SPOT_TEXTURE = static_cast<merian::TextureID>(MAX_GLTEXTURES + 3);
constexpr uint32_t TEXTURE_CAPACITY = MAX_GLTEXTURES + 4;

// One bit per geometry class, written into Mesh::instance_mask. Lets render
// passes include/exclude classes by ANDing with the TLAS trace mask.
enum class InstanceMask : uint8_t {
    WORLD = 1u << 0,        // worldspawn brushes
    BRUSH_ENTITY = 1u << 1, // moving/submodel brushes (doors, lifts, plats)
    ALIAS = 1u << 2,        // monsters, items, third-party players
    SPRITE = 1u << 3,       // sprite billboards
    PARTICLE = 1u << 4,     // particles
    VIEWENT = 1u << 5,      // first-person gun (cl.viewent)
    PLAYER_BODY = 1u << 6,  // local player's third-person body
    LIGHT_ENTITY = 1u << 7,
};

constexpr uint8_t to_mask(InstanceMask m) {
    return static_cast<uint8_t>(m);
}

// Owns Quake's global lifecycle: QuakeSpasm init, the game thread, the input
// listener, the per-frame scene refresh, and the texture upload pump. Quake
// runs on static globals so only one instance can exist at a time.
class QuakeScene : public merian::Scene {
  public:
    QuakeScene(const merian::ShaderCompileContextHandle& compile_context,
               const merian::ContextHandle& context,
               const merian::ResourceAllocatorHandle& allocator,
               const merian::MaterialSystemHandle& material_system);

    ~QuakeScene() override;

    merian::float3 get_up() override {
        return merian::float3(0, 0, 1);
    }

    // Expose Quake's `cl.time` to the shader-side scene clock.
    float get_time(float time) override;

    bool is_ready() const override {
        return last_scene_rendered;
    }

    merian::MaterialModelID get_quake_material_type_id() const {
        return quake_material_type_id;
    }

    // Pass a dummy controller to detach. The window enables text-input
    // toggling (Quake console / menu typing); pass nullptr to skip.
    void set_controller(const merian::InputControllerHandle& controller,
                        const merian::WindowHandle& window = nullptr);

    void queue_command(const std::string& command);

    // --- QuakeSpasm callbacks (invoked from the game thread via extern "C") ---
    void cb_VID_Changed();
    void cb_QS_texture_load(gltexture_t* glt, const uint32_t* data);
    void cb_IN_Move(usercmd_t* cmd);
    void cb_R_RenderScene();
    void cb_QS_worldspawn();

    // 2D draw stream hooks. Sticky state until next set_*.
    void cb_QS_ui_set_canvas(const UICanvas& canvas);
    void cb_QS_ui_set_scissor(const UIScissor& scissor);
    void cb_QS_ui_set_color(uint32_t rgba);
    void cb_QS_ui_push_quad(const UIDrawCmd& cmd);
    void cb_QS_ui_frame_ready();

    // Snapshot of the UI draw commands captured during the most recent on_update().
    const std::shared_ptr<UIDrawCommands>& get_ui_draw_commands() const {
        return last_ui_draw_commands;
    }

    // Called by QuakeNode::properties() to surface Quake-level settings.
    void properties(merian::Properties& config);

  protected:
    void on_update(const merian::CommandBufferHandle& cmd,
                   float time,
                   float time_diff,
                   uint32_t frame) override;

  private:
    void register_input_listener(const merian::InputControllerHandle& controller);

    // --- World lifecycle ---
    void unload_world();
    void load_world(const merian::CommandBufferHandle& cmd);
    void load_world_brushes();
    void register_alias_models(const merian::CommandBufferHandle& cmd);
    void register_sprite_models();
    void init_particle_batch();
    void update_sky();
    void update_fog();
    merian::FogVolume get_fog() const;

    // --- Per-frame ---
    void update_entities(const merian::CommandBufferHandle& cmd);
    void update_alias_entity(entity_t* ent,
                             const merian::CommandBufferHandle& cmd,
                             uint8_t instance_mask);
    void update_brush_entity(entity_t* ent,
                             const merian::CommandBufferHandle& cmd,
                             uint8_t instance_mask);
    void update_sprite_entity(entity_t* ent);
    void update_particles();
    void update_animated_materials();
    void update_sprite_materials();
    void update_material_constants();
    uint16_t quantized_fixture_emission(float radiance) const;
    uint16_t model_fixture_emission(const entity_t* ent, aliashdr_t* hdr, int skin);
    uint16_t sprite_fixture_emission(const entity_t* ent, mspriteframe_t* frame);
    merian::MaterialID
    ensure_alias_material(qmodel_t* model, aliashdr_t* hdr, int skin, uint16_t fixture_emission);
    merian::Scene::MeshID
    ensure_sprite_frame(const qmodel_t* model, mspriteframe_t* frame, uint16_t fixture_emission);
    void load_light_entities();
    void update_camera();

  private:
    merian::MaterialModelID quake_material_type_id{};

    vk::Extent3D resolution{};

    // Set while Host_Frame runs; callbacks use it for GPU-side work.
    merian::CommandBufferHandle active_cmd;
    UIDrawCommands ui_draw_commands;
    UICanvas ui_current_canvas{};
    UIScissor ui_current_scissor{0, 0, 0, -1};
    uint32_t ui_current_color = 0xFFFFFFFFu;
    std::shared_ptr<UIDrawCommands> last_ui_draw_commands = std::make_shared<UIDrawCommands>();
    bool last_scene_rendered = false;
    bool update_gamestate = true;
    bool quakespasm_initialized = false;
    uint64_t frame = 0;
    uint64_t last_worldspawn_frame = 0;
    double server_fps = 0;

    float volume_max_t = 1000.F;

    // Static brush world: rebuilt on every worldspawn.
    bool world_meshes_built = false;
    merian::Scene::NodeID world_node_id = merian::Scene::NODE_ID_INVALID;
    std::vector<merian::Scene::MeshID> world_mesh_ids;

    struct BrushMaterialKey {
        texture_t* tex;
        int surf_flags;
        bool two_sided;
        uint8_t light_style;
        uint32_t fixture_tint;
        uint16_t fixture_emission;
        bool animated() const {
            return tex->anim_total > 0 || light_style != 0 ||
                   (surf_flags & (MAT_TYPE_WATER | MAT_TYPE_SLIME)) != 0;
        }
        bool operator==(const BrushMaterialKey& o) const noexcept = default;
    };
    struct BrushMaterialKeyHash {
        size_t operator()(const BrushMaterialKey& k) const noexcept {
            return std::hash<texture_t*>()(k.tex) ^ (std::hash<int>()(k.surf_flags) << 1u) ^
                   (std::hash<bool>()(k.two_sided) << 2u) ^
                   (std::hash<uint8_t>()(k.light_style) << 3u) ^
                   (std::hash<uint32_t>()(k.fixture_tint) << 4u) ^
                   (std::hash<uint16_t>()(k.fixture_emission) << 5u);
        }
    };
    // SURF_PLANEBACK is per-vertex, SURF_DRAWTILED selects r_notexture; the
    // rest carry meaningful material variants.
    static constexpr int SURF_INTERESTING_BITS =
        SURF_DRAWSKY | SURF_DRAWLAVA | SURF_DRAWSLIME | SURF_DRAWTELE | SURF_DRAWWATER;

    struct FaceKey {
        const mplane_t* plane;
        std::vector<unsigned int> vertices;
        bool operator==(const FaceKey& o) const = default;
    };
    struct FaceKeyHash {
        size_t operator()(const FaceKey& k) const noexcept {
            size_t h = std::hash<const mplane_t*>()(k.plane);
            for (const unsigned int v : k.vertices)
                h = h * 31u + v;
            return h;
        }
    };

    struct BrushSurfaceBucket {
        std::vector<merian::PackedVertexData> vertices;
        std::vector<merian::uint3> indices;
    };
    std::unordered_map<BrushMaterialKey, BrushSurfaceBucket, BrushMaterialKeyHash>
    collect_brush_surfaces(qmodel_t* mod);
    QuakeMaterial brush_material(const BrushMaterialKey& key, texture_t* tex, const entity_t* ent);

    // Per-owner so each brush entity's `frame` can resolve independently.
    struct AnimatedBrushMaterial {
        merian::MaterialID material_id;
        BrushMaterialKey key;
    };
    std::vector<AnimatedBrushMaterial> world_animated_materials;
    MapLights map_lights;

    struct LightEntityMaterial {
        merian::MaterialID material_id;
        uint8_t light_style;
    };
    std::vector<LightEntityMaterial> light_entity_materials;

    bool light_sources_changed = false;
    bool light_styles_enabled = true;
    bool fixture_emission_enabled = true;
    bool light_entities_enabled = true;
    float light_entity_radius = 10.f;

    struct Emission {
        float fullbright_scale = 12.79F;
        float fullbright_gamma = 0.4351F;
        float fullbright_exponent = 0.03384F;
        float fullbright_max_level = 0.9484F;
        float fullbright_red = -0.02604F;
        float fullbright_yellow = 1.147F;
        float fullbright_blue = 2.136F;
        float fullbright_fallback = 1.558F;
        float alias_model = 40.F;
        float fixture = 36.32F;
        float waterfall = 23.37F;
        float sprite = 50.F;
        float particle = 50.F;
        float classic_sky_exp_scale = 0.F;
        float classic_sky_exp_rate = 3.5F;
        float classic_sky_pow_scale = 30.78F;
        float classic_sky_pow_gamma = 1.33F;
        float cube_sky_exp_scale = 0.F;
        float cube_sky_exp_rate = 3.5F;
        float cube_sky_pow_scale = 11.98F;
        float cube_sky_pow_gamma = 1.571F;
        float sun_lobe = 1484.F;
        float sun_disc = 270.F;
        float sun_kappa = 30000.F;
        float switched_lights = 109.6F;
    };
    Emission emission;

    // Per-model info built at worldspawn; stable across frames.
    struct AliasModelInfo {
        merian::BufferHandle index_buffer;
        uint32_t vertex_count;
        uint32_t primitive_count;
        int numskins;
        // smooth per-pose normals; MDL's 162-direction quantization is too coarse for AD models
        std::vector<merian::float3> baked_normals;
    };
    std::unordered_map<qmodel_t*, AliasModelInfo> alias_model_info;

    struct BrushSubmodelGeoPart {
        merian::BufferHandle vb;
        merian::BufferHandle ib;
        uint32_t vertex_count;
        uint32_t primitive_count;
        BrushMaterialKey key;
        bool has_alpha;
    };
    std::unordered_map<qmodel_t*, std::vector<BrushSubmodelGeoPart>> brush_submodel_geo;

    struct AliasSkinKey {
        qmodel_t* model;
        int skin;
        uint16_t fixture_emission;
        bool operator==(const AliasSkinKey& o) const = default;
    };
    struct AliasSkinKeyHash {
        size_t operator()(const AliasSkinKey& k) const noexcept {
            return std::hash<qmodel_t*>()(k.model) ^ (std::hash<int>()(k.skin) << 1u) ^
                   (std::hash<uint16_t>()(k.fixture_emission) << 2u);
        }
    };
    std::unordered_map<AliasSkinKey, merian::MaterialID, AliasSkinKeyHash>
        material_id_for_alias_skin;
    std::unordered_map<merian::MaterialID, int> alias_material_frames;
    std::unordered_map<AliasSkinKey, float, AliasSkinKeyHash> alias_projected_areas;
    QuakeMaterial
    alias_material(aliashdr_t* hdr, int skin, int anim_frame, uint16_t fixture_emission) const;

    struct SpriteFrameKey {
        mspriteframe_t* frame;
        uint16_t fixture_emission;
        bool operator==(const SpriteFrameKey& o) const = default;
    };
    struct SpriteFrameKeyHash {
        size_t operator()(const SpriteFrameKey& k) const noexcept {
            return std::hash<mspriteframe_t*>()(k.frame) ^
                   (std::hash<uint16_t>()(k.fixture_emission) << 1u);
        }
    };
    struct SpriteFrameInfo {
        merian::Scene::MeshID mesh_id;
        merian::MaterialID material_id;
    };
    std::unordered_map<SpriteFrameKey, SpriteFrameInfo, SpriteFrameKeyHash> sprite_frame_info;
    std::unordered_map<mspriteframe_t*, float> sprite_projected_areas;
    QuakeMaterial sprite_material(mspriteframe_t* frame) const;
    QuakeMaterial particle_material() const;
    SpriteFrameInfo
    add_sprite_frame(const std::string& name, mspriteframe_t* frame, const QuakeMaterial& material);

    // owns_meshes=false marks sprite slots whose mesh is shared via sprite_frame_info.
    struct EntityMeshSlot {
        merian::Scene::NodeID node_id = merian::Scene::NODE_ID_INVALID;
        merian::SmallVector<merian::Scene::MeshID, 1> mesh_ids;
        // Opaque identity for migration — never dereferenced (may dangle after Mod_ResetAll).
        qmodel_t* model = nullptr;
        bool owns_meshes = true;

        // Empty for non-brush slots.
        std::vector<AnimatedBrushMaterial> animated_materials;

        mspriteframe_t* cached_sprite_frame = nullptr;

        uint8_t cached_alpha = ENTALPHA_DEFAULT;
        uint16_t fixture_emission = 0;
        int cached_skinnum = -1;
        int cached_anim_frame = -1;
        int cached_pose1 = -1;
        int cached_pose2 = -1;
        float cached_blend = -1.f;
        int cached_prev_pose1 = -1;
        int cached_prev_pose2 = -1;
        float cached_prev_blend = -1.f;
        vec3_t cached_origin = {};
        vec3_t cached_angles = {};
    };
    std::unordered_map<entity_t*, EntityMeshSlot> entity_slots;
    // Holds last frame's slots until each surviving entity migrates back; what
    // remains belongs to entities that vanished (server-removed, culled, or
    // temp-entity slots recycled without nulling ent->model — e.g. expired
    // lightning beams) and is destroyed by release_unused_entities.
    std::unordered_map<entity_t*, EntityMeshSlot> previous_entity_slots;

    // Returns nullptr if the previous-frame slot is unusable (caller builds fresh).
    EntityMeshSlot* migrate_entity_slot(entity_t* ent);
    void release_unused_entities();
    void destroy_slot(EntityMeshSlot& slot);

    // Single particle mesh with palette-encoded color. The instance is only
    // attached while extraction yields non-empty geometry — Scene can't
    // upload empty meshes.
    merian::Scene::MeshID particle_mesh_id = 0;
    merian::Scene::NodeID particle_node_id = merian::Scene::NODE_ID_INVALID;
    merian::MaterialID particle_material_id = 0;
    bool particle_instance_attached = false;
    float particle_size = 3.F;
    float particle_opacity = 1.F;

    // Input.
    merian::InputControllerHandle controller = std::make_shared<merian::DummyInputController>();
    merian::WindowHandle window;
    std::shared_ptr<merian::InputListener> input_listener;
    std::optional<bool> input_in_game;
    double mouse_oldx = 0;
    double mouse_oldy = 0;
    double mouse_x = 0;
    double mouse_y = 0;
    bool raw_mouse_was_enabled = false;

    struct InputEvent {
        enum class Type { KEY, CHAR };
        Type type;
        int value;
        bool down;
    };
    std::vector<InputEvent> pending_input;

    // Queued from graph/UI thread, drained by the game thread.
    std::queue<std::string> pending_commands;
    std::mutex pending_commands_mutex;

    // Properties.
    int default_filtering = 0;
    std::string startup_commands{};
    int stop_after_worldspawn = -1;
    bool rebuild_after_stop = true;
    bool overwrite_sun = false;
    merian::float3 overwrite_sun_dir{0, 0, 1};
    merian::float3 overwrite_sun_col{0};
    bool mu_t_s_overwrite = false;
    float mu_t = 0.0F;
    merian::float3 mu_s_div_mu_t{1};
    float fog_particle_size_um = 7.0F;
    float fog_density_factor = 0.5F;
    bool enable_transparency = false;

    merian::Scene::CameraID quake_camera;

    // Per-frame entity counters; newly_created bumps in update_*_entity when a slot is built.
    struct CategoryStats {
        uint32_t active = 0;
        uint32_t newly_created = 0;
    };
    struct EntityStats {
        CategoryStats alias;
        CategoryStats brush;
        CategoryStats sprite;
    };
    EntityStats current_entity_stats{};
    EntityStats last_frame_entity_stats{};
};

using QuakeSceneHandle = std::shared_ptr<QuakeScene>;

} // namespace merian_quake
