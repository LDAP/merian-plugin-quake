#pragma once

#include "game/quake_draw.hpp"
#include "game/quake_scene.hpp"

#include "merian-graph/connectors/ptr_in.hpp"
#include "merian-graph/connectors/ptr_out.hpp"
#include "merian-graph/graph/node.hpp"
#include "merian-shaders/scene/scene.hpp"
#include "merian/vk/window/window.hpp"

namespace merian_quake {
class QuakeScene;
} // namespace merian_quake

class QuakeNode : public merian::Node {
  public:
    QuakeNode();
    ~QuakeNode() override;

    merian::DeviceSupportInfo
    query_device_support(const merian::DeviceSupportQueryInfo& query_info) override;

    [[nodiscard]] NodeStatusFlags on_connected(const merian::NodeIOLayout& io_layout,
                                               const merian::NodeIO& io,
                                               const merian::NodeConnectionInfo& info,
                                               merian::Submission& submission) override;

    void initialize(const merian::ContextHandle& context,
                    const merian::ResourceAllocatorHandle& allocator) override;

    std::vector<merian::InputConnectorDescriptor> describe_inputs() override;

    std::vector<merian::OutputConnectorDescriptor>
    describe_outputs(const merian::NodeIOLayout& io_layout) override;

    [[nodiscard]] merian::Node::NodeStatusFlags process(const merian::NodeIO& io,
                                                        const merian::NodeProcessInfo& info,
                                                        merian::Submission& submission) override;

    NodeStatusFlags properties(merian::Properties& config) override;

    void queue_command(const std::string& command);

  private:
    merian::ContextHandle context;
    merian::ResourceAllocatorHandle allocator;
    merian::ShaderCompileContextHandle compile_context;
    merian::TextureManagerHandle texture_manager;
    merian::MaterialSystemHandle material_system;

    merian_quake::QuakeSceneHandle scene;

    // Optional input from a Window node, forwarded to the Quake engine for play.
    merian::PtrInHandle<merian::InputController> con_controller =
        merian::PtrIn<merian::InputController>::create();
    merian::PtrInHandle<merian::Window> con_window = merian::PtrIn<merian::Window>::create();
    std::weak_ptr<merian::InputController> registered_controller;

    merian::PtrOutHandle<merian::Scene> con_scene = merian::PtrOut<merian::Scene>::create(true);
    merian::PtrOutHandle<merian_quake::UIDrawCommands> con_ui_draw_commands =
        merian::PtrOut<merian_quake::UIDrawCommands>::create(true);
};
