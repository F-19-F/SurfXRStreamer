#pragma once
#include "surfxr/remote_settings.hpp"
#include <memory>
#include <string>

namespace surfxr {
// Validated menu values plus an SurfXR backend tag. SurfXR stores accepted PC/client
// values, never credentials, input, tracking, or runtime capabilities.
struct SettingsPreferences {
    bool surfxr_video=false;
    std::array<double,kRemoteSettingCount> values=kDefaultRemoteSettings;
    double at(SettingId id) const {return values.at(static_cast<std::size_t>(id));}
};
enum class SettingsStoreStatus {Ok,Missing,Invalid,IoError,Disabled};
const char* settings_store_status_name(SettingsStoreStatus status) noexcept;
// XDG_STATE_HOME, otherwise HOME/.local/state, then surfxr-client/settings.conf.
// Returns empty if neither environment path is absolute and usable.
std::string default_settings_path();
bool valid_settings_preferences(const SettingsPreferences& preferences) noexcept;
bool requested_settings_preferences(const RemoteSettingsSnapshot& snapshot,
                                    SettingsPreferences& output) noexcept;
// Strict, bounded, versioned numeric format; failed parsing leaves output intact.
bool parse_settings_preferences(const std::string& text,SettingsPreferences& output) noexcept;
std::string encode_settings_preferences(const SettingsPreferences& preferences);
// Empty path disables persistence. Missing/invalid/unreadable files leave output
// intact, allowing the caller to retain its defaults and command-line overrides.
SettingsStoreStatus load_settings(const std::string& path,SettingsPreferences& output) noexcept;
struct SettingsStoreStats {
    std::uint64_t submitted=0,coalesced=0,writes=0,failures=0,rejected=0;
    SettingsStoreStatus last_status=SettingsStoreStatus::Ok;
    bool enabled=false,finished=false;
};
// Submit accepted UI changes or authoritative SurfXR snapshots. Pending SurfXR fields
// retain their last accepted values. A single worker
// coalesces pending snapshots and performs private-file fsync/atomic replacement.
// Construction/load do not write. Empty path starts no worker. finish flushes the
// latest accepted snapshot and joins; call after event producers have stopped.
class SettingsStore {
public:
    explicit SettingsStore(std::string path);
    ~SettingsStore();
    SettingsStore(const SettingsStore&)=delete;
    SettingsStore& operator=(const SettingsStore&)=delete;
    bool submit(const RemoteSettingsSnapshot& snapshot) noexcept;
    SettingsStoreStats statistics() const;
    SettingsStoreStats finish() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace surfxr
