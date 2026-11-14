#pragma once
// Bounded semantic analysis of transparent tail forwarding, not a byte
// signature. Unsupported instructions, effects or memory always reject the
// adaptation.
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>

namespace surfxr::forwarding {
using Reader = std::function<bool(std::uintptr_t, void *, std::size_t, bool)>;
inline bool add(std::uintptr_t base, std::int64_t offset, std::uintptr_t &out) {
  if (offset >= 0) {
    if (std::uint64_t(offset) >
        std::numeric_limits<std::uintptr_t>::max() - base)
      return false;
    out = base + offset;
  } else {
    const auto magnitude = std::uint64_t(-(offset + 1)) + 1;
    if (magnitude > base)
      return false;
    out = base - magnitude;
  }
  return true;
}
struct Path {
  unsigned root = 0, depth = 0;
  std::array<std::int64_t, 6> offsets{};
  bool operator==(const Path &other) const {
    if (root != other.root || depth != other.depth)
      return false;
    for (unsigned i = 0; i < depth; ++i)
      if (offsets[i] != other.offsets[i])
        return false;
    return true;
  }
};
inline Path load(Path base, std::int64_t offset) {
  if (!base.root || base.depth == base.offsets.size())
    return {};
  base.offsets[base.depth++] = offset;
  return base;
}
struct Plan {
  Path object;
  unsigned slot = 0;
};
inline std::optional<Plan> finish(const Path &object, const Path &target) {
  if (object.root != 1 || !object.depth || target.root != 1 ||
      target.depth != object.depth + 2)
    return {};
  for (unsigned i = 0; i < object.depth; ++i)
    if (object.offsets[i] != target.offsets[i])
      return {};
  const auto slot = target.offsets[target.depth - 1];
  if (target.offsets[object.depth] != 0 || slot < 0 || slot % 8 ||
      slot / 8 >= 256)
    return {};
  return Plan{object, unsigned(slot / 8)};
}
struct Target {
  std::uintptr_t instance = 0, table = 0, function = 0;
  unsigned slot = 0;
};
inline std::optional<Target> resolve(const Plan &plan, std::uintptr_t instance,
                                     const Reader &read) {
  if (plan.object.root != 1 || !plan.object.depth || plan.object.depth > 4 ||
      plan.slot >= 256)
    return {};
  std::uintptr_t address = 0;
  for (unsigned i = 0; i < plan.object.depth; ++i) {
    if (!add(instance, plan.object.offsets[i], address) ||
        !read(address, &instance, sizeof(instance), false) || !instance)
      return {};
  }
  Target target;
  target.instance = instance;
  target.slot = plan.slot;
  if (!read(instance, &target.table, sizeof(target.table), false) ||
      !target.table ||
      !add(target.table, plan.slot * sizeof(void *), address) ||
      !read(address, &target.function, sizeof(target.function), false) ||
      !target.function)
    return {};
  return target;
}
// Each parser reads at most 32 instructions and detects direct-branch cycles.
std::optional<Plan> x64(std::uintptr_t code, const Reader &read);
std::optional<Plan> arm64(std::uintptr_t code, unsigned integer_arguments,
                          const Reader &read);
} // namespace surfxr::forwarding
#include "detail/forwarding_arm64.hpp"
#include "detail/forwarding_x64.hpp"
