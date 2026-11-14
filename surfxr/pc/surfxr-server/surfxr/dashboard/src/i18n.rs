//! UI-only localization. English strings remain stable schema/preset identifiers.
use eframe::egui::{ComboBox, Ui};
use std::{cell::Cell, collections::HashMap, path::Path, sync::LazyLock};

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Language {
    #[default]
    System,
    English,
    SimplifiedChinese,
}
thread_local! { static LANGUAGE: Cell<Language> = const { Cell::new(Language::System) }; }
static CHINESE: LazyLock<HashMap<String, String>> = LazyLock::new(|| {
    let mut result = HashMap::new();
    for source in [
        include_str!("locales/zh-CN.json"),
        include_str!("locales/zh-CN-help.json"),
        include_str!("locales/zh-CN-settings-help.json"),
    ] {
        let entries: HashMap<String, String> =
            serde_json::from_str(source).expect("Valid translation catalog");
        for (key, value) in entries {
            result.insert(normalize(&key), value);
        }
    }
    result
});

pub fn language() -> Language {
    match LANGUAGE.get() {
        Language::System => system_language(),
        selected => selected,
    }
}
fn locale_language(locale: &str) -> Language {
    if locale.split(['-', '_', '.', '@']).next().is_some_and(|v| v.eq_ignore_ascii_case("zh")) {
        Language::SimplifiedChinese
    } else { Language::English }
}
fn system_language() -> Language {
    #[cfg(windows)]
    {
        #[link(name = "kernel32")]
        unsafe extern "system" { fn GetUserDefaultLocaleName(name: *mut u16, length: i32) -> i32; }
        let mut name = [0u16; 85];
        let count = unsafe { GetUserDefaultLocaleName(name.as_mut_ptr(), name.len() as i32) };
        if count > 1 { return locale_language(&String::from_utf16_lossy(&name[..count as usize - 1])); }
    }
    for key in ["LC_ALL", "LC_MESSAGES", "LANG"] {
        if let Ok(value) = std::env::var(key) { if !value.is_empty() { return locale_language(&value); } }
    }
    Language::English
}
fn normalize(text: &str) -> String {
    text.split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .to_lowercase()
}
pub fn set_language(value: Language) {
    LANGUAGE.set(value);
}
pub fn tr(text: &str) -> String {
    if language() == Language::English {
        return text.into();
    }
    if let Some(value) = CHINESE.get(&normalize(text)) {
        return value.clone();
    }
    if let Some((tier, size)) = text.split_once(" - ") {
        if size.ends_with(" / eye") {
            return format!("{} - {} / 眼", tr(tier), size.trim_end_matches(" / eye"));
        }
    }
    text.into()
}

// Translate the UI summary of known diagnostics, retaining error details verbatim.
// Stored logs and message identity used for dismissal are never rewritten.
pub fn notification(text: &str) -> String {
    let translated = tr(text);
    if translated != text { return translated; }
    if let Some((prefix, detail)) = text.split_once(": ") {
        let translated = tr(prefix);
        if translated != prefix { return format!("{translated}: {detail}"); }
    }
    text.into()
}

pub fn search_matches(text: &str, query: &str) -> bool {
    // The search index retains English and original keys. Add translated terms
    // at query time so switching languages does not rebuild or mutate settings.
    let text = normalize(text);
    let query = query.to_lowercase();
    query.split_whitespace().all(|word| {
        text.contains(word)
            || (language() == Language::SimplifiedChinese
                && CHINESE
                    .iter()
                    .any(|(en, zh)| zh.contains(word) && text.contains(en)))
    })
}

pub fn selector(ui: &mut Ui) -> bool {
    let mut selected = LANGUAGE.get();
    ComboBox::from_id_salt("dashboard_language")
        .selected_text(match selected {
            Language::System => "System / 系统语言",
            Language::English => "English",
            Language::SimplifiedChinese => "简体中文",
        })
        .show_ui(ui, |ui| {
            ui.selectable_value(&mut selected, Language::System, "System / 系统语言");
            ui.selectable_value(&mut selected, Language::English, "English");
            ui.selectable_value(&mut selected, Language::SimplifiedChinese, "简体中文");
        });
    let changed = selected != LANGUAGE.get();
    set_language(selected);
    changed
}

pub fn settings_selector(ui: &mut Ui) {
    if selector(ui) {
        #[cfg(not(target_arch = "wasm32"))]
        if let Err(error) = save(
            &crate::get_filesystem_layout()
                .session()
                .with_file_name("dashboard-ui.json"),
        ) {
            surfxr_common::warn!("Could not save Dashboard language: {error}");
        }
        ui.ctx().request_repaint();
    }
}

pub fn load(path: &Path) {
    let value = std::fs::read(path)
        .ok()
        .and_then(|bytes| serde_json::from_slice::<serde_json::Value>(&bytes).ok());
    set_language(match value.as_ref().and_then(|v| v["language"].as_str()) {
        Some("zh-CN") => Language::SimplifiedChinese,
        Some("en") => Language::English,
        _ => Language::System,
    });
}

pub fn save(path: &Path) -> std::io::Result<()> {
    let value = match LANGUAGE.get() {
        Language::System => "system",
        Language::English => "en",
        Language::SimplifiedChinese => "zh-CN",
    };
    std::fs::write(
        path,
        serde_json::to_vec_pretty(&serde_json::json!({"language": value})).unwrap(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn language_switch_preserves_unknown_values_and_bilingual_search() {
        set_language(Language::SimplifiedChinese);
        assert_eq!(tr("Quick setup"), "快捷设置");
        assert_eq!(
            tr("Steam Streaming Microphone"),
            "Steam Streaming Microphone"
        );
        assert_eq!(tr("my-headset.local"), "my-headset.local");
        assert_eq!(notification("Failed to set session value: codec=AV1"), "保存设置失败: codec=AV1");
        assert_eq!(notification("unknown diagnostic: 0x1234"), "unknown diagnostic: 0x1234");
        assert!(search_matches("encoder preset speed", "编码"));
        assert!(search_matches("encoder preset speed", "encoder"));
        assert!(!search_matches("frame rate", "麦克风"));
        set_language(Language::English);
        assert_eq!(tr("Quick setup"), "Quick setup");
    }
    #[test]
    fn language_preferences_round_trip_and_invalid_file_falls_back() {
        let path =
            std::env::temp_dir().join(format!("surfxr-ui-language-{}.json", std::process::id()));
        std::fs::OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&path)
            .unwrap();
        struct Cleanup(std::path::PathBuf);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                std::fs::remove_file(&self.0).ok();
                set_language(Language::English);
            }
        }
        let _cleanup = Cleanup(path.clone());
        set_language(Language::SimplifiedChinese);
        save(&path).unwrap();
        set_language(Language::English);
        load(&path);
        assert_eq!(language(), Language::SimplifiedChinese);
        std::fs::write(&path, b"broken json").unwrap();
        load(&path);
        assert_eq!(language(), system_language());
    }

    #[test]
    fn system_locale_uses_supported_language_or_english() {
        for locale in ["zh-CN", "zh_TW.UTF-8", "ZH-hans", "zh"] {
            assert_eq!(locale_language(locale), Language::SimplifiedChinese);
        }
        for locale in ["en-US", "ja-JP", "de_DE", "", "zhong"] {
            assert_eq!(locale_language(locale), Language::English);
        }
    }
}
