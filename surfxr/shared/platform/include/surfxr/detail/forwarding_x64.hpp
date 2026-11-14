#pragma once
namespace surfxr::forwarding {
inline std::optional<Plan> x64(std::uintptr_t code, const Reader &read) {
  std::array<Path, 16> regs{};
  regs[1].root = 1;
  for (unsigned r : {2u, 8u, 9u})
    regs[r].root = r + 2;
  std::array<std::uintptr_t, 32> visited{};
  const auto byte = [&](std::uint8_t &value) {
    if (!read(code, &value, 1, true))
      return false;
    return add(code, 1, code);
  };
  const auto displacement = [&](unsigned size, std::int64_t &result) {
    if (size == 1) {
      std::uint8_t v;
      if (!byte(v))
        return false;
      result = static_cast<std::int8_t>(v);
      return true;
    }
    std::int32_t v;
    if (!read(code, &v, 4, true) || !add(code, 4, code))
      return false;
    result = v;
    return true;
  };
  for (unsigned step = 0; step < visited.size(); ++step) {
    for (unsigned i = 0; i < step; ++i)
      if (visited[i] == code)
        return {};
    visited[step] = code;
    std::uint8_t op = 0, rex = 0;
    if (!byte(op))
      return {};
    if (op == 0x90)
      continue;
    if (op == 0xf3) { // ENDBR64 is a no-effect indirect-branch landing pad.
      std::uint8_t a, b, c;
      if (!byte(a) || !byte(b) || !byte(c) || a != 0x0f || b != 0x1e ||
          c != 0xfa)
        return {};
      continue;
    }
    if (op == 0xe9 || op == 0xeb) {
      std::int64_t offset;
      if (!displacement(op == 0xeb ? 1 : 4, offset) || !add(code, offset, code))
        return {};
      continue;
    }
    if ((op & 0xf0) == 0x40) {
      rex = op;
      if (!byte(op))
        return {};
    }
    bool nop = false;
    if (op == 0x0f) {
      if (!byte(op) || op != 0x1f)
        return {};
      nop = true;
    }
    if (!nop && op != 0x8b && op != 0x89 && op != 0xff)
      return {};
    if (!nop && op != 0xff && !(rex & 8))
      return {}; // Reject all narrowing writes.
    std::uint8_t m;
    if (!byte(m))
      return {};
    const unsigned mode = m >> 6, field = (m >> 3) & 7;
    const unsigned reg = field + ((rex & 4) ? 8 : 0),
                   rm = (m & 7) + ((rex & 1) ? 8 : 0);
    Path operand = regs[rm];
    if (mode != 3) {
      // No stack/indexed addressing: forwarding may neither inspect nor
      // modify stack arguments. Decode base+displacement dynamically.
      if ((m & 7) == 4 || (mode == 0 && (m & 7) == 5))
        return {};
      std::int64_t offset = 0;
      if (mode && !displacement(mode == 1 ? 1 : 4, offset))
        return {};
      operand = load(operand, offset);
    }
    if (nop) {
      if (field)
        return {};
      continue;
    }
    if (op == 0xff) {
      if (field != 4)
        return {};
      for (unsigned r : {2u, 8u, 9u})
        if (!(regs[r] == Path{r + 2, 0, {}}))
          return {};
      return finish(regs[1], operand);
    }
    const unsigned destination = op == 0x8b ? reg : rm;
    if (op == 0x89 && mode != 3)
      return {}; // No memory stores.
    // Windows x64 nonvolatile registers and RSP may not be changed.
    if (destination != 0 && destination != 1 && destination != 2 &&
        destination != 8 && destination != 9 && destination != 10 &&
        destination != 11)
      return {};
    regs[destination] = op == 0x8b ? operand : regs[reg];
  }
  return {};
}
} // namespace surfxr::forwarding
