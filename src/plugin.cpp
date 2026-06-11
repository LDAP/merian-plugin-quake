#include "game/quake_node.hpp"
#include "game/quake_ui_node.hpp"

#include "merian-graph/graph/node_registry.hpp"
#include "merian-graph/plugin/node_plugin.hpp"

namespace {

merian::NodeHandle make_quake_node() { return std::make_shared<QuakeNode>(); }

} // namespace

extern "C" {

MERIAN_PLUGIN_EXPORT const char *merian_plugin_name(void) { return "quake"; }

MERIAN_PLUGIN_EXPORT uint32_t merian_node_plugin_abi_version(void) {
  return MERIAN_NODE_PLUGIN_ABI_VERSION;
}

MERIAN_PLUGIN_EXPORT void
merian_register_nodes(merian::NodeRegistry &registry) {
  registry.register_node_type<QuakeNode>(merian::NodeRegistry::NodeTypeInfo{
      "Quake",
      "Runs the Quake engine and outputs its scene and HUD draw commands.",
      make_quake_node});
  registry.register_node_type<merian_quake::QuakeUiNode>(
      "Quake UI", "Replays Quake HUD draw commands as textured quads.");
}

} // extern "C"
