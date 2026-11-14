#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <link.h>
#include <unistd.h>
#endif
namespace surfxr {
// Reads our own process through the OS: malformed runtime pointers cannot turn
// adaptation discovery into an unchecked memcpy. Code must be executable.
struct RuntimeMemory {
  bool operator()(std::uintptr_t address, void *output, std::size_t size,
                  bool code = false) const {
    if (!address || !size ||
        size > std::numeric_limits<std::uintptr_t>::max() - address)
      return false;
#ifdef _WIN32
    if (code) {
      MEMORY_BASIC_INFORMATION region{};
      if (!VirtualQuery(reinterpret_cast<void *>(address), &region,
                        sizeof(region)) ||
          region.State != MEM_COMMIT ||
          (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
          !(region.Protect &
            (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
             PAGE_EXECUTE_WRITECOPY)) ||
          address + size >
              reinterpret_cast<std::uintptr_t>(region.BaseAddress) +
                  region.RegionSize)
        return false;
    }
    SIZE_T count = 0;
    return ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<void *>(address), output, size,
                             &count) &&
           count == size;
#elif defined(__linux__)
    if (code) {
      struct Search {
        std::uintptr_t begin, end;
        bool found = false;
      } search{address, address + size};
      dl_iterate_phdr(
          [](dl_phdr_info *image, std::size_t, void *context) {
            auto &query = *static_cast<Search *>(context);
            for (unsigned i = 0; i < image->dlpi_phnum; ++i) {
              const auto &segment = image->dlpi_phdr[i];
              if (segment.p_type != PT_LOAD || !(segment.p_flags & PF_X))
                continue;
              const auto begin = image->dlpi_addr + segment.p_vaddr;
              if (query.begin >= begin && query.end >= query.begin &&
                  query.end - begin <= segment.p_memsz) {
                query.found = true;
                return 1;
              }
            }
            return 0;
          },
          &search);
      if (!search.found)
        return false;
    }
    if (address >
        static_cast<std::uintptr_t>(std::numeric_limits<off_t>::max()))
      return false;
    const int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      return false;
    const auto count = pread(fd, output, size, static_cast<off_t>(address));
    close(fd);
    return count == static_cast<ssize_t>(size);
#else
    (void)output;
    (void)code;
    return false;
#endif
  }
};
} // namespace surfxr
