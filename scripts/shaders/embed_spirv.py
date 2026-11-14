"""Write one complete SPIR-V array per header and a stable include facade."""
from pathlib import Path
import re


def write_shader_headers(root: Path, stem: str, generated: str, check: bool) -> None:
    directory = root / "frame/xr/generated"
    arrays = re.findall(r"inline constexpr std::uint32_t (\w+)\[\] = \{(.*?)\};", generated, re.S)
    if not arrays:
        raise ValueError("No compiled shader arrays")
    comments = "\n".join(line for line in generated.splitlines() if line.startswith("//")) + "\n"
    outputs = {}
    for name, data in arrays:
        words = re.findall(r"0x[0-9a-f]{8}u", data)
        rows = [", ".join(words[i:i + 16]) for i in range(0, len(words), 16)]
        outputs[name + ".hpp"] = (comments + "#pragma once\n#include <cstdint>\nnamespace surfxr {\n"
                                  + "inline constexpr std::uint32_t " + name + "[] = {\n    "
                                  + ",\n    ".join(rows) + "\n};\n} // namespace surfxr\n")
    outputs[stem + "_spirv.hpp"] = (comments + "#pragma once\n"
                                    + "".join('#include "' + name + '.hpp"\n' for name, _ in arrays))
    for name, content in outputs.items():
        path = directory / name
        if check:
            if path.read_text(encoding="utf-8") != content:
                raise SystemExit(f"Embedded shader differs: {name}; regenerate with the same compiler.")
        else:
            path.write_text(content, encoding="utf-8", newline="\n")
