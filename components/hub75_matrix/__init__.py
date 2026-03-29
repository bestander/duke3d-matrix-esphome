import pathlib

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE, EsphomeError

hub75_ns = cg.esphome_ns.namespace("hub75_matrix")
Hub75MatrixClass = hub75_ns.class_("Hub75Matrix", cg.Component)

CONF_SPLASH_IMAGE = "splash_image"
_GEN_SPLASH = pathlib.Path(__file__).resolve().parent / "hub75_splash.gen.h"

# Matches hud::Hud::HUD_TOP — splash only covers rows [0, kSplashHeight); HUD uses the rest.
_SPLASH_W = 64
_SPLASH_H = 40


def _crop_top_to_aspect(rgb, aspect_w_over_h: float):
    """Keep the top of the image: drop the bottom (tall sources) or crop sides (wide sources)."""
    w, h = rgb.size
    if w <= 0 or h <= 0:
        return rgb
    src_ar = w / h
    if src_ar > aspect_w_over_h:
        # Too wide: center-crop horizontally, full height from top.
        new_w = max(1, int(round(h * aspect_w_over_h)))
        new_w = min(new_w, w)
        left = (w - new_w) // 2
        return rgb.crop((left, 0, left + new_w, h))
    # Too tall: crop the bottom only; top of cover art stays.
    new_h = max(1, int(round(w / aspect_w_over_h)))
    new_h = min(new_h, h)
    return rgb.crop((0, 0, w, new_h))


def _write_splash_header(src: pathlib.Path, dst: pathlib.Path) -> None:
    try:
        from PIL import Image
    except ImportError as exc:
        raise EsphomeError(
            "hub75_matrix splash_image requires Pillow (pip install pillow)"
        ) from exc

    try:
        resample = Image.Resampling.LANCZOS
    except AttributeError:
        resample = Image.LANCZOS

    ar = _SPLASH_W / _SPLASH_H
    with Image.open(src) as im:
        rgb = im.convert("RGB")
        cropped = _crop_top_to_aspect(rgb, ar)
        fitted = cropped.resize((_SPLASH_W, _SPLASH_H), resample)
        raw = fitted.tobytes()

    expected = _SPLASH_W * _SPLASH_H * 3
    if len(raw) != expected:
        raise EsphomeError(f"internal splash resize error: got {len(raw)} bytes, want {expected}")

    lines = [
        "#pragma once",
        "#include <cstdint>",
        "namespace esphome {",
        "namespace hub75_matrix {",
        f"static const int kSplashHeight = {_SPLASH_H};",
        f"static const uint8_t kSplashRgb[{_SPLASH_W} * {_SPLASH_H} * 3] = {{",
    ]
    for i in range(0, len(raw), 12):
        chunk = raw[i : i + 12]
        hexes = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"    {hexes},")
    lines.extend(["};", "}  // namespace hub75_matrix", "}  // namespace esphome", ""])
    dst.write_text("\n".join(lines), encoding="utf-8")


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Hub75MatrixClass),
        cv.Optional(CONF_SPLASH_IMAGE): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    if CONF_SPLASH_IMAGE in config:
        src = pathlib.Path(CORE.relative_config_path(str(config[CONF_SPLASH_IMAGE])))
        if not src.is_file():
            raise EsphomeError(f"hub75_matrix splash_image not found: {src}")
        _write_splash_header(src, _GEN_SPLASH)
        cg.add_define("HUB75_HAS_SPLASH")
    elif _GEN_SPLASH.is_file():
        _GEN_SPLASH.unlink()

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
