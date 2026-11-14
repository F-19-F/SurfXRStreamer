#include "surfxr/forwarding.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace surfxr::forwarding;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Segment {
  std::uintptr_t address;
  std::vector<unsigned char> bytes;
  bool executable = true;
};
struct Memory {
  std::vector<Segment> segments;
  bool read(std::uintptr_t address, void *out, std::size_t size,
            bool code) const {
    for (const auto &s : segments)
      if (address >= s.address && address - s.address <= s.bytes.size() &&
          size <= s.bytes.size() - (address - s.address)) {
        if (code && !s.executable)
          return false;
        std::memcpy(out, s.bytes.data() + address - s.address, size);
        return true;
      }
    return false;
  }
  Reader reader() const {
    return [this](auto a, auto b, auto c, auto d) { return read(a, b, c, d); };
  }
};
Memory words(std::initializer_list<std::uint32_t> code) {
  Memory m;
  Segment s{0x1000, {}, true};
  for (auto word : code)
    for (unsigned i = 0; i < 4; ++i)
      s.bytes.push_back(static_cast<unsigned char>(word >> (i * 8)));
  m.segments.push_back(s);
  return m;
}
void x64_checks() {
  // mov r10,rcx; mov rcx,[r10+40]; mov r11,[rcx]; jmp [r11+56]
  Memory m{{{0x1000,
             {0x49, 0x89, 0xca, 0x49, 0x8b, 0x8a, 0x28, 0, 0, 0, 0x4c, 0x8b,
              0x19, 0x49, 0xff, 0x63, 0x38}}}};
  auto p = x64(0x1000, m.reader());
  check(p && p->object.offsets[0] == 40 && p->slot == 7,
        "x64 derives offset/registers/slot");
  for (unsigned offset : {8, 24, 72, 120})
    for (unsigned slot : {0, 1, 5, 6, 17}) {
      m.segments[0].bytes[6] = static_cast<unsigned char>(offset);
      m.segments[0].bytes[16] = static_cast<unsigned char>(slot * 8);
      // Use disp32 when the positive slot cannot fit disp8.
      if (slot >= 16) {
        m.segments[0].bytes[15] = 0xa3;
        m.segments[0].bytes.resize(20);
        m.segments[0].bytes[17] = m.segments[0].bytes[18] =
            m.segments[0].bytes[19] = 0;
      } else {
        m.segments[0].bytes[15] = 0x63;
        m.segments[0].bytes.resize(17);
      }
      p = x64(0x1000, m.reader());
      check(p && p->object.offsets[0] == offset && p->slot == slot,
            "x64 layout variants");
    }
  auto code = m.segments[0].bytes;
  m.segments[0].bytes.insert(m.segments[0].bytes.begin(),
                             {0xf3, 0x0f, 0x1e, 0xfa, 0x90, 0x0f, 0x1f, 0x00});
  check(bool(x64(0x1000, m.reader())), "landing pad and NOPs");
  m.segments.push_back({0x3000, {0xe9, 0xfb, 0xdf, 0xff, 0xff}});
  check(bool(x64(0x3000, m.reader())), "relative forwarding entry");
  m.segments[1].bytes = {0xeb, 0xfe};
  check(!x64(0x3000, m.reader()), "branch cycle rejected");
  for (auto prefix :
       std::vector<std::vector<unsigned char>>{{0x89, 0xd2},
                                               {0x48, 0x89, 0x01},
                                               {0x55},
                                               {0xff, 0xd0},
                                               {0x48, 0x89, 0xc2}}) {
    m.segments[0].bytes = prefix;
    m.segments[0].bytes.insert(m.segments[0].bytes.end(), code.begin(),
                               code.end());
    check(!x64(0x1000, m.reader()),
          "narrowing/store/stack/call/argument clobber rejected");
  }
  m.segments[0].bytes = {0x48, 0x8b};
  check(!x64(0x1000, m.reader()), "truncated instruction rejected");
  m.segments[0].bytes = code;
  m.segments[0].executable = false;
  check(!x64(0x1000, m.reader()), "non-code rejected");
}
void arm_checks() {
  // The original compiler form and a different allocation/layout must both
  // parse.
  auto m = words({0xf9400400, 0xf9400003, 0xf9400463, 0xaa0303f0, 0xd61f0200});
  auto p = arm64(0x1000, 2, m.reader());
  check(p && p->object.offsets[0] == 8 && p->slot == 1,
        "ARM public forwarding");
  m = words(
      {0xd503245f, 0xd503201f, 0xf9401400, 0xf9400011, 0xf9401e30, 0xd61f0200});
  p = arm64(0x1000, 7, m.reader());
  check(p && p->object.offsets[0] == 40 && p->slot == 7,
        "ARM different offset/slot/scratch and BTI");
  for (unsigned offset : {8, 24, 72, 120})
    for (unsigned slot : {0, 1, 5, 6, 17}) {
      m = words({0xf9400000 | (offset / 8 << 10), 0xf9400011,
                 0xf9400230 | (slot << 10), 0xd61f0200});
      p = arm64(0x1000, 7, m.reader());
      check(p && p->object.offsets[0] == offset && p->slot == slot,
            "ARM layout variants");
    }
  for (auto bad :
       {0xb9400400u, 0xf9000400u, 0x94000000u, 0xaa0003f3u, 0xaa0003e1u}) {
    m = words({bad, 0xf9400400, 0xf9400003, 0xf9400463, 0xd61f0060});
    check(!arm64(0x1000, 2, m.reader()),
          "ARM narrowing/store/call/nonvolatile/argument clobber rejected");
  }
  m = words({0x14000000});
  check(!arm64(0x1000, 2, m.reader()), "ARM cycle rejected");
  m = words({0xf9400400});
  check(!arm64(0x1000, 2, m.reader()), "ARM truncated forwarding rejected");
  check(!arm64(0x1001, 2, m.reader()), "ARM alignment checked");
}
void memory_checks() {
  auto m = words({0xf9401400, 0xf9400011, 0xf9401e30, 0xd61f0200});
  const auto p = arm64(0x1000, 7, m.reader());
  check(bool(p), "plan fixture");
  const auto pointer = [&](std::uintptr_t at, std::uintptr_t value) {
    Segment s{at, std::vector<unsigned char>(sizeof(value)), false};
    std::memcpy(s.bytes.data(), &value, sizeof(value));
    m.segments.push_back(s);
  };
  pointer(0x4000 + 40, 0x5000);
  pointer(0x5000, 0x6000);
  pointer(0x6000 + 56, 0x7000);
  const auto target = resolve(*p, 0x4000, m.reader());
  check(target && target->instance == 0x5000 && target->function == 0x7000 &&
            target->slot == 7,
        "resolve current object graph");
  m.segments.pop_back();
  check(!resolve(*p, 0x4000, m.reader()), "unreadable target rejected");
  std::uintptr_t out;
  check(!add(0, -1, out) && !add(UINTPTR_MAX, 1, out),
        "address overflow rejected");
}
} // namespace
int main() {
  try {
    x64_checks();
    arm_checks();
    memory_checks();
    std::cout << "Dynamic forwarding: layout/register/slot variants, argument "
                 "preservation, bounds and rejection passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
