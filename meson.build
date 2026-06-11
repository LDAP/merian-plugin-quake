project(
    'merian-plugin-quake',
    ['cpp', 'c'],
    version: '0.0.1',
    # No buildtype/b_ndebug here: a plugin shares merian's ABI, so it must inherit merian's
    # assertion/optimization config (built in-tree it follows merian; standalone, pass --buildtype).
    default_options: [
        'warning_level=2',
        'cpp_std=c++23',
    ],
    meson_version: '>=1.3.0',
)

# Consumed via pkg-config (no vendoring of merian). For development against a merian build tree:
#   PKG_CONFIG_PATH=<merian>/build/meson-uninstalled meson setup build
merian = dependency('merian')
quake = dependency('quakespasm', fallback: ['quakespasm', 'quake_dep'],
                   default_options: {'snd_extern': true})

src = files(
    'src/plugin.cpp',
    'src/game/quake_extraction.cpp',
    'src/game/quake_node.cpp',
    'src/game/quake_scene.cpp',
    'src/game/quake_ui_node.cpp',
    'src/game/quake_draw.vert.spv.c',
    'src/game/quake_draw.frag.spv.c',
)

shared_library(
    'merian-plugin-quake',
    src,
    dependencies: [merian, quake],
    include_directories: include_directories('src'),
    name_prefix: '',
    install: true,
    install_dir: join_paths(get_option('libdir'), 'merian', 'plugins'),
)

# Runtime-compiled shaders (quake-material.slang, quake_sky.slang). Installed next to the .so so
# merian's plugin loader finds them automatically (resource_search_paths convention).
install_subdir('res', strip_directory: true,
               install_dir: join_paths(get_option('libdir'), 'merian', 'plugins', 'merian-plugin-quake'))
install_data('quake.json', install_dir: join_paths(get_option('datadir'), 'merian-plugin-quake'))
