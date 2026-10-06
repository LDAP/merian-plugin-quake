#include "game/quake_scene.hpp"

#include "../../res/shader/quake-material.slangh"
#include "game/quake_env_map.hpp"
#include "game/quake_extraction.hpp"
#include "game/quake_material.hpp"
#include "game/quake_meshes.hpp"
#include "merian/utils/audio/audio_device_provider.hpp"
#include "merian/utils/camera/camera.hpp"
#include "merian/utils/colors.hpp"
#include "merian/utils/normal_encoding.hpp"
#include "merian/utils/stopwatch.hpp"
#include "merian/utils/string.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>

extern "C" {
#include "bgmusic.h"
#include "qs_ui_hook.h"
#include "quakedef.h"
#include "screen.h"

extern cvar_t cl_maxpitch;
extern cvar_t cl_minpitch;
extern cvar_t scr_fov;
extern cvar_t cl_gun_fovscale;
extern qboolean scr_drawloading;
}

namespace merian_quake {

namespace {

// Clamp sun radiance to keep it representable in float16 downstream.
constexpr float MAX_SUN_COLOR = 20.f;
constexpr float SUN_LIGHT_SCALE = 4000.f;
constexpr float LIGHT_INTENSITY_SCALE = 1.f / 256;

struct QuakeData {
    QuakeScene* quake_scene{nullptr};
    quakeparms_t params;
    merian::AudioDeviceHandle audio_device;

    // COM_InitArgv keeps the pointers, so the tokens must live as long as the engine.
    std::vector<std::string> argv_tokens;
    std::vector<char*> argv;

    merian::float3 current_sun_color{};
    merian::float3 current_sun_direction{};

    float timediff = 0;
};
QuakeData g_quake_data;

constexpr int DEFAULT_HEAP_SIZE = 1024 * 1024 * 1024;

// startup_commands is tokenized on whitespace into the engine command line
// (e.g. "-game ad +skill 2 +map start"); double quotes keep a token with spaces together and
// lines starting with # are ignored.
void init_quakespasm(const std::string& startup_commands) {
    std::vector<std::string>& tokens = g_quake_data.argv_tokens;
    tokens.assign(1, "quakespasm");
    merian::split(startup_commands, "\n", [&](const std::string& line) {
        if (line.starts_with("#"))
            return;
        for (std::string& token : merian::split_args(line))
            tokens.push_back(std::move(token));
    });
    std::vector<char*>& argv = g_quake_data.argv;
    argv.clear();
    for (std::string& token : tokens)
        argv.push_back(token.data());

    host_parms = &g_quake_data.params;
    g_quake_data.params.argc = static_cast<int>(argv.size());
    g_quake_data.params.argv = argv.data();
    g_quake_data.params.errstate = 0;

    srand(1337);
    COM_InitArgv(g_quake_data.params.argc, g_quake_data.params.argv);
    Sys_Init();

    // -heapsize is in KiB; memsize is int, so an oversized request saturates instead of wrapping.
    int64_t heap_size = DEFAULT_HEAP_SIZE;
    if (const int parm = COM_CheckParm("-heapsize"); parm != 0 && parm + 1 < com_argc) {
        heap_size = std::min<int64_t>(int64_t{Q_atoi(com_argv[parm + 1])} * 1024,
                                      std::numeric_limits<int>::max());
    }
    g_quake_data.params.memsize = static_cast<int>(heap_size);
    g_quake_data.params.membase = malloc(g_quake_data.params.memsize);
    if (g_quake_data.params.membase == nullptr) {
        throw std::runtime_error{fmt::format("could not allocate the {} MiB Quake heap",
                                             g_quake_data.params.memsize / (1024 * 1024))};
    }

    Sys_Printf("Quake %1.2f (c) id Software\n", VERSION);
    Sys_Printf("GLQuake %1.2f (c) id Software\n", GLQUAKE_VERSION);
    Sys_Printf("FitzQuake %1.2f (c) John Fitzgibbons\n", FITZQUAKE_VERSION);
    Sys_Printf("FitzQuake SDL port (c) SleepwalkR, Baker\n");
    Sys_Printf("QuakeSpasm " QUAKESPASM_VER_STRING " (c) Ozkan Sezer, Eric Wasylishen & others\n");

    Host_Init();

    key_dest = key_game;
    m_state = m_none;
}

void shutdown_quakespasm() {
    CL_Disconnect();
    Host_ShutdownServer(false);
    Host_Shutdown();

    free(g_quake_data.params.membase);
}

void parse_worldspawn() {
    merian::float3& quake_sun_col = g_quake_data.current_sun_color;
    merian::float3& quake_sun_dir = g_quake_data.current_sun_direction;

    std::map<std::string, std::string> worldspawn_props;
    char key[128];
    char value[4096];
    const char* data;

    data = COM_Parse(cl.worldmodel->entities);
    if (data == nullptr)
        return;
    if (com_token[0] != '{')
        return;
    while (true) {
        data = COM_Parse(data);
        if (data == nullptr)
            return;
        if (com_token[0] == '}')
            break;
        if (com_token[0] == '_')
            q_strlcpy(key, com_token + 1, sizeof(key));
        else
            q_strlcpy(key, com_token, sizeof(key));
        while ((key[0] != 0) && key[strlen(key) - 1] == ' ')
            key[strlen(key) - 1] = 0;
        data = COM_Parse(data);
        if (data == nullptr)
            return;
        q_strlcpy(value, com_token, sizeof(value));
        SPDLOG_DEBUG("{} {}", key, value);
        worldspawn_props[key] = value;
    }

    quake_sun_col = merian::float3(0);
    if (worldspawn_props.contains("sunlight")) {
        const merian::float3 col = worldspawn_props.contains("sunlight_color")
                                       ? parse_color(worldspawn_props["sunlight_color"].c_str())
                                       : merian::float3(1);
        quake_sun_col = col * std::stof(worldspawn_props["sunlight"]) / SUN_LIGHT_SCALE;
    }

    if (worldspawn_props.contains("sun_mangle")) {
        float angles[3];
        sscanf(worldspawn_props["sun_mangle"].c_str(), "%f %f %f", &angles[1], &angles[0],
               &angles[2]);
        float right[3];
        float up[3];
        angles[1] -= 180;
        AngleVectors(angles, &quake_sun_dir.x, right, up);
    } else {
        quake_sun_dir = merian::float3(1, 1, 1);
    }

    if (merian::yuv_luminance(quake_sun_col) == 0.F) {
        if (const std::optional<SunLight> sun = parse_sun_light(cl.worldmodel->entities)) {
            quake_sun_dir = sun->direction;
            quake_sun_col = sun->color * sun->light / SUN_LIGHT_SCALE;
        }
    }

    const float max_col =
        merian::max(merian::max(quake_sun_col.r, quake_sun_col.g), quake_sun_col.b);
    if (max_col > MAX_SUN_COLOR)
        quake_sun_col = quake_sun_col / max_col * MAX_SUN_COLOR;
    quake_sun_dir = merian::normalize(quake_sun_dir);
}

} // namespace

// --- QuakeSpasm callbacks ---

extern "C" void VID_Changed_f(cvar_t* /*var*/) {
    if (g_quake_data.quake_scene)
        g_quake_data.quake_scene->cb_VID_Changed();
}

extern "C" void QS_worldspawn() {
    if (g_quake_data.quake_scene)
        g_quake_data.quake_scene->cb_QS_worldspawn();
}

extern "C" void QS_texture_load(gltexture_t* glt, uint32_t* data) {
    if (g_quake_data.quake_scene)
        g_quake_data.quake_scene->cb_QS_texture_load(glt, data);
}

extern "C" void IN_Move(usercmd_t* cmd) {
    if (g_quake_data.quake_scene)
        g_quake_data.quake_scene->cb_IN_Move(cmd);
}

extern "C" void R_RenderScene() {
    if (g_quake_data.quake_scene)
        g_quake_data.quake_scene->cb_R_RenderScene();
}

extern "C" void Host_Quit_f() {
    if (key_dest != key_console && cls.state != ca_dedicated) {
        M_Menu_Quit_f();
        return;
    }
    SPDLOG_INFO("quit requested by Quake");
    std::raise(SIGINT);
}

extern "C" void QS_ui_set_canvas(float ortho_l,
                                 float ortho_r,
                                 float ortho_b,
                                 float ortho_t,
                                 int viewport_x,
                                 int viewport_y,
                                 int viewport_w,
                                 int viewport_h) {
    if (g_quake_data.quake_scene)
        g_quake_data.quake_scene->cb_QS_ui_set_canvas(UICanvas{
            ortho_l, ortho_r, ortho_b, ortho_t, viewport_x, viewport_y, viewport_w, viewport_h});
}

extern "C" void QS_ui_set_scissor(int x, int y, int w, int h) {
    if (g_quake_data.quake_scene)
        g_quake_data.quake_scene->cb_QS_ui_set_scissor(UIScissor{x, y, w, h});
}

extern "C" void QS_ui_set_color(uint32_t rgba) {
    if (g_quake_data.quake_scene)
        g_quake_data.quake_scene->cb_QS_ui_set_color(rgba);
}

extern "C" void QS_ui_push_quad(
    int texnum, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    if (g_quake_data.quake_scene)
        g_quake_data.quake_scene->cb_QS_ui_push_quad(
            UIDrawCmd{{x0, y0, x1, y1}, {u0, v0, u1, v1}, texnum});
}

extern "C" void QS_ui_frame_ready() {
    if (g_quake_data.quake_scene)
        g_quake_data.quake_scene->cb_QS_ui_frame_ready();
}

extern "C" qboolean SNDDMA_Init(dma_t* dma) {
    if (!g_quake_data.audio_device)
        return false;

    const auto callback = [](uint8_t* stream, int len) {
        if (!shm) {
            memset(stream, 0, len);
            return;
        }
        int buffersize = shm->samples * (shm->samplebits / 8);
        int pos, tobufend;
        int len1, len2;

        pos = (shm->samplepos * (shm->samplebits / 8));
        if (pos >= buffersize)
            shm->samplepos = pos = 0;
        tobufend = buffersize - pos;
        len1 = len;
        len2 = 0;
        if (len1 > tobufend) {
            len1 = tobufend;
            len2 = len - len1;
        }
        memcpy(stream, shm->buffer + pos, len1);
        if (len2 <= 0) {
            shm->samplepos += (len1 / (shm->samplebits / 8));
        } else {
            memcpy(stream + len1, shm->buffer, len2);
            shm->samplepos = (len2 / (shm->samplebits / 8));
        }
        if (shm->samplepos >= buffersize)
            shm->samplepos = 0;
    };

    merian::AudioDevice::AudioSpec desired = {
        merian::AudioDevice::FORMAT_S16_LSB,
        1024,
        static_cast<int>(snd_mixspeed.value),
        2,
    };
    if (desired.samplerate <= 11025)
        desired.buffersize = 256;
    else if (desired.samplerate <= 22050)
        desired.buffersize = 512;
    else if (desired.samplerate <= 44100)
        desired.buffersize = 1024;
    else if (desired.samplerate <= 56000)
        desired.buffersize = 2048;
    else
        desired.buffersize = 4096;

    auto actual = g_quake_data.audio_device->open_device(desired, callback);
    if (!actual)
        return false;

    memset(static_cast<void*>(dma), 0, sizeof(dma_t));
    shm = dma;
    shm->samplebits = (actual->format & 0xFF);
    shm->signed8 = (actual->format == merian::AudioDevice::FORMAT_S8);
    shm->speed = actual->samplerate;
    shm->channels = actual->channels;
    int tmp = (actual->buffersize * actual->channels) * 10;
    if (tmp & (tmp - 1)) {
        int val = 1;
        while (val < tmp)
            val <<= 1;
        tmp = val;
    }
    shm->samples = tmp;
    shm->samplepos = 0;
    shm->submission_chunk = 1;

    size_t buffersize = shm->samples * (shm->samplebits / 8);
    shm->buffer = static_cast<unsigned char*>(calloc(1, buffersize));

    g_quake_data.audio_device->unpause_audio();
    return 1;
}

extern "C" int SNDDMA_GetDMAPos(void) {
    if (shm != nullptr)
        return shm->samplepos;
    return 0;
}

extern "C" void SNDDMA_Shutdown(void) {
    // the callback reads the buffer until the device is closed
    if (g_quake_data.audio_device)
        g_quake_data.audio_device->close_device();
    if (shm != nullptr) {
        if (shm->buffer != nullptr)
            free(shm->buffer);
        shm->buffer = nullptr;
        shm = nullptr;
    }
    g_quake_data.audio_device.reset();
}

extern "C" void SNDDMA_LockBuffer(void) {
    if (g_quake_data.audio_device)
        g_quake_data.audio_device->lock_device();
}
extern "C" void SNDDMA_Submit(void) {
    if (g_quake_data.audio_device)
        g_quake_data.audio_device->unlock_device();
}
extern "C" void SNDDMA_BlockSound(void) {
    if (g_quake_data.audio_device)
        g_quake_data.audio_device->pause_audio();
}
extern "C" void SNDDMA_UnblockSound(void) {
    if (g_quake_data.audio_device)
        g_quake_data.audio_device->unpause_audio();
}

// --- QuakeScene ---

QuakeScene::QuakeScene(const merian::ShaderCompileContextHandle& compile_context,
                       const merian::ContextHandle& context,
                       const merian::ResourceAllocatorHandle& allocator,
                       const merian::MaterialSystemHandle& material_system)
    : merian::Scene(compile_context, context, allocator, material_system) {

    if (g_quake_data.quake_scene != nullptr) {
        throw std::runtime_error{"only one QuakeScene can exist (Quake uses static globals)"};
    }
    g_quake_data.quake_scene = this;

    // Scene
    const auto& tm = get_texture_manager();

    tm->resize(TEXTURE_CAPACITY);
    quake_material_type_id = material_system->register_material_type(
        QUAKE_MATERIAL_SLANG_TYPE_NAME, QUAKE_MATERIAL_SLANG_MODULE_PATH);
    update_material_constants();
    material_system->set_alpha_test_threshold(0.7F);

    auto cam =
        std::make_shared<merian::Camera>(merian::float3(1, 0, 0), merian::float3(0, 0, 0), get_up(),
                                         merian::radians(60.F), 16.F / 9.F, 0.01F, 1e5f);
    quake_camera = add_camera(std::move(cam));

    // Quake
    if (const auto audio_provider = context->find_provider<merian::AudioDeviceProvider>(true)) {
        g_quake_data.audio_device = audio_provider->create_audio_device();
    } else {
        g_quake_data.audio_device = nullptr;
    }
    // Engine init is deferred to the first on_update so "startup commands" from
    // the graph config can become the engine command line.
}

QuakeScene::~QuakeScene() {
    if (quakespasm_initialized) {
        shutdown_quakespasm();
    }
    g_quake_data.quake_scene = nullptr;
}

float QuakeScene::get_time(const float /*time*/) {
    return static_cast<float>(cl.time);
}

void QuakeScene::set_controller(const merian::InputControllerHandle& controller,
                                const merian::WindowHandle& window) {
    // Drop the old listener so its weak_ptr on the old controller expires.
    input_listener.reset();
    input_in_game.reset();
    this->controller = controller ? controller
                                  : std::static_pointer_cast<merian::InputController>(
                                        std::make_shared<merian::DummyInputController>());
    this->window = window;
    register_input_listener(this->controller);
}

void QuakeScene::queue_command(const std::string& command) {
    std::lock_guard<std::mutex> lock(pending_commands_mutex);
    pending_commands.push(command);
}

void QuakeScene::register_input_listener(const merian::InputControllerHandle& controller) {
    struct QuakeInputListener : merian::InputListener {
        QuakeScene* scene;
        explicit QuakeInputListener(QuakeScene* s) : scene(s) {}

        bool on_key(merian::InputController& /*c*/,
                    merian::InputController::Key key,
                    merian::InputController::KeyStatus action,
                    int /*mods*/) override {
            using K = merian::InputController::Key;
            const int ki = static_cast<int>(key);
            const int A = static_cast<int>(K::A);
            const int N0 = static_cast<int>(K::NUM_0);
            int qkey = 0;
            if (ki >= A && ki <= static_cast<int>(K::Z))
                qkey = 'a' + (ki - A);
            else if (ki >= N0 && ki <= static_cast<int>(K::NUM_9))
                qkey = '0' + (ki - N0);
            else {
                // clang-format off
                static const std::unordered_map<merian::InputController::Key, int> keymap = {
                    {K::TAB,        K_TAB},
                    {K::ENTER,      K_ENTER},
                    {K::ESCAPE,     K_ESCAPE},
                    {K::SPACE,      K_SPACE},
                    {K::BACKSPACE,  K_BACKSPACE},
                    {K::UP,         K_UPARROW},
                    {K::DOWN,       K_DOWNARROW},
                    {K::LEFT,       K_LEFTARROW},
                    {K::RIGHT,      K_RIGHTARROW},
                    {K::LEFT_ALT,   K_ALT},
                    {K::LEFT_CTRL,  K_CTRL},
                    {K::LEFT_SHIFT, K_SHIFT},
                    {K::F1,  K_F1},  {K::F2,  K_F2},  {K::F3,  K_F3},
                    {K::F4,  K_F4},  {K::F5,  K_F5},  {K::F6,  K_F6},
                    {K::F7,  K_F7},  {K::F8,  K_F8},  {K::F9,  K_F9},
                    {K::F10, K_F10}, {K::F11, K_F11}, {K::F12, K_F12},
                };
                // clang-format on
                if (const auto it = keymap.find(key); it != keymap.end())
                    qkey = it->second;
            }
            if (qkey == 0)
                return true;
            using KS = merian::InputController::KeyStatus;
            if (action == KS::PRESS)
                Key_Event(qkey, true);
            else if (action == KS::RELEASE)
                Key_Event(qkey, false);
            return true;
        }

        bool on_cursor(merian::InputController& c, double xpos, double ypos) override {
            const bool raw = c.is_mouse_grabbed();
            if (raw) {
                scene->mouse_x = xpos;
                scene->mouse_y = ypos;
            }
            if (raw != scene->raw_mouse_was_enabled || !raw) {
                scene->mouse_x = scene->mouse_oldx = xpos;
                scene->mouse_y = scene->mouse_oldy = ypos;
            }
            scene->raw_mouse_was_enabled = raw;
            return true;
        }

        bool on_mouse_button(merian::InputController& /*c*/,
                             merian::InputController::MouseButton button,
                             merian::InputController::KeyStatus status) override {
            using MB = merian::InputController::MouseButton;
            using KS = merian::InputController::KeyStatus;
            if (button == MB::UNKNOWN)
                return true;
            const int remap[] = {K_MOUSE1, K_MOUSE2, K_MOUSE3, K_MOUSE4, K_MOUSE5};
            Key_Event(remap[static_cast<int>(button)], status == KS::PRESS);
            return true;
        }

        bool
        on_scroll(merian::InputController& /*c*/, double /*xoffset*/, double yoffset) override {
            if (yoffset > 0) {
                Key_Event(K_MWHEELUP, true);
                Key_Event(K_MWHEELUP, false);
            } else if (yoffset < 0) {
                Key_Event(K_MWHEELDOWN, true);
                Key_Event(K_MWHEELDOWN, false);
            }
            return true;
        }

        bool on_char(merian::InputController& /*c*/, unsigned int codepoint) override {
            if (codepoint >= 32 && codepoint < 127)
                Char_Event(static_cast<int>(codepoint));
            return true;
        }
    };

    input_listener = std::make_shared<QuakeInputListener>(this);
    controller->add_listener(input_listener, 0);
}

// --- QuakeSpasm callback bodies ---

void QuakeScene::cb_VID_Changed() {
    resolution =
        vk::Extent3D{static_cast<uint32_t>(vid.width), static_cast<uint32_t>(vid.height), 1U};
}

void QuakeScene::cb_QS_worldspawn() {
    SPDLOG_DEBUG("worldspawn");
    parse_worldspawn();
    last_worldspawn_frame = frame;

    MERIAN_PROFILE_SCOPE_GPU(active_cmd, "worldspawn");
    key_dest = key_game;
    m_state = m_none;
    sv_player = nullptr;
    unload_world();
    load_world(active_cmd);
}

void QuakeScene::cb_IN_Move(usercmd_t* cmd) {
    SPDLOG_TRACE("move");
    // pretty much a copy from in_sdl.c:

    int dmx = (mouse_x - mouse_oldx) * sensitivity.value;
    int dmy = (mouse_y - mouse_oldy) * sensitivity.value;
    mouse_oldx = mouse_x;
    mouse_oldy = mouse_y;

    if ((in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1)))
        cmd->sidemove += m_side.value * dmx;
    else
        cl.viewangles[YAW] -= m_yaw.value * dmx;

    if (in_mlook.state & 1) {
        if (dmx || dmy)
            V_StopPitchDrift();
    }

    if ((in_mlook.state & 1) && !(in_strafe.state & 1)) {
        cl.viewangles[PITCH] += m_pitch.value * dmy;
        cl.viewangles[PITCH] = std::min(cl.viewangles[PITCH], cl_maxpitch.value);
        cl.viewangles[PITCH] = std::max(cl.viewangles[PITCH], cl_minpitch.value);
    } else {
        if ((in_strafe.state & 1) && noclip_anglehack)
            cmd->upmove -= m_forward.value * dmy;
        else
            cmd->forwardmove -= m_forward.value * dmy;
    }
}

// Fired by R_RenderScene from inside V_RenderView. Extract camera + entity
// state into the scene graph. active_cmd is the cmd buffer of the current
// on_update call.
void QuakeScene::cb_R_RenderScene() {
    last_scene_rendered = scr_drawloading == 0;
    if (!last_scene_rendered)
        return;

    if (cl.worldmodel != nullptr) {
        if (light_sources_changed && world_meshes_built) {
            map_lights.release();
            unload_world();
            load_world(active_cmd);
        }
        light_sources_changed = false;
        R_AnimateLight();
        update_entities(active_cmd);
        update_animated_materials();
    }
    update_camera();
    update_sky();
    update_fog();

    const bool in_game = key_dest == key_game;
    if (in_game != input_in_game) {
        input_in_game = in_game;
        controller->set_mouse_grabbed(in_game);
        if (window) {
            if (in_game)
                window->stop_text_input();
            else
                window->start_text_input();
        }
        if (input_listener)
            controller->add_listener(input_listener, in_game ? 100 : 0);
    }
}

void QuakeScene::cb_QS_ui_set_canvas(const UICanvas& canvas) {
    ui_current_canvas = canvas;
    ui_draw_commands.canvas_events.emplace_back(ui_draw_commands.cmds.size(), canvas);
}

void QuakeScene::cb_QS_ui_set_scissor(const UIScissor& scissor) {
    ui_current_scissor = scissor;
    ui_draw_commands.scissor_events.emplace_back(ui_draw_commands.cmds.size(), scissor);
}

void QuakeScene::cb_QS_ui_set_color(const uint32_t rgba) {
    ui_current_color = rgba;
    ui_draw_commands.color_events.emplace_back(ui_draw_commands.cmds.size(), rgba);
}

void QuakeScene::cb_QS_ui_push_quad(const UIDrawCmd& cmd) {
    ui_draw_commands.cmds.push_back(cmd);
}

void QuakeScene::cb_QS_ui_frame_ready() {
    ui_draw_commands.width = static_cast<uint32_t>(vid.width);
    ui_draw_commands.height = static_cast<uint32_t>(vid.height);
    last_ui_draw_commands = std::make_shared<UIDrawCommands>(std::move(ui_draw_commands));
}

void QuakeScene::cb_QS_texture_load(gltexture_t* glt, const uint32_t* data) {
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
    const std::string source = strcmp(glt->source_file, "") == 0 ? "memory" : glt->source_file;
    SPDLOG_DEBUG("texture_load {} {} {}x{} from {}, frame: {}", glt->texnum, glt->name, glt->width,
                 glt->height, source, glt->visframe);
#endif

    if (glt->width == 0 || glt->height == 0) {
        SPDLOG_WARN("image extent was 0. skipping");
        return;
    }

    const bool linear =
        merian::ends_with(glt->name, "_norm") || merian::ends_with(glt->name, "_gloss");

    vk::Filter mag_filter;
    if (default_filtering == 0) {
        mag_filter =
            ((glt->flags & TEXPREF_LINEAR) != 0u) ? vk::Filter::eLinear : vk::Filter::eNearest;
    } else {
        mag_filter =
            ((glt->flags & TEXPREF_NEAREST) != 0u) ? vk::Filter::eNearest : vk::Filter::eLinear;
    }
    const bool generate_mipmaps = (glt->flags & TEXPREF_MIPMAP) != 0U;

    get_texture_manager()->set_texture_from_rgba8(static_cast<merian::TextureID>(glt->texnum), data,
                                                  glt->width, glt->height,
                                                  vk::SamplerAddressMode::eRepeat, mag_filter,
                                                  vk::Filter::eLinear, !linear, generate_mipmaps);
}

// --- Per-frame ---

void QuakeScene::on_update(const merian::CommandBufferHandle& cmd,
                           const float /*time*/,
                           const float time_diff,
                           const uint32_t /*frame*/) {
    MERIAN_PROFILE_SCOPE_GPU(cmd, "QuakeScene::on_update");

    if (!quakespasm_initialized) {
        init_quakespasm(startup_commands);
        quakespasm_initialized = true;
    }

    if (!update_gamestate)
        return;

    {
        std::lock_guard<std::mutex> lock(pending_commands_mutex);
        while (!pending_commands.empty()) {
            Cmd_ExecuteString(pending_commands.front().c_str(), src_command);
            pending_commands.pop();
        }
    }

    active_cmd = cmd;
    last_scene_rendered = false;
    // SCR_UpdateScreen may be skipped (vid_hidden, throttle) — clear so a
    // skipped frame doesn't leak draws into the next.
    ui_draw_commands.clear();
    Host_Frame(time_diff);
    active_cmd.reset();

    this->frame++;

    if (stop_after_worldspawn >= 0 &&
        (this->frame - last_worldspawn_frame) == static_cast<uint64_t>(stop_after_worldspawn)) {
        update_gamestate = false;
    }
}

void QuakeScene::update_camera() {
    MERIAN_PROFILE_SCOPE("camera");

    const auto cam = get_camera(quake_camera);
    assert(cam);

    float fwd[3];
    float rgt[3];
    float up[3];
    AngleVectors(r_refdef.viewangles, fwd, rgt, up);

    const merian::float3 pos = merian::as_float3(r_refdef.vieworg);
    cam->look_at(pos, pos + merian::float3(fwd[0], fwd[1], fwd[2]),
                 merian::float3(up[0], up[1], up[2]), merian::radians(r_refdef.fov_y));
    if (resolution.width > 0 && resolution.height > 0) {
        cam->set_resolution(resolution);
    } else {
        cam->set_aspect_ratio(16.F / 9.F);
    }
}

namespace {

constexpr float TRANSLUCENT_LIQUID_OPACITY_SCALE = 0.5F;
constexpr float FOG_OPACITY_SCALE = 2.F;
constexpr float WATERFALL_OPACITY_SCALE = 0.25F;

bool is_waterfall(const texture_t* tex) {
    return tex->gltexture != nullptr && strstr(tex->gltexture->name, "wfall") != nullptr;
}

bool is_fog(const texture_t* tex) {
    return tex->gltexture != nullptr && strstr(tex->gltexture->name, "fog") != nullptr;
}

bool is_glass(const texture_t* tex) {
    if (tex->gltexture == nullptr)
        return false;
    const char* name = tex->gltexture->name;
    return strstr(name, "glas") != nullptr || strstr(name, "gls") != nullptr ||
           strstr(name, "window") != nullptr;
}

float brush_opacity(const entity_t* ent, const texture_t* tex, const int surf_flags) {
    const bool liquid = (surf_flags & (SURF_DRAWWATER | SURF_DRAWSLIME)) != 0 || is_waterfall(tex);
    float opacity = 1.F;
    if (ent != nullptr && ent->alpha != ENTALPHA_DEFAULT)
        opacity = ENTALPHA_DECODE(ent->alpha);
    else if ((surf_flags & SURF_DRAWSLIME) != 0)
        opacity = map_slimealpha > 0 ? map_slimealpha : map_wateralpha;
    else if (liquid)
        opacity = map_wateralpha;
    if (opacity >= 1.F)
        return opacity;
    if (is_fog(tex))
        return std::min(opacity * FOG_OPACITY_SCALE, 1.F);
    if (is_waterfall(tex))
        return opacity * TRANSLUCENT_LIQUID_OPACITY_SCALE * WATERFALL_OPACITY_SCALE;
    return liquid ? opacity * TRANSLUCENT_LIQUID_OPACITY_SCALE : opacity;
}

uint8_t brush_pane(const texture_t* tex, const int surf_flags) {
    if ((surf_flags & MAT_TYPE_WARP) != 0)
        return is_waterfall(tex) || is_fog(tex) ? PANE_NONE : PANE_LIQUID;
    return is_glass(tex) ? PANE_GLASS : PANE_NONE;
}

constexpr int NORMAL_LIGHT_STYLE_VALUE = 264;

float light_style_value(const uint8_t light_style) {
    return light_style == 0
               ? 1.f
               : static_cast<float>(d_lightstylevalue[light_style]) / NORMAL_LIGHT_STYLE_VALUE;
}

bool is_static_entity(const entity_t* ent) {
    return std::greater_equal<>()(ent, cl_static_entities) &&
           std::less<>()(ent, cl_static_entities + cl.num_statics);
}

QuakeMaterial as_fixture(QuakeMaterial m, const uint16_t emission) {
    m.payload.surface_flags = MAT_TYPE_FIXTURE;
    m.payload.emission_scale = emission;
    return m;
}

QuakeMaterial make_brush_material(texture_t* tex,
                                  int surf_flags,
                                  const entity_t* ent,
                                  const uint8_t light_style) {
    QuakeMaterial m;
    // Sky brushes carry no surface textures — shading goes through the scene env map.
    if ((surf_flags & MAT_TYPE_SKY) != 0) {
        m.header.alpha_texture_id = QUAKE_NO_TEXTURE;
        m.payload.fullbright_tex = QUAKE_NO_TEXTURE;
        m.payload.normal_tex = QUAKE_NO_TEXTURE;
        m.payload.gloss_tex = QUAKE_NO_TEXTURE;
        m.payload.surface_flags = static_cast<uint16_t>(surf_flags);
        return m;
    }

    m.header.alpha_texture_id = tex->gltexture != nullptr
                                    ? static_cast<merian::TextureID>(tex->gltexture->texnum)
                                    : QUAKE_NO_TEXTURE;
    m.payload.fullbright_tex = tex->fullbright != nullptr
                                   ? static_cast<merian::TextureID>(tex->fullbright->texnum)
                                   : QUAKE_NO_TEXTURE;
    m.payload.normal_tex =
        tex->norm != nullptr ? static_cast<merian::TextureID>(tex->norm->texnum) : QUAKE_NO_TEXTURE;
    m.payload.gloss_tex = tex->gloss != nullptr ? static_cast<merian::TextureID>(tex->gloss->texnum)
                                                : QUAKE_NO_TEXTURE;
    // MAT_TYPE_* alias SURF_DRAW* bits; callers pre-mask with SURF_INTERESTING_BITS.
    m.payload.surface_flags = static_cast<uint16_t>(surf_flags);
    m.payload.pane = brush_pane(tex, surf_flags);
    m.payload.opacity = static_cast<uint8_t>(
        std::lround(std::clamp(brush_opacity(ent, tex, surf_flags), 0.F, 1.F) * 255.F));
    // ad_tears emissive waterfalls
    if (is_waterfall(tex)) {
        m.payload.surface_flags = MAT_TYPE_WATERFALL;
    }

    if (m.payload.surface_flags == MAT_TYPE_TELE && m.payload.fullbright_tex == QUAKE_NO_TEXTURE) {
        m.payload.fullbright_tex = m.header.alpha_texture_id;
    }
    m.payload.emission_scale = merian::half(light_style_value(light_style)).data;
    return m;
}

QuakeMaterial make_alias_material(aliashdr_t* hdr, int skin, int fm = 0) {
    if (hdr->numskins <= 0)
        return {};
    skin = std::clamp(skin, 0, hdr->numskins - 1);
    fm &= 3;
    QuakeMaterial m;
    if (hdr->gltextures[skin][fm] != nullptr)
        m.header.alpha_texture_id =
            static_cast<merian::TextureID>(hdr->gltextures[skin][fm]->texnum);
    if (hdr->fbtextures[skin][fm] != nullptr)
        m.payload.fullbright_tex =
            static_cast<merian::TextureID>(hdr->fbtextures[skin][fm]->texnum);
    if (hdr->nmtextures[skin][fm] != nullptr)
        m.payload.normal_tex = static_cast<merian::TextureID>(hdr->nmtextures[skin][fm]->texnum);
    if (hdr->gstextures[skin][fm] != nullptr)
        m.payload.gloss_tex = static_cast<merian::TextureID>(hdr->gstextures[skin][fm]->texnum);
    m.payload.surface_flags = MAT_TYPE_NONE;
    return m;
}

QuakeMaterial make_sprite_frame_material(mspriteframe_t* frame) {
    QuakeMaterial m;
    if (frame->gltexture != nullptr)
        m.header.alpha_texture_id = static_cast<merian::TextureID>(frame->gltexture->texnum);
    m.payload.surface_flags = MAT_TYPE_NONE;
    m.payload.fullbright_tex = m.header.alpha_texture_id;
    return m;
}

uint32_t pack_unorm8(const merian::float4& v) {
    const auto unorm = [](const float x) {
        return static_cast<uint32_t>(std::lround(std::clamp(x, 0.f, 1.f) * 255));
    };
    return unorm(v.x) | unorm(v.y) << 8u | unorm(v.z) << 16u | unorm(v.w) << 24u;
}

QuakeMaterial make_light_entity_material(const uint8_t light_style) {
    QuakeMaterial m;
    m.payload.fullbright_tex = LIGHT_ENTITY_RADIANCE_TEXTURE;
    m.payload.normal_tex = LIGHT_ENTITY_SPOT_TEXTURE;
    m.payload.surface_flags = MAT_TYPE_LIGHT_ENTITY;
    m.payload.emission_scale = merian::half(light_style_value(light_style)).data;
    return m;
}

// Quake world transform: yaw is flipped relative to the engine convention.
merian::float4x4 entity_transform(entity_t* ent) {
    std::array<float, 3> a = {-ent->angles[0], ent->angles[1], ent->angles[2]};
    merian::float4x4 m = merian::identity();
    AngleVectors(a.data(), &m[0].x, &m[1].x, &m[2].x);
    m[1] *= -1;
    m[3] = merian::float4(ent->origin[0], ent->origin[1], ent->origin[2], 1.f);
    return merian::transpose(m);
}

// fov_scaled stretches the model's Y/Z for the viewent gun at wide FOVs.
merian::float4x4 alias_transform(const float* origin,
                                 const float* angles,
                                 const bool fov_scaled,
                                 const merian::float3& hdr_scale,
                                 const merian::float3& hdr_scale_origin) {
    merian::float3 fovscale(1.f);
    if (fov_scaled) {
        const float t = std::tan(scr_fov.value * static_cast<float>(0.5 * M_PI / 180.0));
        fovscale.y = t;
        fovscale.z = t;
    }
    const merian::float4x4 scale_col = merian::mul(merian::translation(hdr_scale_origin * fovscale),
                                                   merian::scale(hdr_scale * fovscale));

    std::array<float, 3> a = {-angles[0], angles[1], angles[2]};
    merian::float4x4 rt = merian::identity();
    AngleVectors(a.data(), &rt[0].x, &rt[1].x, &rt[2].x);
    rt[1] *= -1;
    rt[3] = merian::float4(origin[0], origin[1], origin[2], 1.f);
    return merian::mul(merian::transpose(rt), scale_col);
}

std::optional<merian::float4x4> sprite_node_transform(entity_t* ent) {
    auto* psprite = (msprite_t*)ent->model->cache.data;
    merian::float3 s_up;
    merian::float3 s_right;
    if (!sprite_world_basis(ent, psprite, s_up, s_right))
        return std::nullopt;
    // Local quads live in the y-z plane; column 0 only needs a sane basis
    // vector for determinant sign.
    const float scale = ENTSCALE_DECODE(ent->scale);
    const merian::float3 n = merian::normalize(merian::cross(s_right, s_up));
    merian::float4x4 m = merian::identity();
    m[0] = merian::float4(n * scale, 0.f);
    m[1] = merian::float4(s_right * scale, 0.f);
    m[2] = merian::float4(s_up * scale, 0.f);
    m[3] = merian::float4(merian::as_float3(ent->origin), 1.f);
    return merian::transpose(m);
}

} // namespace

// --- World lifecycle ---

void QuakeScene::unload_world() {
    if (!world_meshes_built)
        return;

    for (const merian::Scene::MeshID id : world_mesh_ids)
        remove_mesh(id);
    world_mesh_ids.clear();

    for (auto& [_, slot] : entity_slots)
        destroy_slot(slot);
    for (auto& [_, slot] : previous_entity_slots)
        destroy_slot(slot);
    entity_slots.clear();
    previous_entity_slots.clear();

    for (const auto& [_, info] : sprite_frame_info)
        remove_mesh(info.mesh_id);
    sprite_frame_info.clear();
    sprite_projected_areas.clear();

    if (particle_mesh_id != merian::Scene::MeshID{}) {
        remove_mesh(particle_mesh_id);
        particle_mesh_id = merian::Scene::MeshID{};
    }
    if (particle_node_id != merian::Scene::NODE_ID_INVALID) {
        remove_node(particle_node_id);
        particle_node_id = merian::Scene::NODE_ID_INVALID;
    }
    particle_instance_attached = false;
    if (world_node_id != merian::Scene::NODE_ID_INVALID) {
        remove_node(world_node_id);
        world_node_id = merian::Scene::NODE_ID_INVALID;
    }

    material_id_for_alias_skin.clear();
    alias_material_frames.clear();
    alias_projected_areas.clear();
    world_animated_materials.clear();
    light_entity_materials.clear();

    for (auto& [_, info] : alias_model_info)
        defer_buffer_release(std::move(info.index_buffer));
    alias_model_info.clear();

    for (auto& [_, parts] : brush_submodel_geo) {
        for (auto& part : parts) {
            defer_buffer_release(std::move(part.vb));
            defer_buffer_release(std::move(part.ib));
        }
    }
    brush_submodel_geo.clear();

    get_material_system()->clear();

    world_meshes_built = false;
}

void QuakeScene::load_world(const merian::CommandBufferHandle& cmd) {
    {
        MERIAN_PROFILE_SCOPE("upload_palette");
        const auto& tm = get_texture_manager();
        tm->set_texture_from_rgba8(PALETTE_TEXTURE, d_8to24table, 256, 1,
                                   vk::SamplerAddressMode::eClampToEdge, vk::Filter::eNearest,
                                   vk::Filter::eNearest, true, false);
        // fullbright palette hack for rocket trails and explosions
        std::array<uint32_t, 256> fb_palette{};
        std::memcpy(fb_palette.data(), d_8to24table_fbright, sizeof(fb_palette));
        for (uint32_t i = 96; i <= 111; i++)
            fb_palette[i] = d_8to24table[i];
        tm->set_texture_from_rgba8(FULLBRIGHT_PALETTE_TEXTURE, fb_palette.data(), 256, 1,
                                   vk::SamplerAddressMode::eClampToEdge, vk::Filter::eNearest,
                                   vk::Filter::eNearest, true, false);
    }

    {
        MERIAN_PROFILE_SCOPE("load_world_brushes");
        load_world_brushes();
    }
    {
        MERIAN_PROFILE_SCOPE("load_light_entities");
        load_light_entities();
    }
    {
        MERIAN_PROFILE_SCOPE_GPU(cmd, "register_alias_models");
        register_alias_models(cmd);
    }
    {
        MERIAN_PROFILE_SCOPE("register_sprite_models");
        register_sprite_models();
    }
    {
        MERIAN_PROFILE_SCOPE("init_particle_batch");
        init_particle_batch();
    }

    world_meshes_built = true;
}

std::unordered_map<QuakeScene::BrushMaterialKey,
                   QuakeScene::BrushSurfaceBucket,
                   QuakeScene::BrushMaterialKeyHash>
QuakeScene::collect_brush_surfaces(qmodel_t* mod) {
    const bool world_space = mod == cl.worldmodel || mod->name[0] == '*';
    const auto drawn = [&](const msurface_t* surf) {
        return surf->texinfo != nullptr && surf->texinfo->texture != nullptr &&
               strcmp(surf->texinfo->texture->name, "skip") != 0;
    };

    enum class Sides : uint8_t { ONE, TWO, MERGED };
    std::vector<Sides> sides(mod->nummodelsurfaces, Sides::ONE);
    std::unordered_map<FaceKey, int, FaceKeyHash> faces;
    for (int i = 0; i < mod->nummodelsurfaces; i++) {
        const msurface_t* surf = &mod->surfaces[mod->firstmodelsurface + i];
        if (!drawn(surf))
            continue;
        FaceKey face{surf->plane, {}};
        face.vertices.reserve(surf->numedges);
        for (int e = 0; e < surf->numedges; e++) {
            const int edge = mod->surfedges[surf->firstedge + e];
            face.vertices.push_back(edge >= 0 ? mod->edges[edge].v[0] : mod->edges[-edge].v[1]);
        }
        std::ranges::sort(face.vertices);
        const auto [it, inserted] = faces.try_emplace(std::move(face), i);
        if (inserted || sides[it->second] != Sides::ONE)
            continue;
        const msurface_t* other = &mod->surfaces[mod->firstmodelsurface + it->second];
        if (((surf->flags ^ other->flags) & SURF_PLANEBACK) != 0) {
            sides[it->second] = Sides::TWO;
            sides[i] = Sides::MERGED;
        }
    }

    std::unordered_map<BrushMaterialKey, BrushSurfaceBucket, BrushMaterialKeyHash> buckets;
    for (int i = 0; i < mod->nummodelsurfaces; i++) {
        msurface_t* surf = &mod->surfaces[mod->firstmodelsurface + i];
        if (!drawn(surf) || sides[i] == Sides::MERGED)
            continue;
        texture_t* tex = surf->texinfo->texture;

        const MapLights::SurfaceLight surface_light =
            world_space ? map_lights.surface_light(*surf) : MapLights::SurfaceLight{};
        const uint16_t fixture_emission = quantized_fixture_emission(surface_light.radiance);
        const uint32_t fixture_tint = fixture_emission != 0 ? surface_light.tint : 0;
        const uint8_t light_style =
            light_styles_enabled && (fixture_emission != 0 || tex->fullbright != nullptr)
                ? surface_light.style
                : 0;
        auto& bucket = buckets[{tex, surf->flags & SURF_INTERESTING_BITS, sides[i] == Sides::TWO,
                                light_style, fixture_tint, fixture_emission}];

        merian::float3 plane_n = merian::as_float3(surf->plane->normal);
        if ((surf->flags & SURF_PLANEBACK) != 0)
            plane_n = -plane_n;
        const uint32_t enc_n = merian::encode_normal(merian::normalize(plane_n));

        if (const glpoly_t* p = surf->polys; p != nullptr) {
            const uint32_t base = static_cast<uint32_t>(bucket.vertices.size());
            for (int v = 0; v < p->numverts; v++) {
                merian::PackedVertexData pv{};
                pv.position = merian::as_float3(p->verts[v]);
                pv.encoded_normal = enc_n;
                pv.uv = merian::half2(p->verts[v][3], p->verts[v][4]);
                bucket.vertices.push_back(pv);
            }
            // fan triangulation
            for (int v = 2; v < p->numverts; v++) {
                bucket.indices.push_back(merian::uint3(base, base + static_cast<uint32_t>(v) - 1u,
                                                       base + static_cast<uint32_t>(v)));
            }
        }
    }
    return buckets;
}

void QuakeScene::load_world_brushes() {
    if (cl.worldmodel == nullptr)
        return;

    qmodel_t* world = cl.worldmodel;

    map_lights =
        world->bspversion != BSPVERSION_VALVE ? MapLights(world->entities, world) : MapLights{};

    if (world_node_id == merian::Scene::NODE_ID_INVALID) {
        merian::Scene::Node root;
        root.name = "worldspawn";
        world_node_id = add_node(std::move(root));
    }

    auto buckets = collect_brush_surfaces(world);

    const auto& material_system = get_material_system();
    for (auto& [key, bucket] : buckets) {
        if (bucket.indices.empty())
            continue;

        const merian::MaterialID material_id = material_system->add_material(
            quake_material_type_id, brush_material(key, key.tex, nullptr));
        if (key.animated())
            world_animated_materials.push_back({material_id, key});

        auto mesh = std::make_unique<QuakeBrushMesh>();
        mesh->name =
            fmt::format("worldspawn:{}", key.tex->name[0] != 0 ? key.tex->name : "unnamed");
        mesh->material_id = material_id;
        const bool has_alpha =
            key.tex->gltexture != nullptr && (key.tex->gltexture->flags & TEXPREF_ALPHA) != 0u;
        mesh->flags = merian::Scene::MeshFlags::FlipFacing;
        if (!has_alpha) {
            mesh->flags = mesh->flags | merian::Scene::MeshFlags::IsOpaque;
        }
        if (key.two_sided) {
            mesh->flags = mesh->flags | merian::Scene::MeshFlags::TwoSided;
        }
        if ((key.surf_flags & MAT_TYPE_SKY) != 0) {
            mesh->flags = mesh->flags | merian::Scene::MeshFlags::UseEnvMap;
        }
        mesh->instance_mask = to_mask(InstanceMask::WORLD);
        mesh->vertices = std::move(bucket.vertices);
        mesh->indices = std::move(bucket.indices);

        const merian::Scene::MeshID mesh_id = add_mesh(std::move(mesh));
        add_mesh_instance(mesh_id, world_node_id);
        world_mesh_ids.push_back(mesh_id);
    }

    SPDLOG_DEBUG("static world: {} partitions, {} surfaces, {} animated materials", buckets.size(),
                 world->nummodelsurfaces, world_animated_materials.size());
}

merian::FogVolume QuakeScene::get_fog() const {
    merian::FogVolume fog;
    if (mu_t_s_overwrite) {
        fog.mu_t = merian::float3(mu_t);
        fog.albedo = mu_s_div_mu_t;
    } else {
        // Quake authors fog as a density plus an LDR colour. The squared density matches the
        // engine's falloff, the colour exponent the de-gamma the materials use.
        const float density = std::pow(Fog_GetDensity(), 2.F) * fog_density_factor;
        const float* color = Fog_GetColor();
        fog.mu_t = merian::float3(density);
        fog.albedo = merian::float3(std::pow(color[0], 1.F / 1.2F), std::pow(color[1], 1.F / 1.2F),
                                    std::pow(color[2], 1.F / 1.2F));
    }
    fog.particle_size_um = fog_particle_size_um;
    fog.max_distance = volume_max_t;
    return fog;
}

void QuakeScene::update_fog() {
    const merian::FogVolume fog = get_fog();
    // an all-zero extinction compiles the medium out of the renderers instead of scaling by one
    if (fog.mu_t.x == 0.F && fog.mu_t.y == 0.F && fog.mu_t.z == 0.F) {
        set_exterior_volume(std::make_shared<merian::VacuumVolume>());
    } else {
        set_exterior_volume(std::make_shared<merian::FogVolume>(fog));
    }
}

void QuakeScene::update_sky() {
    MERIAN_PROFILE_SCOPE("sky");

    const merian::float3 raw_dir =
        overwrite_sun ? overwrite_sun_dir : g_quake_data.current_sun_direction;
    const merian::float3 sun_dir =
        merian::length(raw_dir) > 0 ? merian::normalize(raw_dir) : raw_dir;
    const merian::float3 sun_col =
        overwrite_sun ? overwrite_sun_col : g_quake_data.current_sun_color;
    const auto tid = [](uint32_t texnum) { return static_cast<merian::TextureID>(texnum); };

    if (skybox_name[0] != 0) {
        std::array<merian::TextureID, 6> faces;
        for (int i = 0; i < 6; ++i) {
            faces[i] = skybox_textures[i] != nullptr ? tid(skybox_textures[i]->texnum) : tid(0);
        }
        auto env = std::make_shared<merian_quake::QuakeCubemapSkyEnvMap>(faces);
        env->set_sun(sun_dir, sun_col);
        set_env(env);
    } else if (solidskytexture != nullptr) {
        const merian::TextureID solid = tid(solidskytexture->texnum);
        const merian::TextureID alpha =
            alphaskytexture != nullptr ? tid(alphaskytexture->texnum) : tid(0);
        auto env = std::make_shared<merian_quake::QuakeClassicSkyEnvMap>(solid, alpha);
        env->set_sun(sun_dir, sun_col);
        set_env(env);
    } else {
        set_env(std::make_shared<merian::EmptyEnvMap>());
    }
}

void QuakeScene::register_alias_models(const merian::CommandBufferHandle& cmd) {
    const auto& ms = get_material_system();
    const auto& alloc = get_allocator();
    const auto buf_usage = vk::BufferUsageFlagBits::eStorageBuffer |
                           vk::BufferUsageFlagBits::eTransferSrc |
                           vk::BufferUsageFlagBits::eTransferDst |
                           vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR |
                           vk::BufferUsageFlagBits::eShaderDeviceAddress;

    // [0] is the worldmodel, handled separately.
    for (int i = 1; i < MAX_MODELS; i++) {
        qmodel_t* mod = cl.model_precache[i];
        if (mod == nullptr)
            break;
        if (mod->type != mod_alias)
            continue;

        auto* hdr = (aliashdr_t*)Mod_Extradata(mod);
        if (hdr == nullptr)
            continue;

        const auto* indexes = (const int16_t*)((uint8_t*)hdr + hdr->indexes);
        const uint32_t prim_count = static_cast<uint32_t>(hdr->numindexes / 3);
        const uint32_t vert_count = static_cast<uint32_t>(hdr->numverts_vbo);

        // mdl indices are non-negative int16: same bit pattern as uint16.
        merian::BufferHandle ib = alloc->create_buffer(
            cmd, sizeof(int16_t) * prim_count * 3, buf_usage, indexes,
            merian::MemoryMappingType::NONE, fmt::format("alias_ib:{}", mod->name));

        alias_model_info[mod] = AliasModelInfo{std::move(ib), vert_count, prim_count, hdr->numskins,
                                               bake_alias_pose_normals(hdr)};

        for (int s = 0; s < hdr->numskins; s++) {
            material_id_for_alias_skin[{mod, s, 0}] =
                ms->add_material(quake_material_type_id, make_alias_material(hdr, s));
        }
    }
}

void QuakeScene::register_sprite_models() {
    const auto register_frame = [&](qmodel_t* mod, mspriteframe_t* frame, int debug_idx) {
        if (frame != nullptr && !sprite_frame_info.contains({frame, 0}))
            sprite_frame_info[{frame, 0}] =
                add_sprite_frame(fmt::format("sprite:{}:{}", mod->name, debug_idx), frame,
                                 make_sprite_frame_material(frame));
    };

    for (int i = 1; i < MAX_MODELS; i++) {
        qmodel_t* mod = cl.model_precache[i];
        if (mod == nullptr)
            break;
        if (mod->type != mod_sprite)
            continue;
        auto* spr = (msprite_t*)mod->cache.data;
        if (spr == nullptr)
            continue;
        for (int f = 0; f < spr->numframes; f++) {
            if (spr->frames[f].type == SPR_SINGLE) {
                register_frame(mod, spr->frames[f].frameptr, f);
            } else {
                auto* group = (mspritegroup_t*)spr->frames[f].frameptr;
                if (group == nullptr)
                    continue;
                for (int g = 0; g < group->numframes; g++)
                    register_frame(mod, group->frames[g], (f << 8) | g);
            }
        }
    }
}

QuakeScene::SpriteFrameInfo QuakeScene::add_sprite_frame(const std::string& name,
                                                         mspriteframe_t* frame,
                                                         const QuakeMaterial& material) {
    const merian::MaterialID material_id =
        get_material_system()->add_material(quake_material_type_id, material);
    auto sprite_mesh = std::make_unique<QuakeSpriteFrameMesh>();
    sprite_mesh->name = name;
    sprite_mesh->material_id = material_id;
    sprite_mesh->flags = merian::Scene::MeshFlags::TwoSided;
    sprite_mesh->instance_mask = to_mask(InstanceMask::SPRITE);

    const uint32_t enc_n = merian::encode_normal(merian::float3(1, 0, 0));
    const float smax = frame->smax;
    const float tmax = frame->tmax;
    const auto push = [&](float y, float z, float u, float v) {
        merian::PackedVertexData pv{};
        pv.position = merian::float3(0.f, y, z);
        pv.encoded_normal = enc_n;
        pv.uv = merian::half2(u, v);
        sprite_mesh->vertices.push_back(pv);
    };
    push(frame->left, frame->down, 0.f, tmax);
    push(frame->left, frame->up, 0.f, 0.f);
    push(frame->right, frame->up, smax, 0.f);
    push(frame->left, frame->down, 0.f, tmax);
    push(frame->right, frame->up, smax, 0.f);
    push(frame->right, frame->down, smax, tmax);

    return SpriteFrameInfo{add_mesh(std::move(sprite_mesh)), material_id};
}

void QuakeScene::init_particle_batch() {
    QuakeMaterial particle_mat;
    particle_mat.header.alpha_texture_id = PALETTE_TEXTURE;
    particle_mat.payload.fullbright_tex = FULLBRIGHT_PALETTE_TEXTURE;
    particle_mat.payload.surface_flags = MAT_TYPE_NONE;
    particle_material_id =
        get_material_system()->add_material(quake_material_type_id, particle_mat);

    merian::Scene::Node node;
    node.name = "particles";
    particle_node_id = add_node(std::move(node));

    auto mesh = std::make_unique<QuakeHostDynamicMesh>();
    mesh->name = "particles";
    mesh->material_id = particle_material_id;
    mesh->flags = merian::Scene::MeshFlags::IsMorphed |
                  merian::Scene::MeshFlags::HasVariableTopology |
                  merian::Scene::MeshFlags::FlipFacing;
    mesh->instance_mask = to_mask(InstanceMask::PARTICLE);
    particle_mesh_id = add_mesh(std::move(mesh));
    // update_particles attaches the instance lazily on first non-empty extraction.
    particle_instance_attached = false;
}

// --- Per-entity slot management ---

void QuakeScene::destroy_slot(EntityMeshSlot& slot) {
    if (slot.owns_meshes) {
        for (const merian::Scene::MeshID id : slot.mesh_ids)
            remove_mesh(id);
    } else if (slot.node_id != merian::Scene::NODE_ID_INVALID) {
        for (const merian::Scene::MeshID id : slot.mesh_ids)
            remove_mesh_instance(id, slot.node_id);
    }
    if (slot.node_id != merian::Scene::NODE_ID_INVALID)
        remove_node(slot.node_id);
}

QuakeScene::EntityMeshSlot* QuakeScene::migrate_entity_slot(entity_t* ent) {
    auto node = previous_entity_slots.extract(ent);
    if (node.empty())
        return nullptr;
    // Same model pointer implies same type — the model identifies the kind.
    if (node.mapped().model != ent->model) {
        destroy_slot(node.mapped());
        return nullptr;
    }
    auto inserted = entity_slots.insert(std::move(node));
    return &inserted.position->second;
}

void QuakeScene::release_unused_entities() {
    for (auto& [_, slot] : previous_entity_slots)
        destroy_slot(slot);
    previous_entity_slots.clear();
}

void QuakeScene::update_alias_entity(entity_t* ent,
                                     const merian::CommandBufferHandle& /*cmd*/,
                                     const uint8_t instance_mask) {
    const auto info_it = alias_model_info.find(ent->model);
    if (info_it == alias_model_info.end() || info_it->second.numskins <= 0)
        return;
    auto* hdr = static_cast<aliashdr_t*>(Mod_Extradata(ent->model));

    EntityMeshSlot* slot = migrate_entity_slot(ent);
    if (slot == nullptr) {
        const AliasModelInfo& info = info_it->second;
        const auto& alloc = get_allocator();
        const vk::DeviceSize vb_size = info.vertex_count * sizeof(merian::PackedVertexData);
        const vk::DeviceSize prev_vb_size =
            info.vertex_count * sizeof(merian::PackedPrevVertexData);
        const auto staging_usage = vk::BufferUsageFlagBits::eTransferSrc |
                                   vk::BufferUsageFlagBits::eStorageBuffer |
                                   vk::BufferUsageFlagBits::eShaderDeviceAddress;

        auto vb = alloc->create_buffer(vb_size, staging_usage,
                                       merian::MemoryMappingType::HOST_ACCESS_SEQUENTIAL_WRITE,
                                       fmt::format("alias_vb:{}", ent->model->name));
        auto prev_vb = alloc->create_buffer(prev_vb_size, staging_usage,
                                            merian::MemoryMappingType::HOST_ACCESS_SEQUENTIAL_WRITE,
                                            fmt::format("alias_prev_vb:{}", ent->model->name));

        const int skin = std::clamp(ent->skinnum, 0, info.numskins - 1);
        const uint16_t fixture_emission = model_fixture_emission(ent, hdr, skin);
        const merian::MaterialID material_id =
            ensure_alias_material(ent->model, hdr, skin, fixture_emission);

        merian::Scene::Node node;
        node.name = fmt::format("alias:{}", ent->model->name);
        node.is_animated = true;
        const merian::Scene::NodeID node_id = add_node(std::move(node));

        auto mesh = std::make_unique<AliasInstanceMesh>();
        mesh->name = fmt::format("alias:{}", ent->model->name);
        mesh->material_id = material_id;
        mesh->flags = merian::Scene::MeshFlags::IsMorphed | merian::Scene::MeshFlags::FlipFacing;
        mesh->instance_mask = instance_mask;
        mesh->vb_staging = std::move(vb);
        mesh->prev_vb_staging = std::move(prev_vb);
        mesh->vb_mapped = mesh->vb_staging->get_memory()->map_as<merian::PackedVertexData>();
        mesh->prev_vb_mapped =
            mesh->prev_vb_staging->get_memory()->map_as<merian::PackedPrevVertexData>();
        mesh->ib_shared = info.index_buffer;
        mesh->vertex_count = info.vertex_count;
        mesh->primitive_count = info.primitive_count;

        const merian::Scene::MeshID mesh_id = add_mesh(std::move(mesh));
        add_mesh_instance(mesh_id, node_id);

        EntityMeshSlot fresh;
        fresh.node_id = node_id;
        fresh.mesh_ids = {mesh_id};
        fresh.model = ent->model;
        fresh.fixture_emission = fixture_emission;
        auto [it, _] = entity_slots.emplace(ent, std::move(fresh));
        slot = &it->second;
        current_entity_stats.alias.newly_created++;
    }
    current_entity_stats.alias.active++;

    auto& mesh = static_cast<AliasInstanceMesh&>(*get_mesh_infos()[slot->mesh_ids[0]].mesh);

    if (hdr->numskins > 0) {
        const int skin = std::clamp(ent->skinnum, 0, hdr->numskins - 1);
        const int anim_frame = static_cast<int>(cl.time * 10) & 3;
        if (ent->skinnum != slot->cached_skinnum || anim_frame != slot->cached_anim_frame) {
            if (ent->skinnum != slot->cached_skinnum)
                mesh.material_id =
                    ensure_alias_material(ent->model, hdr, skin, slot->fixture_emission);
            if (const auto [it, inserted] =
                    alias_material_frames.try_emplace(mesh.material_id, anim_frame);
                inserted || it->second != anim_frame) {
                it->second = anim_frame;
                const QuakeMaterial material = make_alias_material(hdr, skin, anim_frame);
                get_material_system()->update_material(
                    mesh.material_id, slot->fixture_emission != 0
                                          ? as_fixture(material, slot->fixture_emission)
                                          : material);
            }
            slot->cached_skinnum = ent->skinnum;
            slot->cached_anim_frame = anim_frame;
        }
    }

    lerpdata_t lerpdata;
    R_SetupAliasFrame(ent, hdr, ent->frame, &lerpdata);
    R_SetupEntityTransform(ent, &lerpdata);

    const int prev_pose1 = slot->cached_pose1 >= 0 ? slot->cached_pose1 : lerpdata.pose1;
    const int prev_pose2 = slot->cached_pose2 >= 0 ? slot->cached_pose2 : lerpdata.pose2;
    const float prev_blend = slot->cached_blend >= 0.f ? slot->cached_blend : lerpdata.blend;

    const bool pose_changed =
        lerpdata.pose1 != slot->cached_pose1 || lerpdata.pose2 != slot->cached_pose2 ||
        lerpdata.blend != slot->cached_blend || prev_pose1 != slot->cached_prev_pose1 ||
        prev_pose2 != slot->cached_prev_pose2 || prev_blend != slot->cached_prev_blend;

    if (pose_changed) {
        lerp_alias_vertices(hdr, info_it->second.baked_normals.data(), lerpdata.pose1,
                            lerpdata.pose2, lerpdata.blend, prev_pose1, prev_pose2, prev_blend,
                            mesh.vb_mapped, mesh.prev_vb_mapped);
        slot->cached_pose1 = lerpdata.pose1;
        slot->cached_pose2 = lerpdata.pose2;
        slot->cached_blend = lerpdata.blend;
        slot->cached_prev_pose1 = prev_pose1;
        slot->cached_prev_pose2 = prev_pose2;
        slot->cached_prev_blend = prev_blend;
        mesh.vertices_dirty = true;
    }

    if ((VectorCompare(lerpdata.origin, slot->cached_origin) == 0) ||
        (VectorCompare(lerpdata.angles, slot->cached_angles) == 0)) {
        const bool fov_scaled =
            ent == &cl.viewent && scr_fov.value > 90.f && cl_gun_fovscale.value != 0.f;
        update_node(slot->node_id, alias_transform(lerpdata.origin, lerpdata.angles, fov_scaled,
                                                   merian::as_float3(hdr->scale),
                                                   merian::as_float3(hdr->scale_origin)));
        VectorCopy(lerpdata.origin, slot->cached_origin);
        VectorCopy(lerpdata.angles, slot->cached_angles);
    }
}

void QuakeScene::update_brush_entity(entity_t* ent,
                                     const merian::CommandBufferHandle& cmd,
                                     const uint8_t instance_mask) {
    // lazy-build submodel geometry on first reference
    auto geo_it = brush_submodel_geo.find(ent->model);
    if (geo_it == brush_submodel_geo.end()) {
        const auto& alloc = get_allocator();
        const auto buf_usage =
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
            vk::BufferUsageFlagBits::eTransferDst |
            vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR |
            vk::BufferUsageFlagBits::eShaderDeviceAddress;

        auto buckets = collect_brush_surfaces(ent->model);
        auto& parts = brush_submodel_geo[ent->model];
        for (auto& [key, bucket] : buckets) {
            if (bucket.indices.empty())
                continue;
            auto vb = alloc->create_buffer(cmd, bucket.vertices, buf_usage,
                                           fmt::format("brush_vb:{}", ent->model->name));
            auto ib = alloc->create_buffer(cmd, bucket.indices, buf_usage,
                                           fmt::format("brush_ib:{}", ent->model->name));
            const bool has_alpha =
                key.tex->gltexture != nullptr && (key.tex->gltexture->flags & TEXPREF_ALPHA) != 0u;
            parts.push_back({std::move(vb), std::move(ib),
                             static_cast<uint32_t>(bucket.vertices.size()),
                             static_cast<uint32_t>(bucket.indices.size()), key, has_alpha});
        }
        geo_it = brush_submodel_geo.find(ent->model);
    }
    if (geo_it->second.empty())
        return;

    EntityMeshSlot* slot = migrate_entity_slot(ent);
    if (slot == nullptr) {
        merian::Scene::Node node;
        node.name = fmt::format("brush:{}", ent->model->name);
        node.is_animated = true;
        const merian::Scene::NodeID node_id = add_node(std::move(node));

        EntityMeshSlot fresh;
        fresh.node_id = node_id;
        fresh.model = ent->model;

        const auto& material_system = get_material_system();
        for (const auto& part : geo_it->second) {
            const merian::MaterialID material_id = material_system->add_material(
                quake_material_type_id, brush_material(part.key, part.key.tex, ent));
            if (part.key.animated())
                fresh.animated_materials.push_back({material_id, part.key});

            auto mesh = std::make_unique<BrushEntityMesh>();
            mesh->name = fmt::format("brush:{}:{}", ent->model->name, material_id);
            mesh->material_id = material_id;
            mesh->flags = merian::Scene::MeshFlags::FlipFacing;
            if (!part.has_alpha)
                mesh->flags = mesh->flags | merian::Scene::MeshFlags::IsOpaque;
            if (part.key.two_sided)
                mesh->flags = mesh->flags | merian::Scene::MeshFlags::TwoSided;
            if ((part.key.surf_flags & MAT_TYPE_SKY) != 0)
                mesh->flags = mesh->flags | merian::Scene::MeshFlags::UseEnvMap;
            mesh->instance_mask = instance_mask;
            mesh->vb = part.vb;
            mesh->ib = part.ib;
            mesh->vertex_count = part.vertex_count;
            mesh->primitive_count = part.primitive_count;
            const merian::Scene::MeshID mesh_id = add_mesh(std::move(mesh));
            add_mesh_instance(mesh_id, node_id);
            fresh.mesh_ids.push_back(mesh_id);
        }
        fresh.cached_alpha = ent->alpha;

        auto [it, _] = entity_slots.emplace(ent, std::move(fresh));
        slot = &it->second;
        current_entity_stats.brush.newly_created++;
    }
    current_entity_stats.brush.active++;

    if (ent->alpha != slot->cached_alpha) {
        const auto& parts = geo_it->second;
        for (size_t i = 0; i < parts.size(); i++) {
            get_material_system()->update_material(
                get_mesh_infos()[slot->mesh_ids[i]].mesh->material_id,
                brush_material(parts[i].key, R_TextureAnimation(parts[i].key.tex, ent->frame),
                               ent));
        }
        slot->cached_alpha = ent->alpha;
    }

    update_node(slot->node_id, entity_transform(ent));
}

void QuakeScene::update_sprite_entity(entity_t* ent) {
    mspriteframe_t* frame = R_GetSpriteFrame(ent);
    if (!sprite_frame_info.contains({frame, 0}))
        return;

    EntityMeshSlot* slot = migrate_entity_slot(ent);
    if (slot == nullptr) {
        const uint16_t emission = sprite_fixture_emission(ent, frame);
        const merian::Scene::MeshID mesh_id = ensure_sprite_frame(ent->model, frame, emission);
        merian::Scene::Node node;
        node.name = fmt::format("sprite:{}", ent->model->name);
        node.is_animated = true;
        const merian::Scene::NodeID node_id = add_node(std::move(node));
        add_mesh_instance(mesh_id, node_id);

        EntityMeshSlot fresh;
        fresh.node_id = node_id;
        fresh.mesh_ids = {mesh_id};
        fresh.model = ent->model;
        fresh.owns_meshes = false;
        fresh.fixture_emission = emission;
        fresh.cached_sprite_frame = frame;
        auto [it, _] = entity_slots.emplace(ent, std::move(fresh));
        slot = &it->second;
        current_entity_stats.sprite.newly_created++;
    } else if (frame != slot->cached_sprite_frame) {
        const merian::Scene::MeshID mesh_id =
            ensure_sprite_frame(ent->model, frame, slot->fixture_emission);
        remove_mesh_instance(slot->mesh_ids[0], slot->node_id);
        add_mesh_instance(mesh_id, slot->node_id);
        slot->mesh_ids[0] = mesh_id;
        slot->cached_sprite_frame = frame;
    }
    current_entity_stats.sprite.active++;

    if (const auto transform = sprite_node_transform(ent)) {
        update_node(slot->node_id, *transform);
        VectorCopy(ent->origin, ent->mv_prev_origin);
    }
}

void QuakeScene::update_entities(const merian::CommandBufferHandle& cmd) {
    MERIAN_PROFILE_SCOPE_GPU(cmd, "update_entities");

    previous_entity_slots = std::move(entity_slots);
    entity_slots.clear();
    current_entity_stats = {};

    // mask_override != 0 forces a specific bit (viewent / player body);
    // otherwise the bit is derived from ent->model->type.
    const auto visit = [&](entity_t* ent, uint8_t mask_override) {
        if (ent == nullptr || ent->model == nullptr)
            return;
        switch (ent->model->type) {
        case mod_alias:
            update_alias_entity(ent, cmd,
                                mask_override != 0 ? mask_override : to_mask(InstanceMask::ALIAS));
            break;
        case mod_brush:
            update_brush_entity(
                ent, cmd, mask_override != 0 ? mask_override : to_mask(InstanceMask::BRUSH_ENTITY));
            break;
        case mod_sprite:
            // sprite mask is baked on the shared frame mesh at register time
            update_sprite_entity(ent);
            break;
        default:
            break;
        }
    };

    // gbuffer toggles between viewent (gun) and player body via the trace mask.
    visit(&cl.viewent, to_mask(InstanceMask::VIEWENT));
    if (cl.viewentity > 0 && cl.viewentity < cl_max_edicts && cl_entities != nullptr)
        visit(&cl_entities[cl.viewentity], to_mask(InstanceMask::PLAYER_BODY));
    for (int i = 0; i < cl_numvisedicts; i++)
        visit(cl_visedicts[i], 0);
    for (int i = 0; i < cl.num_statics; i++)
        visit(&cl_static_entities[i], 0);

    release_unused_entities();
    update_particles();

    last_frame_entity_stats = current_entity_stats;
}

void QuakeScene::update_particles() {
    auto& mesh = static_cast<QuakeHostDynamicMesh&>(*get_mesh_infos()[particle_mesh_id].mesh);
    mesh.vertices.clear();
    mesh.prev_vertices.clear();
    mesh.indices.clear();

    std::vector<merian::float3> prev_pos;
    extract_particle_geo(mesh.vertices, prev_pos, mesh.indices, reproducible_renders, prev_cl_time);
    prev_cl_time = cl.time;

    mesh.prev_vertices.resize(prev_pos.size());
    for (size_t i = 0; i < prev_pos.size(); i++)
        mesh.prev_vertices[i].position = prev_pos[i];

    // Scene can't upload empty meshes — detach the instance so it's skipped.
    const bool has_particles = !mesh.vertices.empty();
    if (has_particles && !particle_instance_attached) {
        add_mesh_instance(particle_mesh_id, particle_node_id);
        particle_instance_attached = true;
    } else if (!has_particles && particle_instance_attached) {
        remove_mesh_instance(particle_mesh_id, particle_node_id);
        particle_instance_attached = false;
    }
    if (has_particles) {
        mesh.vertices_dirty = true;
        mesh.indices_dirty = true;
    }
}

uint16_t QuakeScene::quantized_fixture_emission(const float radiance) const {
    const float scale = std::min(LIGHT_INTENSITY_SCALE * radiance, QUAKE_MAX_RADIANCE);
    if (!fixture_emission_enabled || !(scale > 0))
        return 0;
    return merian::half(std::exp2(std::round(4 * std::log2(scale)) / 4)).data;
}

uint16_t QuakeScene::model_fixture_emission(const entity_t* ent, aliashdr_t* hdr, const int skin) {
    if (!is_static_entity(ent) || hdr->fbtextures[skin][0] == nullptr)
        return 0;
    const float intensity = map_lights.model_intensity(merian::as_float3(ent->origin));
    if (intensity <= 0)
        return 0;
    const auto [it, inserted] = alias_projected_areas.try_emplace({ent->model, skin, 0}, 0.f);
    if (inserted)
        it->second = model_projected_area(hdr, skin);
    return it->second > 0 ? quantized_fixture_emission(intensity / it->second) : 0;
}

uint16_t QuakeScene::sprite_fixture_emission(const entity_t* ent, mspriteframe_t* frame) {
    if (!is_static_entity(ent))
        return 0;
    const float intensity = map_lights.model_intensity(merian::as_float3(ent->origin));
    if (intensity <= 0)
        return 0;
    const auto [it, inserted] = sprite_projected_areas.try_emplace(frame, 0.f);
    if (inserted)
        it->second = sprite_projected_area(frame);
    return it->second > 0 ? quantized_fixture_emission(intensity / it->second) : 0;
}

merian::MaterialID QuakeScene::ensure_alias_material(qmodel_t* model,
                                                     aliashdr_t* hdr,
                                                     const int skin,
                                                     const uint16_t fixture_emission) {
    const auto [it, inserted] = material_id_for_alias_skin.try_emplace(
        {model, skin, fixture_emission}, merian::MaterialID{});
    if (inserted) {
        const QuakeMaterial material = make_alias_material(hdr, skin);
        it->second = get_material_system()->add_material(
            quake_material_type_id,
            fixture_emission != 0 ? as_fixture(material, fixture_emission) : material);
    }
    return it->second;
}

merian::Scene::MeshID QuakeScene::ensure_sprite_frame(const qmodel_t* model,
                                                      mspriteframe_t* frame,
                                                      const uint16_t fixture_emission) {
    const auto [it, inserted] = sprite_frame_info.try_emplace({frame, fixture_emission});
    if (inserted)
        it->second =
            add_sprite_frame(fmt::format("fixture sprite:{}", model->name), frame,
                             as_fixture(make_sprite_frame_material(frame), fixture_emission));
    return it->second.mesh_id;
}

void QuakeScene::update_material_constants() {
    const Emission& e = emission;
    std::string constants = fmt::format("export static const bool quake_enable_transparency = {};",
                                        enable_transparency ? "true" : "false");
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, float>>{
             {"fullbright_scale", e.fullbright_scale},
             {"fullbright_gamma", e.fullbright_gamma},
             {"fullbright_exponent", e.fullbright_exponent},
             {"fullbright_max_level", e.fullbright_max_level},
             {"fullbright_red", e.fullbright_red},
             {"fullbright_yellow", e.fullbright_yellow},
             {"fullbright_blue", e.fullbright_blue},
             {"waterfall_emission", e.waterfall},
             {"fixture", e.fixture},
             {"switched_lights", e.switched_lights},
             {"classic_sky_exp_scale", e.classic_sky_exp_scale},
             {"classic_sky_exp_rate", e.classic_sky_exp_rate},
             {"classic_sky_pow_scale", e.classic_sky_pow_scale},
             {"classic_sky_pow_gamma", e.classic_sky_pow_gamma},
             {"cube_sky_exp_scale", e.cube_sky_exp_scale},
             {"cube_sky_exp_rate", e.cube_sky_exp_rate},
             {"cube_sky_pow_scale", e.cube_sky_pow_scale},
             {"cube_sky_pow_gamma", e.cube_sky_pow_gamma},
             {"sun_lobe", e.sun_lobe},
             {"sun_disc", e.sun_disc},
             {"sun_kappa", e.sun_kappa},
         })
        constants += fmt::format(" export static const float quake_{} = {:.9g};", name, value);
    get_material_system()->get_composition()->add_module_from_string(
        "quake_material_constants", fmt::format("namespace merian {{ {} }}", constants));
}

QuakeMaterial
QuakeScene::brush_material(const BrushMaterialKey& key, texture_t* tex, const entity_t* ent) {
    QuakeMaterial m = make_brush_material(tex, key.surf_flags, ent, key.light_style);
    if (key.fixture_emission == 0 || brush_opacity(ent, tex, key.surf_flags) < 1)
        return m;
    m.payload.fullbright_tex = static_cast<merian::TextureID>(
        map_lights.ensure_emission_texture(tex, key.fixture_tint)->texnum);
    merian::half fixture_emission;
    fixture_emission.data = key.fixture_emission;
    return as_fixture(
        m, merian::half(static_cast<float>(fixture_emission) * light_style_value(key.light_style))
               .data);
}

void QuakeScene::load_light_entities() {
    if (!light_entities_enabled || world_node_id == merian::Scene::NODE_ID_INVALID)
        return;
    const std::vector<SwitchedEmitter> emitters = map_lights.switched_emitters(light_entity_radius);
    if (emitters.empty())
        return;

    const uint32_t width = static_cast<uint32_t>(std::ceil(std::sqrt(emitters.size())));
    const uint32_t height = static_cast<uint32_t>((emitters.size() + width - 1) / width);
    std::vector<uint32_t> spot_texels(static_cast<size_t>(width) * height);
    std::vector<uint32_t> radiance_texels(spot_texels.size());
    std::unordered_map<uint8_t, std::unique_ptr<QuakeBrushMesh>> meshes;
    for (size_t i = 0; i < emitters.size(); i++) {
        const LightEntity& light = map_lights.get_lights()[emitters[i].light_index];
        const merian::float3 intensity = LIGHT_INTENSITY_SCALE * emitters[i].intensity;
        const float max_intensity = std::max({intensity.r, intensity.g, intensity.b, 1e-6f});
        const float unit_area = light_emitter_area(light, 1.f);
        const float radius = std::max(
            light.deviance > 0 ? light.deviance : light_entity_radius,
            std::sqrt(emission.switched_lights * max_intensity / (QUAKE_MAX_RADIANCE * unit_area)));
        const float radiance = max_intensity / (unit_area * radius * radius);
        spot_texels[i] = pack_unorm8(
            merian::float4(light.spot_direction * 0.5f + 0.5f, light.spot_half_angle / 90));
        radiance_texels[i] = pack_unorm8(
            merian::float4(intensity / max_intensity,
                           std::log2(radiance) / LIGHT_ENTITY_LOG2_RADIANCE_RANGE + 0.5f));

        auto& mesh = meshes[light_styles_enabled ? light.style : static_cast<uint8_t>(0)];
        if (!mesh)
            mesh = std::make_unique<QuakeBrushMesh>();
        const merian::float2 uv((static_cast<float>(i % width) + 0.5f) / static_cast<float>(width),
                                (static_cast<float>(i / width) + 0.5f) /
                                    static_cast<float>(height));
        append_light_emitter(mesh->vertices, mesh->indices, light, radius, uv);
    }
    for (const auto& [texture, texels] :
         {std::pair{LIGHT_ENTITY_SPOT_TEXTURE, &spot_texels},
          std::pair{LIGHT_ENTITY_RADIANCE_TEXTURE, &radiance_texels}})
        get_texture_manager()->set_texture_from_rgba8(
            texture, texels->data(), width, height, vk::SamplerAddressMode::eClampToEdge,
            vk::Filter::eNearest, vk::Filter::eNearest, false, false);

    const auto& material_system = get_material_system();
    for (auto& [light_style, mesh] : meshes) {
        const merian::MaterialID material_id = material_system->add_material(
            quake_material_type_id, make_light_entity_material(light_style));
        if (light_style != 0)
            light_entity_materials.push_back({material_id, light_style});
        mesh->name = fmt::format("light entities:{}", light_style);
        mesh->material_id = material_id;
        mesh->flags = merian::Scene::MeshFlags::IsOpaque;
        mesh->instance_mask = to_mask(InstanceMask::LIGHT_ENTITY);
        const merian::Scene::MeshID mesh_id = add_mesh(std::move(mesh));
        add_mesh_instance(mesh_id, world_node_id);
        world_mesh_ids.push_back(mesh_id);
    }
}

void QuakeScene::update_animated_materials() {
    const auto& material_system = get_material_system();
    const auto resolve = [&](const AnimatedBrushMaterial& entry, const entity_t* ent, int frame) {
        material_system->update_material(
            entry.material_id,
            brush_material(entry.key, R_TextureAnimation(entry.key.tex, frame), ent));
    };

    for (const auto& entry : world_animated_materials)
        resolve(entry, nullptr, 0);
    for (const auto& [material_id, light_style] : light_entity_materials)
        material_system->update_material(material_id, make_light_entity_material(light_style));

    // ent->frame picks the +0… vs +a… alt-anim set for togglable brush entities.
    for (auto& [ent, slot] : entity_slots) {
        if (slot.animated_materials.empty())
            continue;
        for (const auto& entry : slot.animated_materials)
            resolve(entry, ent, ent->frame);
    }
}

void QuakeScene::properties(merian::Properties& config) {
    config.st_separate("General");
    config.config_bool("gamestate update", update_gamestate);
    update_gamestate = update_gamestate || frame == 0;

    std::string cmd;
    if (config.config_text("command", cmd, true)) {
        queue_command(cmd);
        if (!update_gamestate) {
            SPDLOG_WARN("command unpaused gamestate update");
            update_gamestate = true;
        }
    }
    std::ignore = config.config_text_multiline(
        "startup commands", startup_commands, false,
        "engine command line, e.g. '-game ad +skill 2 +map start'; whitespace separated, use "
        "double quotes for values with spaces, lines starting with # are ignored; applied at "
        "engine startup");

    if (config.config_bool("enable transparency", enable_transparency,
                           "Brush entities and liquids with an alpha let light through.")) {
        update_material_constants();
    }

    config.config_options("filtering", default_filtering, {"nearest", "linear"},
                          merian::Properties::OptionsStyle::COMBO,
                          "requires a level reload to show any effect.");

    config.st_separate("Reproducibility");
    config.config_int("stop after worldspawn", stop_after_worldspawn,
                      "Can be used for reference renders.");
    config.config_bool("rebuild after stop", rebuild_after_stop);
    config.config_bool("reproducible renders", reproducible_renders,
                       "e.g. disables random behavior");

    config.st_separate("Debug / Info");
    config.config_bool("overwrite sun", overwrite_sun);
    if (overwrite_sun) {
        config.config_vec("sun dir", overwrite_sun_dir);
        config.config_vec("sun col", overwrite_sun_col);
    }
    config.config_float("volume max t", volume_max_t,
                        "Distance at which the fog optical depth saturates, so distant sky is not "
                        "fully extinguished.");
    config.config_float("fog particle size", fog_particle_size_um,
                        "Water droplet diameter in micrometer. Fog is roughly 5 - 15, cloud "
                        "droplets reach 50; below 0.1 the phase function tends to Rayleigh.",
                        0.1F, 0.001F, 50.F);
    config.config_float("fog density factor", fog_density_factor,
                        "Scales the engine fog density into an extinction coefficient.", 0.001F,
                        0.F);
    config.config_bool("overwrite mu_t/s", mu_t_s_overwrite);
    if (mu_t_s_overwrite) {
        config.config_float("mu_t", mu_t, "", 0.000001);
        config.config_vec("mu_s / mu_t", mu_s_div_mu_t);
    }
    const merian::FogVolume fog = get_fog();
    config.output_text(fmt::format("mu_t: {}\nalbedo: ({}, {}, {})", fog.mu_t.x, fog.albedo.r,
                                   fog.albedo.g, fog.albedo.b));
    const merian::float3 sd =
        overwrite_sun ? overwrite_sun_dir : g_quake_data.current_sun_direction;
    const merian::float3 sc = overwrite_sun ? overwrite_sun_col : g_quake_data.current_sun_color;
    config.output_text(fmt::format("sun direction: ({}, {}, {})\nsun color: ({}, {}, {})", sd.x,
                                   sd.y, sd.z, sc.r, sc.g, sc.b));
    config.output_text(fmt::format("view angles {} {} {}", r_refdef.viewangles[0],
                                   r_refdef.viewangles[1], r_refdef.viewangles[2]));
    config.output_text(fmt::format("server fps: {}", server_fps));

    if (config.st_begin_child("lighting", "Lighting")) {
        Emission& e = emission;
        bool constants_changed = false;
        bool lights_changed = false;
        const auto constant = [&](const char* id, float& value, const float sensitivity,
                                  const float min, const char* desc = "") {
            constants_changed |= config.config_float(id, value, desc, sensitivity, min);
        };
        lights_changed |= config.config_bool(
            "light styles", light_styles_enabled,
            "Animates emission with the light style (flicker, pulse, switches) of the light the "
            "emitting surface belongs to.");
        config.st_separate("Fullbright: scale * c^gamma * l / (1 - l) * hue gain");
        constant("fullbright scale", e.fullbright_scale, 0.01F, 0.F);
        constant("fullbright gamma", e.fullbright_gamma, 0.01F, 0.01F);
        constant("fullbright exponent", e.fullbright_exponent, 0.01F, 0.01F,
                 "l = min(mean(c)^exponent, max level)");
        constants_changed |= config.config_float("fullbright max level", e.fullbright_max_level, "",
                                                 0.001F, 0.F, 0.999F);
        constant("fullbright red", e.fullbright_red, 0.01F, -1.F, "Extra gain of saturated red.");
        constant("fullbright yellow", e.fullbright_yellow, 0.01F, -1.F);
        constant("fullbright blue", e.fullbright_blue, 0.01F, -1.F);
        constant("waterfall", e.waterfall, 0.01F, 0.F);
        config.st_separate("Sky: exp scale * (2^(exp rate * t) - 1) + pow scale * t^gamma");
        constant("classic exp scale", e.classic_sky_exp_scale, 0.01F, 0.F);
        constant("classic exp rate", e.classic_sky_exp_rate, 0.01F, 0.F);
        constant("classic pow scale", e.classic_sky_pow_scale, 0.01F, 0.F);
        constant("classic pow gamma", e.classic_sky_pow_gamma, 0.01F, 0.01F);
        constant("cube exp scale", e.cube_sky_exp_scale, 0.01F, 0.F);
        constant("cube exp rate", e.cube_sky_exp_rate, 0.01F, 0.F);
        constant("cube pow scale", e.cube_sky_pow_scale, 0.01F, 0.F);
        constant("cube pow gamma", e.cube_sky_pow_gamma, 0.01F, 0.01F);
        config.st_separate("Sun: wide lobe and vMF disc");
        constant("sun lobe", e.sun_lobe, 0.01F, 0.F);
        constant("sun disc", e.sun_disc, 0.01F, 0.F);
        constant("sun kappa", e.sun_kappa, 10.F, 1.F, "vMF concentration, larger is sharper.");
        config.st_separate("Light entities");
        lights_changed |= config.config_bool(
            "fixture emission", fixture_emission_enabled,
            "Lamp surfaces, models and sprites of light entities emit the light of their entity "
            "and of the plain lights around them.");
        constant("fixture", e.fixture, 0.01F, 0.F);
        lights_changed |=
            config.config_bool("light entities", light_entities_enabled,
                               "Hidden emitters at lights the game switches that have no lamp "
                               "surface, model or sprite of their own.");
        constant("switched lights", e.switched_lights, 0.01F, 0.F);
        lights_changed |= config.config_float("light entity radius", light_entity_radius,
                                              "For lights without \"_deviance\".", 0.1F, 0.1F);
        if (constants_changed)
            update_material_constants();
        light_sources_changed |= lights_changed;
        config.st_end_child();
    }

    config.st_separate("Entity counts");
    const auto& s = last_frame_entity_stats;
    config.output_text(fmt::format("alias:  {} active (+{} new)\n"
                                   "brush:  {} active (+{} new)\n"
                                   "sprite: {} active (+{} new)\n"
                                   "sprite frames: {}\n"
                                   "brush submodels: {}",
                                   s.alias.active, s.alias.newly_created, s.brush.active,
                                   s.brush.newly_created, s.sprite.active, s.sprite.newly_created,
                                   sprite_frame_info.size(), brush_submodel_geo.size()));

    config.st_separate("Scene");

    Scene::properties(config);
}

} // namespace merian_quake
