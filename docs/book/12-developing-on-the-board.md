# Chapter 12 - Developing on the board itself

> **Companion reading (official PYNQ documentation):** [Jupyter notebooks on PYNQ](https://pynq.readthedocs.io/en/v3.1/getting_started/jupyter_notebooks.html), [the Python environment](https://pynq.readthedocs.io/en/v3.1/getting_started/python_environment.html), and [designing your own overlay](https://pynq.readthedocs.io/en/v3.1/overlay_design_methodology.html). The desktop menu **PYNQ help** opens the tutorial notebooks stored on the board.

With a desktop, a keyboard and a mouse, the PYNQ-Z2 can be used as a small standalone development machine. This chapter says what is installed for that, and - just as
important for an engineer - which obvious choice was measured and rejected.

## 12.1 The toolkit

| Task | Tool | Why this one |
|---|---|---|
| Notebooks (PYNQ's normal workflow) | **Epiphany** web browser at `http://localhost:9090` (menu: *Jupyter notebooks*) | the board already runs the Jupyter server; the browser only displays it |
| Writing C, Rust, Python, shell, Verilog, device-tree sources | **Geany** (menu: *Text editor*) | a real editor with syntax highlighting, a symbol list, build/run commands and a terminal pane, in 29 MB of RAM |
| Quick edits in a terminal, over SSH too | **micro** | a modern, discoverable terminal editor: normal shortcuts (Ctrl-S, Ctrl-Q), mouse support |
| Browsing files, copying to a USB stick | **PCManFM** (menu: *File manager*) | the file manager of the LXDE desktop: tabs, a places sidebar, drag and drop, 24 MB of RAM |
| Builds | `gcc`, `make`, `cargo` (already on the image), `gcc-12` for kernel modules | the same tools the installer uses |

Everything starts from the right-click menu (chapter 9, section 9.9).

## 12.2 The browser

A modern notebook interface needs a modern JavaScript engine. The lightweight browsers (NetSurf, Dillo, Links) cannot run it. On Ubuntu 22.04 for 32-bit ARM, Firefox and Chromium exist only as *snap* packages, which are not built for this architecture. That leaves the WebKit-based browsers; **Epiphany** ("GNOME Web") was chosen because it needs no desktop environment.

Two details are in the launcher `panther-browser`:

* `WEBKIT_DISABLE_COMPOSITING_MODE=1` - WebKit normally composites page layers with OpenGL; the board has no GPU, so it is told not to.
* **The sandbox stays on.** An earlier attempt set a variable that disables the browser's security sandbox "to be safe from failures". That is the wrong direction: a browser renders untrusted content, and the sandbox is what contains a compromised page. It was never needed; with the sandbox on, Epiphany showed the Jupyter login page. (The tooling used in this project refused the unsafe setting, correctly.)

Measured: Epiphany with a Jupyter page open uses about **280 MB** in total (UI, network process and web processes), on a board with 491 MB.

## 12.3 Why not VS Code?

The first request was for Visual Studio Code with its Jupyter extensions. It was *tested*, not assumed:

* Microsoft publishes a 32-bit ARM build (`code_..._armhf.deb`, 166 MB, 660 MB installed). It installed and started.
* The Jupyter extension also has a native armhf build and installed with its four companion extensions.
* **Memory:** with only the Explorer open, VS Code ran as **10 processes using about 450 MB of the board's 491 MB**. The system started to swap (193 MB of swap used), the load average reached 3.6, and 145 MB remained for everything else - including the 280 MB browser needed to view the notebooks.
* **Experience:** on the board's two 650 MHz cores it was visibly laggy in use, and the person using it judged it not worth keeping.

So it was removed (disk reclaimed: 660 MB). This is a *resource-budget* decision, the most common kind in embedded Linux:

> available memory (491 MB) - system and desktop (about 100 MB) = what applications may use (about 390 MB)
> VS Code alone: ~450 MB.  Browser alone: ~280 MB.  Geany: 29 MB.  PCManFM: 24 MB.

The swap file lives on the SD card; swapping there is much slower than RAM and wears the card. An application that cannot fit is not just slow, it makes *every other program* slow.

**Think about it.** Electron applications (VS Code, Slack, many others) bundle a whole browser engine per application. What does that mean for the total memory when you run three of them? What would you do to run a heavy IDE against this board anyway? (Hint: the board is a *remote* target; where could the editor run instead?)

## 12.4 Developing from another computer

The board runs an SSH server, and a laptop's VS Code can use it through its *Remote - SSH* extension, editing files on the board while the heavy interface runs on the laptop. The Jupyter notebooks the board serves can likewise be opened from the laptop's browser at `http://pynq-panther.local:9090`. The on-board toolkit above is for when there is no laptop: a lab bench with just the board, a monitor and a keyboard.

## 12.5 Exercises

1. Open `kmod/panther_drm/panther_drm.c` in Geany. Use the symbol list to find `panther_pipe_update` and read it against section 5.7.
2. In a terminal, run `free -m` and `top`. Open the browser, then Geany, then a notebook; watch how the numbers move. At what point does swap start to be used?
3. Run `xterm -e micro` and write a small shell script that prints the board's temperature or uptime. Make it executable and run it.
4. Edit the fluxbox menu (`~/.fluxbox/menu`) to add an entry that starts a Python REPL in the PYNQ virtual environment. Reload the menu and test it.
5. Measure the memory of three alternative editors installed with `apt` (`mousepad`, `featherpad`, `l3afpad`) the same way, and make your own recommendation.
