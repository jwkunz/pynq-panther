# What the driver talks to (PYNQ-Z2 base overlay, PYNQ v3.1)

Taken from `base.hwh` and PYNQ's own Python/C video drivers (`pynq/lib/video`, `pynq/lib/_pynq/_video`).

| Block | Physical address | Role |
|---|---|---|
| `video/axi_vdma` | `0x43000000` | MM2S reads frames from DDR; 4 frame stores; M_AXI is **32 bit** |
| `hdmi_out/frontend/axi_dynclk` | `0x43C10000` | Digilent MMCM wrapper: generates the 5x serial clock, the pixel clock is derived from it |
| `hdmi_out/frontend/vtc_out` | `0x43C20000` | video timing generator |
| `hdmi_out/color_convert` | `0x43C60000` | HLS 3x4 matrix (`ap_ctrl_none`): coefficient i at `0x10 + 8 i`, 1.0 = 256 |
| `hdmi_out/pixel_unpack` | `0x43C70000` | HLS: reg `0x10` = 0 (24 bpp), 1 (32 bpp), 2 (8 bpp) |
| `hdmi_out/frontend/hdmi_out_hpd_video` | `0x41230000` | GPIO to the HDMI-out HPD pin; not touched by PYNQ either |

Not available on this board: DDC/EDID on the output (so the connector offers a fixed mode list) and HPD read-back
(so the connector is always reported connected).

## Pipeline
`DDR --VDMA--> pixel_unpack --> color_convert --> (color_swap) --> v_axi4s_vid_out --> rgb2dvi --> HDMI`
The driver sets `pixel_unpack` to 32 bpp and `color_convert` to the identity, so a DRM `XRGB8888` buffer
(bytes in memory B, G, R, X) is shown with correct colours (identity = PYNQ's `PIXEL_BGR` convention).

## Start-up order (same as PYNQ's `HDMIOut`)
1. program the MMCM through `axi_dynclk` (CTRL=0, wait not running, CTRL=1, wait running)
2. program the VTC generator and enable it (this releases back-pressure on the VDMA)
3. configure `pixel_unpack`, `color_convert`
4. VDMA: start addresses, HSIZE, STRIDE, `CR = 0x00011089`, wait running, then write VSIZE
Page flips write the next frame store's start address and the park pointer; the VDMA switches at its next frame boundary.

## Limits
* 32 bit VDMA memory port: 1080p30 (74.25 MHz, ~250 MB/s) is the top mode; 1080p60 does not fit.
* Pixel clock 25..85 MHz; progressive only; format XRGB8888 only.
