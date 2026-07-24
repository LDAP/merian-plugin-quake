#include "quake_node.hpp"

#include "merian/shader/shader_compile_context.hpp"

#include <spdlog/spdlog.h>

QuakeNode::QuakeNode() : Node() {}

QuakeNode::~QuakeNode() {
    scene.reset(); // before the allocator
}

merian::DeviceSupportInfo
QuakeNode::query_device_support(const merian::DeviceSupportQueryInfo& query_info) {
    return merian::DeviceSupportInfo::check(
        query_info, {"rayQuery", "accelerationStructure", "shaderInt64", "bufferDeviceAddress"});
}

void QuakeNode::initialize(const merian::ContextHandle& context,
                           const merian::ResourceAllocatorHandle& allocator) {
    assert(this->context == nullptr && "QuakeNode was initialized multiple times.");
    this->context = context;
    this->allocator = allocator;
    compile_context = context->get_shader_compile_context();
    texture_manager = std::make_shared<merian::TextureManager>(compile_context, context, allocator,
                                                               merian_quake::MAX_GLTEXTURES);
    material_system = std::make_shared<merian::MaterialSystem>(compile_context, context, allocator,
                                                               texture_manager);
    scene = std::make_shared<merian_quake::QuakeScene>(compile_context, context, allocator,
                                                       material_system);
}

std::vector<merian::InputConnectorDescriptor> QuakeNode::describe_inputs() {
    return {{"controller", con_controller, {}, 0, true}, {"window", con_window, {}, 0, true}};
}

std::vector<merian::OutputConnectorDescriptor>
QuakeNode::describe_outputs([[maybe_unused]] const merian::NodeIOLayout& io_layout) {
    return {{"scene", con_scene}, {"ui_draw_commands", con_ui_draw_commands}};
}

[[nodiscard]] merian::Node::NodeStatusFlags QuakeNode::process(const merian::NodeIO& io,
                                                               const merian::NodeProcessInfo& info,
                                                               merian::Submission& submission) {
    const merian::CommandBufferHandle& cmd = submission.get_cmd();

    // Forward the Window node's controller/window to the engine for input
    // (re-wire if it changes).
    if (io.is_connected(con_controller)) {
        const merian::InputControllerHandle& controller = io[con_controller];
        if (controller && controller != registered_controller.lock()) {
            const merian::WindowHandle window =
                io.is_connected(con_window) ? io[con_window] : nullptr;
            scene->set_controller(controller, window);
            registered_controller = controller;
        }
    }

    scene->update(cmd, static_cast<float>(info.get_elapsed()),
                  static_cast<float>(info.get_time_delta()), info.get_total_iteration());

    io[con_ui_draw_commands] = scene->get_ui_draw_commands();
    return {};
}

QuakeNode::NodeStatusFlags
QuakeNode::on_connected([[maybe_unused]] const merian::NodeIOLayout& io_layout,
                        [[maybe_unused]] const merian::NodeIO& io,
                        [[maybe_unused]] const merian::NodeConnectionInfo& info,
                        [[maybe_unused]] merian::Submission& submission) {
    io[con_scene] = std::static_pointer_cast<merian::Scene>(scene);
    return {};
}

QuakeNode::NodeStatusFlags QuakeNode::properties(merian::Properties& config) {
    if (scene)
        scene->properties(config);
    return {};
}

void QuakeNode::queue_command(const std::string& command) {
    if (scene) {
        scene->queue_command(command);
    } else {
        SPDLOG_WARN("QuakeNode: queue_command before scene init dropped: {}", command);
    }
}
