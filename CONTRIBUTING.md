# Contributing

Bug reports and pull requests are welcome. The code is small: a C DRM driver and ALSA module (`kmod/`), a Rust session supervisor (`session/`), shell
(`install.sh`, `scripts/`) and the theme (`config/theme/`). The book in `docs/book/` explains how it works.

**Bug report checklist** (the template asks for these): PYNQ image version, `uname -r`, the output of `systemctl status panther-desktop panther-io`,
and `journalctl -b -u panther-desktop --no-pager | tail -100`. If the board freezes, say what you were doing:
reloading the bitstream (e.g. `BaseOverlay("base.bit")`) while the desktop runs is the known cause; see `panther-mode` and `docs/PYNQ-RESOURCES.md`.

**Pull requests:** keep the change focused; run `shellcheck install.sh scripts/*.sh` if you have it; say which board/image you tested on.
Contributions are accepted under GPL-2.0.
