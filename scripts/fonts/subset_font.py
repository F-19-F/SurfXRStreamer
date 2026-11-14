"""Rebuild the embedded OFL CJK subset from the pinned upstream font, offline."""
from pathlib import Path
import argparse
import hashlib
from fontTools import subset
from fontTools.ttLib import TTFont

SOURCE_SHA256 = "2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    options = parser.parse_args()
    if hashlib.sha256(options.source.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise SystemExit("Upstream Noto Sans CJK SC font hash mismatch")
    ui = Path(__file__).resolve().parents[2] / "surfxr/frame/ui"
    font = TTFont(options.source, recalcTimestamp=False)
    settings = subset.Options()
    settings.recalc_timestamp = False
    settings.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]
    worker = subset.Subsetter(options=settings)
    text = (ui / "translations_zh.hpp").read_text(encoding="utf-8")
    worker.populate(text=text + "".join(chr(c) for c in range(32, 127)) + "…中文")
    worker.subset(font)
    for record in font["name"].names:
        names = {1: "SurfXR UI Sans", 3: "SurfXR UI Sans Regular 1.0", 4: "SurfXR UI Sans Regular", 6: "SurfXRUISans-Regular"}
        if record.nameID in names:
            record.string = names[record.nameID].encode(record.getEncoding())
    font.save(ui / "fonts" / "SurfXRUI.otf")

if __name__ == "__main__":
    main()
