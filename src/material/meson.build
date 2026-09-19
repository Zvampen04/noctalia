# Standalone project: meson setup BUILD_DIR /path/to/noctalia/src/material
# The shell can include the headers directly; do not subdir() this project.
project('noctalia-material-core', 'cpp',
  version: '5.0.0',
  license: ['MIT', 'BSD-3-Clause'],
  default_options: ['cpp_std=c++23', 'cpp_extensions=false', 'warning_level=3'],
)

material_headers = files('material.h', 'shader_source.h', 'fields.def', 'scene_descriptor.h', 'scene_transport.h', 'shape_source.h', 'shape_geometry.h')
install_headers(material_headers, subdir: 'noctalia/material')
install_data('LICENSE', 'LIQUID-GLASS-LICENSE', 'NEUMORPHISM-LICENSE',
  install_dir: get_option('datadir') / 'licenses' / 'noctalia-material-core')
install_data('README.md',
  install_dir: get_option('datadir') / 'doc' / 'noctalia-material-core')

install_data('protocol/noctalia-material-v1.xml', install_dir: get_option('datadir') / 'noctalia-material-core' / 'protocol')

pkgconfig = import('pkgconfig')
pkgconfig.generate(
  name: 'Noctalia material core',
  filebase: 'noctalia-material-core',
  description: 'Palette-independent material parameters and shared GLSL primitives',
  version: meson.project_version(),
  subdirs: 'noctalia',
  variables: ['material_schema_version=5', 'protocol_dir=${prefix}/' + get_option('datadir') / 'noctalia-material-core' / 'protocol'],
)

if get_option('tests')
  material_test = executable('material-core-test', 'tests/material_test.cpp',
    include_directories: include_directories('.'),
    install: false,
  )
  test('material-core', material_test)
  descriptor_test = executable('material-scene-test', 'tests/scene_descriptor_test.cpp',
    include_directories: include_directories('.'), install: false)
  test('material-scene', descriptor_test)
  transport_test = executable('material-scene-transport-test', 'tests/scene_transport_test.cpp',
    include_directories: include_directories('.'), install: false)
  test('material-scene-transport', transport_test)
endif
