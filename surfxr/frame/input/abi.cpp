#include "surfxr/forwarding.hpp"
#include "surfxr/runtime_memory.hpp"
#include "driver.hpp"
namespace surfxr::frame_input {
std::optional<HookInterface> forwarded_interface(const HookInterface &known) {
#if defined(__aarch64__)
  const std::array<std::uintptr_t, 4> code{
      {reinterpret_cast<std::uintptr_t>(known.create),
       reinterpret_cast<std::uintptr_t>(known.update),
       reinterpret_cast<std::uintptr_t>(known.create_skeleton),
       reinterpret_cast<std::uintptr_t>(known.update_skeleton)}};
  // Public signatures: preserve x1..xN; the parser rejects stack/SIMD writes.
  const std::array<unsigned, 4> arguments{{3, 2, 7, 4}};
  std::array<forwarding::Target, 4> targets{};
  RuntimeMemory read;
  for (unsigned i = 0; i < code.size(); ++i) {
    const auto plan = forwarding::arm64(code[i], arguments[i], read);
    if (!plan)
      return {};
    const auto target = forwarding::resolve(
        *plan, reinterpret_cast<std::uintptr_t>(known.instance), read);
    unsigned char byte = 0;
    if (!target ||
        target->instance == reinterpret_cast<std::uintptr_t>(known.instance) ||
        !read(target->function, &byte, 1, true))
      return {};
    if (i && (target->instance != targets[0].instance ||
              target->table != targets[0].table))
      return {};
    for (unsigned n = 0; n < i; ++n)
      if (target->slot == targets[n].slot)
        return {};
    targets[i] = *target;
  }
  HookInterface result;
  for (unsigned i = 0; i < targets.size(); ++i)
    result.slots[i] = targets[i].slot;
  result.instance = reinterpret_cast<void *>(targets[0].instance);
  result.table = reinterpret_cast<void **>(targets[0].table);
  result.create = reinterpret_cast<CreateBoolean>(targets[0].function);
  result.update = reinterpret_cast<UpdateBoolean>(targets[1].function);
  result.create_skeleton =
      reinterpret_cast<CreateSkeleton>(targets[2].function);
  result.update_skeleton =
      reinterpret_cast<UpdateSkeleton>(targets[3].function);
  return result;
#else
  (void)known;
  return {};
#endif
}
} // namespace surfxr::frame_input
