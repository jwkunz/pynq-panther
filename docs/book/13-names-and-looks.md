# Chapter 13 - Names and looks

Two small features finish the project: the board gets a **name** other computers can find (`pynq-panther.local`), and the desktop gets a **look** (pink and dark). Both are good small studies of how Linux layers work: a name travels through several services; a colour travels through several toolkits.

## 13.1 Finding a computer by name

A computer on a network is reached by an *IP address* (`192.168.2.99`), but people prefer names. The usual path is **DNS**: a server holds the table "name -> address" and your computer asks it. On a bench with one cable and no router there is no DNS server. The answer is **mDNS** (*multicast DNS*, the "Bonjour"/"Zeroconf" idea):

1. Names under the special domain **`.local`** are not looked up on a server.
2. Instead the asking computer sends its question to a *multicast* address (`224.0.0.251`, UDP port 5353) that every computer on the same link listens to: "who is `pynq-panther.local`?"
3. The computer with that name answers: "I am, at `192.168.2.141`".

On the board the program that answers is `avahi-daemon`. On the asking side, the C library consults a list in `/etc/nsswitch.conf` (`hosts: files mdns4_minimal [NOTFOUND=return] dns`): *files* first (`/etc/hosts`), then the mDNS plug-in (`libnss-mdns`), then DNS. That is why `ping pynq-panther.local` works with no server anywhere.

### What the installer sets up

| Step | Why |
|---|---|
| `hostnamectl set-hostname Pynq-Panther` | the board's own idea of its name; shown in the shell prompt |
| the `127.0.1.1` line of `/etc/hosts` | so that programs that look up the board's own name (such as `sudo`) never wait for a network lookup |
| `host-name=pynq-panther` and `domain-name=local` in `/etc/avahi/avahi-daemon.conf` | the label avahi announces: lower-case letters, digits and hyphens, because host names in the DNS are case-insensitive and restricted to those characters |
| `/etc/avahi/services/panther-ssh.service`, `panther-jupyter.service` | *service discovery*: besides the name, announce that this host offers SSH (port 22) and a web server (port 9090), so network browsers list them |
| restart `avahi-daemon` | to publish the new names at once |

The service files are tiny XML documents; the `%h` in a name is replaced by the host name.

**What can go wrong.** Two hosts claiming one name: avahi notices the clash and gives the second one `-2`. Some networks block multicast (guest Wi-Fi, many campus networks); then `.local` names fail while IP addresses work. A name is only as unique as the network segment: mDNS does not cross routers.

**Think about it.** Why does mDNS use *multicast* and not broadcast? What would stop it working across two networks joined by a router? Why must the label be lower-case ASCII?

## 13.2 Where does a program get its colours?

A "theme" would be one file in a perfect world. In reality every graphical program draws with a *toolkit*, and each toolkit has its own idea of colour. The desktop in this project has programs from **five** worlds:

| Program | Toolkit | Where its colours come from |
|---|---|---|
| fluxbox (window frames, toolbar, menus) | its own drawing code | a *style file* (`session.styleFile` in `~/.fluxbox/init`) |
| xterm (the terminal) | the old X Toolkit (Xt) | *X resources* in the X server's database, set with `xrdb` |
| Geany's window, the browser's window | **GTK 3** | `~/.config/gtk-3.0/settings.ini` and `gtk.css` |
| PCManFM (the file manager) | **GTK 2** | `~/.gtkrc-2.0` |
| the *text* inside Geany's editor | Scintilla (a text-editing component inside Geany) | Geany's own *colour scheme* files |

### Fluxbox: a style file

A style is a plain list of `property: value` lines: `window.title.focus: flat gradient rectangle`, `window.title.focus.color: #1f111a`, `window.title.focus.colorTo: #ff3389`. The names say where they apply: `menu.*` the menus, `toolbar.*` the bar, `window.*` the frames. The pink-and-dark style is `config/theme/PinkDark`, derived in *structure* from an existing style on the board with every colour replaced. The menu **Colour theme** (and the stock *Styles* entry) lists the style files in `/usr/share/fluxbox/styles` and `~/.fluxbox/styles`; choosing one makes fluxbox load it and rewrite `session.styleFile`, so the choice survives a restart.

### X resources: the terminal

X keeps a small database of settings; programs ask it for "XTerm\*background". `xrdb -merge` adds entries. The session script merges the project's file first and the user's own `~/.Xresources` second, so the user's settings win.

### GTK 3 and GTK 2

GTK 3 reads `settings.ini` for choices (use the dark variant of the built-in Adwaita theme) and `gtk.css` for overrides. Adwaita's colours come from *named colours* (`@theme_bg_color`, `@theme_selected_bg_color`, ...); redefining those few names with `@define-color` repaints the whole interface. GTK 2 has an older, different mechanism: an *rc file* that assigns colours per widget *state* (`bg[NORMAL]`, `fg[PRELIGHT]`, `base[SELECTED]`) to a named style which is attached to a class of widgets (`class "GtkWidget" style "panther"`).

Which toolkit does a program use? `ldd /usr/bin/pcmanfm | grep gtk` prints `libgtk-x11-2.0.so.0`: GTK 2. (Tale 11 in chapter 10 is the story of finding this out the hard way.)

### Geany: colours inside the editor

Syntax colours are not the GTK theme's business. Geany reads a *colour scheme* from `~/.config/geany/colorschemes/`, a list of named styles (`keyword=#ff4fa3;;true` meaning foreground; background; bold), chosen with `color_scheme=` in `geany.conf`. The project's scheme is a copy of the one Geany ships with new colours.

### The wallpaper

The wallpaper is requested by the *style* too, when the desktop starts: `background: aspect` and `background.pixmap: /path/to/picture.png` (and `background.color` for the bars when the picture's shape differs from the screen's). Fluxbox does not draw pictures itself: it runs a helper script, `fbsetbg`, which looks for an image-setting program (`feh`, `hsetroot`, `Esetroot`, `display`, ...) and uses the first it finds, and it remembers what was set in `~/.fluxbox/lastwallpaper`. This is why the installer adds the small `feh` package, and why writing `background: none` is a trap: it looks harmless, but makes fluxbox re-apply the *last remembered* wallpaper (on this board a logo from another style). The colours of the whole theme were *sampled from the picture* (a short program counted the most frequent non-black pixels and the colours of the text and logo), so that the window frames, menus and terminal belong with the wallpaper. One more detail matters at boot: `fbsetbg` records what it last set in `~/.fluxbox/lastwallpaper` and **skips a request identical to the record**, but the picture on the root window does not survive a reboot. The session script therefore deletes the record before starting fluxbox, and asks for the active style's wallpaper again once the desktop has settled, in case the one request made during a busy boot is lost (chapter 10, tale 12). Also, choosing a different style from the menu changes the window colours at once but leaves the current picture on screen until the next start (observed on the board).

## 13.3 Fitting in with existing machinery

Two rules guided the design:

1. **Use the mechanisms that are already there.** Fluxbox already lists styles in its menu, so the theme is just another style file in the folder it scans, plus an explicit entry at the top of a *Colour theme* submenu. No new menu code, and users can pick any other look and return.
2. **Never destroy the user's files.** Each file the theme replaces is first saved once as `<name>.pre-panther`; the uninstall puts every one back. Files the installer writes carry a first-line marker (`panther-managed`) so that it can tell its own files from yours when it runs again.

## 13.4 Exercises

1. Change the accent colour from pink to teal. Which files must you edit? Use `grep -rn ff3389 config/theme` to find every place, and decide whether a single palette file plus a small script would be better engineering.
2. Install a GTK 2 program and a GTK 3 program you have not used before. How do you tell which toolkit each uses? Does it follow the theme?
3. On a laptop on the same network, run `avahi-browse -a` (Linux) or a Bonjour browser. Which services does the board announce? Add a service file that announces `_ftp._tcp` on port 21 and check that it appears.
4. Use `tcpdump -n udp port 5353` on your computer while running `ping pynq-panther.local`. Find the question and the answer packets.
5. Design a *dark* variant and a *light* variant of the same palette so a user can switch with one menu click. What has to change in each of the five layers?
