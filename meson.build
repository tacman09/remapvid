project(
  'remapvid',
  ['c',],
  version : '0.0.1',
  license : 'MIT',
  default_options : [
    'buildtype=debugoptimized',
    'c_std=gnu99'
  ],
)

cc = meson.get_compiler('c')

mmal_libs = [
  'vcsm',
  'mmal',
  'mmal_core',
  'mmal_util',
  'mmal_vc_client',
  'bcm_host',
  'vcos',
]

vc_dir = '/opt/vc'
mmal_dep = []
foreach lib: mmal_libs
  mmal_dep += declare_dependency(
    link_args: [
      '-L@0@/lib'.format(vc_dir),
      '-l@0@'.format(lib),
      '-Wl,-rpath-link,@0@/lib'.format(vc_dir),
    ],
    include_directories: include_directories('@0@/include'.format(vc_dir))
  )
endforeach

env_prog = find_program('env')
python3_prog = import('python').find_installation('python3')
kernel_bin = custom_target(
    'kernel.bin',
    output : 'kernel.bin',
    input : 'assemble_kernel.py',
    command : [env_prog, 'PYTHONPATH=' + join_paths(meson.source_root(), 'rpi-vcsm') + ':' + join_paths(meson.source_root(), 'py-videocore'), python3_prog, '@INPUT@', '@OUTPUT@'],
)

xxd_prog = find_program('xxd')
kernel_h = custom_target(
    'kernel.h',
    output : 'kernel.h',
    input : kernel_bin,
    command : [xxd_prog, '--include', '@INPUT@', '@OUTPUT@'],
)

executable(
  'remapvid',
  [kernel_h, 'mailbox.c', 'vcsm_util.c', 'remapvid.c'],
  dependencies: [
    dependency('threads'),
    cc.find_library('rt'),
    cc.find_library('m'),
    mmal_dep,
  ],
  install: true,
)
