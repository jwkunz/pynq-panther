# Security notes

This project configures a hobby/teaching board; it is not hardened for hostile networks.

* The stock PYNQ image ships the account `xilinx` / `xilinx`, and Jupyter on port 9090 with the password `xilinx`. This project does not change them.
  **Change both** (`passwd`; Jupyter's password in its config) before connecting the board to a shared or public network.
* The installer announces `pynq-panther.local` by mDNS/avahi, with the SSH and Jupyter services. Turn this off with `--keep-hostname` at install time or by
  stopping `avahi-daemon`.
* `install.sh` runs as root and builds and loads kernel modules. Read it first; it is a plain shell script.
* The offline bundle is verified by the SHA-256 in the release's `SHA256SUMS.txt`.

**Reporting a vulnerability:** please open a GitHub issue marked *security* (no secrets in it), or contact the maintainer through their GitHub profile.
