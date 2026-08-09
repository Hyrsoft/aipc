# AIPC VisionG models for RV1106

This bundle contains the model and resource files used by the 19 official AIPC
VisionG examples. It was validated on Luckfox Pico Ultra W / RV1106 with
VisionG v1.2.1 and `librknnmrt.so` 2.3.2.

- `manifest.json` maps every example project to its required resources.
- `SHA256SUMS` is the authoritative integrity list.
- The models imported from VisionG are pinned to commit
  `2c12bebe6852f522a61fa80a03bdefe3d40b2f17`.
- `yolo11n_number_320.rknn` comes from the Python sample archive supplied for
  the AIPC port and is included so every official example is reproducible.
- `ncc_template.jpg` is an AIPC example asset.

The model files are examples for Rockchip RKNN hardware. See the AIPC and
VisionG third-party notices, including `RKNN-SDK-LICENSE.txt`, before
redistributing them in another product.
