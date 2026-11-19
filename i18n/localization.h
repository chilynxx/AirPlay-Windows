#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace ap::i18n {

enum class Language : int {
    English    = 0,
    Vietnamese = 1
};

enum class Str : uint16_t {
    AppName = 0,
    YourSpace,
    NavHome,
    NavMusic,
    NavMirror,
    NavUsb,
    NavAndroid,
    NavSettings,

    StatusDeviceConnected,
    StatusReceiverAvailable,
    StatusReadyToReceive,
    StatusConnected,
    StatusCheckFirewall,
    StatusConnectedAudio,
    StatusConnectedMirror,
    StatusConnectedUsb,
    StatusConnectedAndroid,
    StatusWaitingForAirPlay,
    StatusAutoReconnecting,
    StatusAutoReconnectingDetail,
    StatusAutoRecoveredNotice,

    BtnDisconnect,
    BtnReconnect,
    BtnRecord,
    BtnStopRecording,
    BtnScreenshot,
    BtnFullscreen,
    BtnHideInterface,
    BtnRotation,
    BtnMute,
    BtnUnmute,
    BtnThemeDark,
    BtnThemeLight,
    BtnLangSwitchTooltip,

    DockModeWifi,
    DockModeUsb,
    DockPlatformIos,
    DockPlatformAndroid,
    DockTooltipMode,
    DockTooltipPlatform,
    DockTooltipReconnect,
    DockTooltipRotate,
    DockTooltipRecord,
    DockTooltipScreenshot,
    DockTooltipFullscreen,
    DockTooltipMute,
    DockTooltipVolume,

    PageTitleHome,
    PageTitleMusic,
    PageTitleMirror,
    HeroHeading1,
    HeroHeading2,
    HeroSubtitleIosWifi,
    HeroSubtitleIosUsb,
    HeroSubtitleAndroidWifi,
    HeroSubtitleAndroidUsb,

    CardSelectOnDevice,
    CardReceiverSettingsTooltip,
    CardAndroidQuickConnect,
    CardAndroidConnectBtn,
    CardAndroidIpPlaceholder,

    Step1IosWifiTitle,
    Step1IosWifiDesc,
    Step2IosWifiTitle,
    Step2IosWifiDesc,
    Step3IosWifiTitle,
    Step3IosWifiDesc,

    Step1IosUsbTitle,
    Step1IosUsbDesc,
    Step2IosUsbTitle,
    Step2IosUsbDesc,
    Step3IosUsbTitle,
    Step3IosUsbDesc,

    Step1AndroidWifiTitle,
    Step1AndroidWifiDesc,
    Step2AndroidWifiTitle,
    Step2AndroidWifiDesc,
    Step3AndroidWifiTitle,
    Step3AndroidWifiDesc,

    Step1AndroidUsbTitle,
    Step1AndroidUsbDesc,
    Step2AndroidUsbTitle,
    Step2AndroidUsbDesc,
    Step3AndroidUsbTitle,
    Step3AndroidUsbDesc,

    UsbBannerIosUntrustedTitle,
    UsbBannerIosUntrustedDesc,
    UsbBannerIosTrustedTitle,
    UsbBannerIosTrustedDesc,
    UsbBannerAndroidUnauthTitle,
    UsbBannerAndroidUnauthDesc,
    UsbBannerAndroidReadyTitle,
    UsbBannerAndroidReadyDesc,
    UsbBannerScanningTitle,
    UsbBannerScanningDesc,

    HelpLinkNotShowing,
    HelpPopupTitle,
    HelpCloseTooltip,
    HelpStep1Title,
    HelpStep1Desc,
    HelpStep2Title,
    HelpStep2Desc,
    HelpStep3Title,
    HelpStep3Desc,
    HelpFooterNote,
    FooterLocalNetwork,

    FirewallBannerAllowTitle,
    FirewallBannerBlockedTitle,
    FirewallBannerUnavailableTitle,
    FirewallBannerAllowDetail,
    FirewallBtnWaiting,
    FirewallBtnAllow,

    MusicNowPlaying,
    MusicPaused,
    MusicProgressTooltip,
    MusicPrevTooltip,
    MusicPlayTooltip,
    MusicPauseTooltip,
    MusicNextTooltip,
    MusicSendingCmd,
    MusicRemoteUnavailable,
    MusicCmdFailed,
    MusicPlayingFooter,

    DecodeGpuNote,
    DecodeCpuNote,
    ScreenshotSavedToast,

    SettingsTitle,
    SettingsCloseTooltip,
    SettingsSectionLanguageTheme,
    SettingsLanguageLabel,
    SettingsThemeLabel,
    SettingsThemeDarkOption,
    SettingsThemeLightOption,
    SettingsDeviceFrameLabel,
    SettingsSectionReceiverName,
    SettingsApplyName,
    SettingsNameInvalidHint,
    SettingsNameSavedHint,
    SettingsSectionResolution,
    SettingsCustomRes,
    SettingsWidth,
    SettingsHeight,
    SettingsResRangeHint,
    SettingsResMaxHint,
    SettingsResReceivedFmt,
    SettingsAppliesNextConn,
    SettingsAppliesLive,
    SettingsSectionDecoder,
    SettingsUseGpu,
    SettingsGpuHint,
    SettingsCpuHint,
    SettingsVsync,
    SettingsVsyncOnHint,
    SettingsVsyncOffHint,
    SettingsSectionStability,
    SettingsAutoRecoverLabel,
    SettingsAutoRecoverHint,
    SettingsSectionRecording,
    SettingsFolder,
    SettingsBrowse,
    SettingsBrowseDialogTitle,
    SettingsVideoCodec,
    SettingsEncoder,
    SettingsBitrateMbps,
    SettingsBitrateHint,
    SettingsRecNextHint,
    SettingsRecMp4Hint,
    SettingsStartRecording,
    SettingsSectionAdvanced,
    SettingsShowDiagnostics,
    SettingsSectionRotation,
    SettingsRotationAuto,
    SettingsRotation0,
    SettingsRotation90,
    SettingsRotation180,
    SettingsRotation270,
    SettingsRotationHint,
    SettingsSectionFramerate,
    SettingsFpsCap,
    SettingsFpsHint,
    SettingsSectionCompression,
    SettingsAllowHevc,
    SettingsHevcOnHint,
    SettingsHevcOffHint,
    SettingsSectionNetwork,
    SettingsSectionShortcuts,
    SettingsShortcutFullscreen,
    SettingsShortcutHideUi,
    SettingsShortcutRecord,
    SettingsShortcutScreenshot,
    SettingsShortcutEsc,
    SettingsResetHint,
    SettingsResetBtn,
    SettingsResetDone,

    DiagStateIdle,
    DiagStateConnected,
    DiagStateStreaming,
    DiagStatePaused,
    DiagStateReconnecting,

    TrayShow,
    TrayHide,
    TrayExit,
    SingleInstanceError,

    Count
};

class LocalizationManager {
public:
    static LocalizationManager& instance();

    void set_language(Language lang) noexcept {
        lang_.store(static_cast<int>(lang), std::memory_order_release);
    }

    Language language() const noexcept {
        return static_cast<Language>(lang_.load(std::memory_order_acquire));
    }

    void toggle_language() noexcept {
        const Language cur = language();
        set_language(cur == Language::English ? Language::Vietnamese : Language::English);
    }

    const char* get(Str id) const noexcept;
    std::wstring get_wide(Str id) const;

    static std::wstring utf8_to_wide(const std::string& utf8);
    static std::string wide_to_utf8(const std::wstring& wide);

    static const char* language_code(Language lang) noexcept {
        return lang == Language::Vietnamese ? "vi" : "en";
    }

    static Language parse_language(const std::string& code) noexcept {
        if (code == "vi" || code == "VI" || code == "1") return Language::Vietnamese;
        return Language::English;
    }

private:
    LocalizationManager() = default;
    std::atomic<int> lang_{static_cast<int>(Language::English)};
};

inline const char* tr(Str id) noexcept {
    return LocalizationManager::instance().get(id);
}

inline std::wstring tr_w(Str id) {
    return LocalizationManager::instance().get_wide(id);
}

inline Language current_language() noexcept {
    return LocalizationManager::instance().language();
}

} // namespace ap::i18n
