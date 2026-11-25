#pragma once

#include <memory>
#include <optional>
#include <string>

#include <QObject>
#include <QPointer>
#include <QWidget>

#include "platform/platform-provider.h"
#include "streamlabs-tiktok.h"

class StreamlabsTikTokClient;

class TikTokPlatformSession final : public QObject, public PlatformSession {
public:
    enum class State {
        Idle,
        Authenticating,
        CreatingEndpoint,
        EndpointReady,
        EndingEndpoint,
        Error,
    };

    TikTokPlatformSession(OutputTargetConfigPtr config,
                          PlatformSession::Callbacks callbacks,
                          QWidget *parent = nullptr);
    ~TikTokPlatformSession() override;

    bool HasEndpoint() const override;
    bool IsEndpointReady() const override;
    bool IsStarting() const override;
    bool IsCreating() const override;
    bool IsEnding() const override;
    bool IsBusy() const override;
    bool HasError() const override;
    bool IsCreationUncertain() const override;
    std::optional<PlatformEndpoint> Endpoint() const override;

    bool CreateEndpoint() override;
    void EndEndpoint() override;
    void CancelPendingWork() override;
    void Shutdown() override;
    void UpdateConfig(OutputTargetConfigPtr config) override;

    // Test seam: submits a creation request with an already authenticated
    // client instead of entering the interactive login path first.
    bool BeginEndpointRequest(std::shared_ptr<StreamlabsTikTokClient> client);

private:
    struct EndpointSession {
        State state = State::Idle;
        std::string streamId;
        std::string server;
        std::string streamKey;
        std::shared_ptr<StreamlabsTikTokClient> client;

        bool HasEndpoint() const
        {
            return !streamId.empty() && !server.empty() && !streamKey.empty();
        }
        void Reset() { *this = EndpointSession{}; }
    };

    static const char *FailureText(TikTokFailure failure, bool resolvingCategory);
    static void SubmitCleanup(std::shared_ptr<StreamlabsTikTokClient> client,
                              std::string streamId, bool liveCounted);

    void ShowMessage(const QString &message) const;
    void NotifyStateChanged() const;
    void NotifyStartCancelled() const;
    void OnPrepared(uint64_t generation, std::shared_ptr<StreamlabsTikTokClient> client,
                    const TikTokCategoryResult &categoryResult,
                    const TikTokStartResult &startResult);

    OutputTargetConfigPtr config_;
    PlatformSession::Callbacks callbacks_;
    QPointer<QWidget> uiParent_;
    EndpointSession tiktokSession_;
    uint64_t tiktokRequestGen_ = 0;
    bool endAfterCreate_ = false;
    bool endpointCreationUncertain_ = false;

    friend struct TikTokLifecycleTest;
    friend struct StandaloneTest;
};
