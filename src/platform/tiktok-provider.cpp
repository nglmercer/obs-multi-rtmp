#include "pch.h"

#include "platform/platform-provider.h"
#include "platform/tiktok-session.h"
#include "platform/tiktok-target-editor.h"
#include "tiktok/auth-manager.h"

namespace {

class TikTokPlatformProvider final : public PlatformProvider {
public:
    std::string Id() const override { return "tiktok"; }

    QString TargetTabTitle() const override
    {
        return QString::fromUtf8(obs_module_text("Tab.TikTok"));
    }

    QString CreateEndpointText() const override
    {
        return QString::fromUtf8(obs_module_text("TikTok.CreateEndpoint"));
    }

    QString EndEndpointText() const override
    {
        return QString::fromUtf8(obs_module_text("TikTok.EndLive"));
    }

    QString Message(PlatformMessage message) const override
    {
        const char *key = "TikTok.EndpointReady";
        switch (message) {
        case PlatformMessage::EndpointReady:
            key = "TikTok.EndpointReady";
            break;
        case PlatformMessage::StartingRestream:
            key = "TikTok.StartingRestream";
            break;
        case PlatformMessage::RestreamStopped:
            key = "TikTok.RestreamStopped";
            break;
        case PlatformMessage::SessionExpired:
            key = "TikTok.SessionExpired";
            break;
        }
        return QString::fromUtf8(obs_module_text(key));
    }

    bool SupportsProtocol(const std::string &protocol) const override
    {
        return protocol == "RTMP";
    }

    bool IsTargetEnabled(const OutputTargetConfig &target) const override
    {
        return target.tiktok.enabled;
    }

    std::optional<PlatformEncoderPreset>
    IndependentEncoderPreset(const OutputTargetConfig &target) const override
    {
        if (!IsTargetEnabled(target))
            return std::nullopt;
        return PlatformEncoderPreset{"h264", "obs_x264", "aac", "ffmpeg_aac"};
    }

    PlatformTargetEditor *CreateTargetEditor(OutputTargetConfigPtr target,
                                             QWidget *parent) const override
    {
        return new TikTokTargetEditor(std::move(target), parent);
    }

    std::unique_ptr<PlatformSession> CreateSession(
        OutputTargetConfigPtr target,
        PlatformSession::Callbacks callbacks,
        QWidget *parent) const override
    {
        return std::make_unique<TikTokPlatformSession>(
            std::move(target), std::move(callbacks), parent);
    }

    void RestoreSession() override
    {
        TikTokAuthManager::instance().restoreSession();
    }

    void MigrateLegacyConfiguration() override
    {
        TikTokAuthManager::instance().migrateLegacyTokens(ConsumeLegacyTikTokTokens());
    }

    void Shutdown() override
    {
        TikTokAuthManager::instance().shutdown();
    }
};

TikTokPlatformProvider &TikTokProvider()
{
    static auto *provider = new TikTokPlatformProvider();
    return *provider;
}

const bool registeredTikTokProvider = [] {
    RegisterPlatformProvider(&TikTokProvider());
    return true;
}();

} // namespace
