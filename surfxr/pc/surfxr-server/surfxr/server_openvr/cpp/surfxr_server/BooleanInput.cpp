#include "BooleanInput.h"
#include "surfxr/runtime_memory.hpp"
#if defined(_WIN32) && defined(_M_X64)
namespace {
struct Image {
    HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr;
    void* view = nullptr;
    ~Image() {
        if (view)
            UnmapViewOfFile(view);
        if (mapping)
            CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }
};
const IMAGE_NT_HEADERS64* headers(const void* image) {
    const auto* bytes = static_cast<const unsigned char*>(image);
    IMAGE_DOS_HEADER dos {};
    surfxr::RuntimeMemory read;
    if (!read(reinterpret_cast<std::uintptr_t>(image), &dos, sizeof(dos))
        || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos)
        || dos.e_lfanew > 1024 * 1024)
        return nullptr;
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes + dos.e_lfanew);
    IMAGE_NT_HEADERS64 copy {};
    if (!read(reinterpret_cast<std::uintptr_t>(pe), &copy, sizeof(copy))
        || copy.Signature != IMAGE_NT_SIGNATURE
        || copy.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || copy.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || copy.FileHeader.NumberOfSections > 96)
        return nullptr;
    return pe;
}

}
#endif
namespace surfxr {
BooleanInput ResolveBooleanInput(vr::IVRDriverInput* input) {
    auto fallback = boolean_input::entry(input);
#if defined(_WIN32) && defined(_M_X64)
    if (!input)
        return fallback;
    RuntimeMemory live;
    const auto* module = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* live_headers = headers(module);
    if (!live_headers)
        return fallback;
    std::uintptr_t table = 0, create = 0, update = 0;
    if (!live(reinterpret_cast<std::uintptr_t>(input), &table, sizeof(table))
        || !live(table, &create, sizeof(create))
        || !live(table + sizeof(void*), &update, sizeof(update)))
        return fallback;
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto size = live_headers->OptionalHeader.SizeOfImage;
    if (create < base || update < base || create - base >= size || update - base >= size)
        return fallback;
    // An already transparent public adapter does not need a workaround.
    if (forwarding::x64(update, live))
        return fallback;
    fallback.adaptation = "boolean forwarding unavailable: runtime image/ABI verification failed; "
                          "public API retained";
    wchar_t path[32768];
    const auto length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768)
        return fallback;
    Image image;
    image.file = CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr
    );
    if (image.file == INVALID_HANDLE_VALUE)
        return fallback;
    image.mapping = CreateFileMappingW(
        image.file, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr
    );
    if (!image.mapping)
        return fallback;
    image.view = MapViewOfFile(image.mapping, FILE_MAP_READ, 0, 0, 0);
    if (!image.view)
        return fallback;
    const auto* disk = headers(image.view);
    // Identity comparison uses values obtained now, never a supported-build list.
    if (!disk || disk->FileHeader.TimeDateStamp != live_headers->FileHeader.TimeDateStamp
        || disk->OptionalHeader.SizeOfImage != size
        || disk->OptionalHeader.CheckSum != live_headers->OptionalHeader.CheckSum)
        return fallback;
    const forwarding::Reader original = [&](std::uintptr_t address,
                                            void* output,
                                            std::size_t count,
                                            bool code) {
        if (address < base || address - base >= size || count > size - (address - base))
            return false;
        const auto rva = address - base;
        if (code) {
            bool executable = false;
            const auto* sections = IMAGE_FIRST_SECTION(disk);
            for (unsigned i = 0; i < disk->FileHeader.NumberOfSections; ++i) {
                IMAGE_SECTION_HEADER section {};
                if (!live(
                        reinterpret_cast<std::uintptr_t>(sections + i), &section, sizeof(section)
                    ))
                    return false;
                if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE)
                    && rva >= section.VirtualAddress
                    && rva - section.VirtualAddress < section.Misc.VirtualSize
                    && count <= section.Misc.VirtualSize - (rva - section.VirtualAddress))
                    executable = true;
            }
            if (!executable)
                return false;
        }
        return live(reinterpret_cast<std::uintptr_t>(image.view) + rva, output, count, false);
    };
    return boolean_input::select(input, create, update, original, live);
#else
    return fallback;
#endif
}
}
