project('task-manager', 'c',
  version : '0.1.0',
  default_options : ['c_std=c11', 'warning_level=3'])


pkg = [
  dependency('gtk4', version : '>=4.12'),
  dependency('libadwaita-1', version : '>=1.5'),
  dependency('glib-2.0', version : '>=2.76'),
  dependency('gio-2.0'),
  dependency('gio-unix-2.0'),
  dependency('gdk-pixbuf-2.0'),
  dependency('cairo'),
  dependency('json-glib-1.0'),
  dependency('epoxy'),
  dependency('libsystemd'),
]

add_project_arguments('-DTASK_MANAGER_DATA_DIR="@0@"'.format(join_paths(get_option('datadir'), 'task-manager')), language : 'c')

sources = files(
  'main.c',
  'app/app.c',
  'ui/ui.c',
  'graphs/graph.c',
  'data/history.c',
  'system/process_backend.c',
  'pages/performance/performance.c',
  'pages/processes/processes.c',
  'pages/applications/applications.c',
  'pages/startup/startup.c',
  'pages/services/services.c',
  'pages/storage/storage.c',
  'pages/users/users.c',
  'pages/fan_control/fan_control.c',
)

executable('task-manager', sources,
  dependencies : pkg,
  c_args : ['-D_GNU_SOURCE'],
  install : true,
  link_args : ['-lm', '-ldl'])

install_data('resources/style.css', install_dir : join_paths(get_option('datadir'), 'task-manager'))
install_data('resources/fan.glb', install_dir : join_paths(get_option('datadir'), 'task-manager', 'resources'))
