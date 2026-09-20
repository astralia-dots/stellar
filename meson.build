project(
  'stellar',
  'c',
  'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++23', 'warning_level=3'],
)

wayland_client_dep = dependency('wayland-client')
wayland_egl_dep    = dependency('wayland-egl')
egl_dep            = dependency('egl')
glesv2_dep         = dependency('glesv2') # ES 2.0 entry points; shaders are #version 100
freetype2_dep      = dependency('freetype2')
fontconfig_dep     = dependency('fontconfig')
nlohmann_json_dep  = dependency('nlohmann_json')

wayland_scanner = find_program('wayland-scanner')
wl_proto_dir = dependency('wayland-protocols').get_variable(pkgconfig: 'pkgdatadir')

# base => [xml, needs_sed]. wlr-layer-shell has a `namespace` argument (a C++
# keyword), so its output is sed-renamed to `name_space`. xdg-shell is only
# here because layer-shell's get_popup references its types. Core wl_*
# interfaces come from libwayland-client.
protocol_defs = [
  ['xdg-shell',                   wl_proto_dir / 'stable/xdg-shell/xdg-shell.xml', false],
  ['wlr-layer-shell-unstable-v1', 'protocols/wlr-layer-shell-unstable-v1.xml',     true],
]

sed_fix = ' && sed -i s/namespace/name_space/g "$3"'
proto_gen = []
foreach p : protocol_defs
  base = p[0]
  fixup = p[2] ? sed_fix : ''
  proto_gen += [
    custom_target(base + '-client-header',
      input: p[1], output: base + '-client-protocol.h',
      command: ['sh', '-c', '"$1" client-header "$2" "$3"' + fixup,
                '_', wayland_scanner.full_path(), '@INPUT@', '@OUTPUT@']),
    custom_target(base + '-client-source',
      input: p[1], output: base + '-client-protocol.c',
      command: ['sh', '-c', '"$1" private-code "$2" "$3"' + fixup,
                '_', wayland_scanner.full_path(), '@INPUT@', '@OUTPUT@']),
  ]
endforeach

stellar_inc = include_directories('src')

executable(
  'stellar',
  'src/main.cpp',
  'src/binds.cpp',
  'src/watch.cpp',
  'src/text.cpp',
  'src/ui.cpp',
  proto_gen,
  include_directories: stellar_inc,
  dependencies: [
    wayland_client_dep,
    wayland_egl_dep,
    egl_dep,
    glesv2_dep,
    freetype2_dep,
    fontconfig_dep,
    nlohmann_json_dep,
  ],
  install: true,
  install_dir: 'bin',
)

stellar_test = executable(
  'stellar-test',
  'test/binds_test.cpp',
  'src/binds.cpp',
  include_directories: stellar_inc,
  dependencies: [nlohmann_json_dep],
)

test('stellar', stellar_test)
