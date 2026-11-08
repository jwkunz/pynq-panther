#!/bin/sh
# Desktop session started by panther-session once X is up: the pink-and-dark colours, a terminal and the fluxbox window manager.
# dbus-launch gives every program in the session a private message bus (the browser and Geany expect one).
xrdb -merge /usr/local/lib/panther/theme/Xresources 2>/dev/null     # terminal colours; the user's own ~/.Xresources is merged after, so it wins
[ -f "$HOME/.Xresources" ] && xrdb -merge "$HOME/.Xresources"
xsetroot -solid '#050505' 2>/dev/null                              # the desktop colour (the fluxbox style leaves the root window alone)
xset s off -dpms 2>/dev/null
# fluxbox hands the style's wallpaper to its helper fbsetbg, which SKIPS the request when it is identical to the one recorded in
# lastwallpaper - but the picture on the root window does not survive an X restart or a reboot, so the record would make every
# later start come up black. Forgetting it makes fluxbox's own request take effect each time.
rm -f "$HOME/.fluxbox/lastwallpaper"
# On a busy boot, the one wallpaper request fluxbox makes while it starts can get lost (restarts of the desktop never lose it, a cold boot
# sometimes does). So, once fluxbox has settled, ask again - using the ACTIVE style's own background lines, so a style chosen from the menu
# keeps its own picture. Done twice (the second time catches a slow boot); re-applying a picture that is already there changes nothing.
apply_wallpaper() {
  style=$(sed -n 's/^session.styleFile:[[:space:]]*//p' "$HOME/.fluxbox/init" 2>/dev/null | head -1)
  case "$style" in "~"*) style="$HOME${style#\~}" ;; esac
  [ -f "$style" ] || return 0
  pix=$(sed -n 's/^background.pixmap:[[:space:]]*//p' "$style" | head -1)
  mode=$(sed -n 's/^background:[[:space:]]*//p' "$style" | head -1 | tr -d '[:space:]')
  [ -f "$pix" ] || return 0
  case "$mode" in aspect) flag=-a ;; fullscreen) flag=-f ;; centered) flag=-c ;; tiled) flag=-t ;; *) return 0 ;; esac
  rm -f "$HOME/.fluxbox/lastwallpaper"          # fbsetbg would skip a request equal to its record
  fbsetbg $flag "$pix" >/dev/null 2>&1
}
( sleep 6; apply_wallpaper; sleep 20; apply_wallpaper ) &
xterm -geometry 100x30+40+40 -fa Monospace -fs 11 &
exec dbus-launch --exit-with-session fluxbox
