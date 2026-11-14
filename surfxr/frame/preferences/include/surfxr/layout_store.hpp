#pragma once
#include "surfxr/vr_ui.hpp"
#include <string>

namespace surfxr {
enum class LayoutStoreStatus {Ok,Missing,Invalid,IoError};
const char* layout_store_status_name(LayoutStoreStatus status) noexcept;
// Linux/Frame local state only. No account, frame pixels, input state, or world
// coordinates are stored. Failed reads leave output unchanged. Missing is a
// normal first-run result; the caller should use the default layout.
LayoutStoreStatus load_layout(const std::string& path,UiLayout& output) noexcept;
// Same-directory private temporary file, fsync, then atomic rename. Existing
// files survive any failure before rename. The caller saves only on normal exit.
LayoutStoreStatus save_layout(const std::string& path,const UiLayout& layout) noexcept;
} // namespace surfxr
