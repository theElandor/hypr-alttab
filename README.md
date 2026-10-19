# hypr-alttab

A Windows-style **Alt+Tab** window switcher for [Hyprland](https://hyprland.org), with live window previews.

- Hold **Alt**, tap **Tab** to walk through windows, **release Alt** to switch.
- Windows are listed in most-recently-used order, and the selection starts on the previous window, like on Windows.
- A quick Alt+Tab tap switches to the previous window instantly, without flashing the switcher.
- Live previews of every window, including windows on other workspaces and special workspaces, captured with Hyprland's `hyprland-toplevel-export-v1` protocol.
- Acrylic (blurred) panel with rounded tiles, app icons, titles and a selection outline.
- Opens on the focused monitor, and wraps into several rows when there are many windows.

## Controls (while the switcher is open)

| Key / action               | Effect                                |
| -------------------------- | ------------------------------------- |
| `Tab` / `Shift+Tab`        | next / previous window                |
| Arrow keys, `h j k l`      | move the selection                    |
| Release `Alt`, `Enter`     | switch to the selected window         |
| `Esc`                      | cancel                                |
| `Delete`, middle click, ✕  | close the selected / clicked window   |
| Click                      | switch to that window                 |
| Mouse wheel                | move the selection                    |

## Requirements

- Hyprland (tested on 0.54)
- Python 3 with PyGObject, GTK 3, [gtk-layer-shell](https://github.com/wmww/gtk-layer-shell) and pycairo
- To build the capture helper: a C compiler, `wayland-client` and `wayland-scanner`

On Debian/Ubuntu:

```sh
sudo apt install python3-gi python3-gi-cairo gir1.2-gtk-3.0 gir1.2-gtklayershell-0.1 \
                 libwayland-dev wayland-protocols build-essential pkg-config
```

On Arch:

```sh
sudo pacman -S python-gobject python-cairo gtk3 gtk-layer-shell wayland base-devel
```

## Install

```sh
make
make install                 # installs to ~/.local/bin (override with PREFIX=...)
cp hypr/hypr-alttab.conf ~/.config/hypr/
echo 'source = ~/.config/hypr/hypr-alttab.conf' >> ~/.config/hypr/hyprland.conf
```

Remove any existing `ALT, Tab` binds from your config. The snippet `unbind`s them, but a bind declared after it in your config would win.

You don't need to start the daemon yourself: `hypr-alttab-ctl` starts it on first use. The snippet also starts it with `exec-once`, so the first Alt+Tab is instant.

## Configuration

Copy `config.example.ini` to `~/.config/hypr-alttab/config.ini`. Options include:

- the window scope (all / current monitor / current workspace)
- the modifier key (to use it as Super+Tab, for example)
- the show delay and live-refresh rate
- the tile size and colours

Restart the daemon after changing the config:

```sh
hypr-alttab-ctl quit
```

To use another modifier, change both the bind and the `modifier` option, so that releasing that key confirms the selection:

```ini
# config.ini
[general]
modifier = super
```

```
bind = SUPER, Tab, exec, ~/.local/bin/hypr-alttab-ctl next
bind = SUPER SHIFT, Tab, exec, ~/.local/bin/hypr-alttab-ctl prev
```

## How it works

- `hypr-alttab` is a small GTK 3 daemon that draws the switcher on a layer-shell overlay surface. While the switcher is open, the surface holds exclusive keyboard focus, which is how it sees the Alt release.
- `hypr-alttab-ctl` is the client your binds call. It sends `next`, `prev`, `commit`, `cancel` or `quit` over a unix socket in `$XDG_RUNTIME_DIR`.
- `hypr-alttab-capture` is a tiny C helper. It captures windows through `hyprland-toplevel-export-v1`, box-filters them down to thumbnail size and streams the pixels to the daemon.

## License

MIT, see [LICENSE](LICENSE). `protocols/hyprland-toplevel-export-v1.xml` is from [hyprland-protocols](https://github.com/hyprwm/hyprland-protocols) and keeps its own BSD-3-Clause license.
