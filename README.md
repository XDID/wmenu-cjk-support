fork from https://codeberg.org/adnano/wmenu

add cjk input support and config for both latin and cjk font
-E for latin
-C for CJK
-F to enable fuzzy search

# wmenu

wmenu is an efficient dynamic menu for Sway and wlroots based Wayland
compositors. It provides a Wayland-native dmenu replacement which maintains the
look and feel of dmenu.

## Installation

Dependencies:

- cairo
- pango
- wayland
- xkbcommon
- scdoc
```
$ meson setup build
$ ninja -C build
# ninja -C build install
```

## Usage

See wmenu(1)

To use wmenu with Sway, you can add the following to your configuration file:

```
set $menu wmenu-run
bindsym $mod+d exec $menu
```
# wmenu-cjk-support
