# Task Manager

A desktop system monitor and process manager for Linux, written in C with GTK 4 and libadwaita so it fits the modern GNOME look. The window has a sidebar with eight pages, each covering one area of the system.

## Pages

- **Performance**: live graphs for CPU, memory, disk, network, and temperature. A simple view gives a quick glance, and an expanded view shows full statistics.
- **Processes**: every running program, with search, sorting, and per-process details. You can terminate or kill a process, and both actions ask for confirmation first.
- **Applications**: installed applications.
- **Startup Applications**: see and toggle the programs that launch at login.
- **Services**: systemd units over D-Bus. View, start, stop, or restart them, check dependencies, and read journal logs.
- **Storage**: physical drives and partitions, with usage and throughput. Folder scans show what is using space.
- **Users & Sessions**: logged-in users and their sessions, through logind.
- **Fan Control**: fan and temperature sensors from the hardware monitoring interface. You can set fan speeds, and a 3D fan model rendered with OpenGL reflects the current speed.

## Settings

Open the Settings window from the header gear button or with Ctrl+,.

- Refresh rate per page, from Paused through 100 ms, 250 ms, 500 ms, 1 s, 2 s, 5 s, 10 s, to 30 s
- Pause all live updates
- Choose the startup page
- Remember window size
- Restore defaults

Settings are saved to `~/.config/task-manager/settings.ini`.

## Design

- Only the Performance page loads at startup. Other pages are built on first visit and stop their background work when you leave.
- One shared process monitor scans `/proc` and feeds every page that needs process data.
- Sampling, disk scanning, and other slow work runs on worker threads so the interface stays responsive.

## Building

Requirements: GTK 4.12+, libadwaita 1.5+, GLib 2.76+, json-glib, libepoxy, libsystemd, and Meson with Ninja.

```bash
cd src
meson setup build
ninja -C build
./build/task-manager
```

The build expects `resources/fan.glb` (the 3D fan model). It must be present or `meson setup` will fail.

## Status

This is a work in progress. Requirements and behavior may change between versions.
