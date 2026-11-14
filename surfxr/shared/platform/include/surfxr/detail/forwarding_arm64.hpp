#pragma once
namespace surfxr::forwarding {
inline std::optional<Plan>
arm64(std::uintptr_t code, unsigned integer_arguments, const Reader &read) {
  if (integer_arguments > 7 || code % 4)
    return {};
  std::array<Path, 32> regs{};
  regs[0].root = 1;
  for (unsigned r = 1; r <= integer_arguments; ++r)
    regs[r].root = r + 2;
  std::array<std::uintptr_t, 32> visited{};
  for (unsigned step = 0; step < visited.size(); ++step) {
    for (unsigned i = 0; i < step; ++i)
      if (visited[i] == code)
        return {};
    visited[step] = code;
    std::uint32_t word;
    if (!read(code, &word, 4, true))
      return {};
    const auto current = code;
    if (!add(code, 4, code))
      return {};
    if (word == 0xd503201f || word == 0xd503245f || word == 0xd503249f ||
        word == 0xd50324df)
      continue;                              // NOP/BTI
    if ((word & 0xfc000000) == 0x14000000) { // B, never BL (changes LR).
      std::int64_t offset = word & 0x03ffffff;
      if (offset & (1 << 25))
        offset -= 1 << 26;
      if (!add(current, offset * 4, code))
        return {};
      continue;
    }
    const unsigned destination = word & 31, base = (word >> 5) & 31;
    if ((word & 0xfffffc1f) == 0xd61f0000) { // BR Xn
      for (unsigned r = 1; r <= integer_arguments; ++r)
        if (!(regs[r] == Path{r + 2, 0, {}}))
          return {};
      return finish(regs[0], regs[base]);
    }
    // x18 is platform-reserved, x19+ nonvolatile/LR/SP; reject writes.
    if (destination >= 18)
      return {};
    if ((word & 0xffc00000) == 0xf9400000) { // LDR Xt,[Xn,#imm12*8]
      if (base == 31)
        return {};
      regs[destination] = load(regs[base], ((word >> 10) & 4095) * 8);
    } else if ((word & 0xffe0ffe0) == 0xaa0003e0) { // MOV Xd,Xm (ORR alias)
      regs[destination] = regs[(word >> 16) & 31];
    } else
      return {}; // No stores, calls, SIMD changes or W-register narrowing.
  }
  return {};
}
} // namespace surfxr::forwarding
