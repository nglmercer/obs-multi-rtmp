#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QString>
#include <QWidget>

#include "output-config.h"

// Platform-provided RTMP ingest. server/streamKey are secrets and must
// never be serialized or logged.
struct PlatformEndpoint {
    std::string id;
    std::string server;
    std::string streamKey;
};

// A remote account/endpoint session attached to one output target. The
// local OBS output remains owned by PushWidget; implementations own only
// provider resources and report state through the supplied callbacks.
class PlatformSession {
public:
    struct Callbacks {
        std::function<void(const QString &)> showMessage;
        std::function<void()> stateChanged;
        std::function<void()> endpointReady;
        std::function<void()> endpointEnded;
        std::function<void()> requestLocalStop;
        std::function<void()> startCancelled;
    };

    virtual ~PlatformSession() = default;

    virtual bool HasEndpoint() const = 0;
    virtual bool IsEndpointReady() const = 0;
    virtual bool IsStarting() const = 0;
    virtual bool IsCreating() const = 0;
    virtual bool IsEnding() const = 0;
    virtual bool IsBusy() const = 0;
    virtual bool HasError() const = 0;
    virtual bool IsCreationUncertain() const = 0;
    virtual std::optional<PlatformEndpoint> Endpoint() const = 0;

    virtual bool CreateEndpoint() = 0;
    virtual void EndEndpoint() = 0;
    virtual void CancelPendingWork() = 0;
    virtual void Shutdown() = 0;
    virtual void UpdateConfig(OutputTargetConfigPtr config) = 0;
};

// Provider-owned target settings tab. It only mutates the working target;
// the containing editor decides when Save() is called.
class PlatformTargetEditor : public QWidget {
public:
    explicit PlatformTargetEditor(QWidget *parent = nullptr) : QWidget(parent) {}
    ~PlatformTargetEditor() override = default;
    virtual void Load(const OutputTargetConfig &target) = 0;
    virtual void Save(OutputTargetConfig &target) = 0;
    virtual void SetEnabledChangedCallback(std::function<void()> callback) = 0;
};

struct PlatformEncoderPreset {
    const char *videoCodec = nullptr;
    const char *videoEncoderId = nullptr;
    const char *audioCodec = nullptr;
    const char *audioEncoderId = nullptr;
};

enum class PlatformMessage {
    EndpointReady,
    StartingRestream,
    RestreamStopped,
    SessionExpired,
};

class PlatformProvider {
public:
    virtual ~PlatformProvider() = default;

    virtual std::string Id() const = 0;
    virtual QString TargetTabTitle() const = 0;
    virtual QString CreateEndpointText() const = 0;
    virtual QString EndEndpointText() const = 0;
    virtual QString Message(PlatformMessage message) const = 0;

    virtual bool SupportsProtocol(const std::string &protocol) const = 0;
    virtual bool IsTargetEnabled(const OutputTargetConfig &target) const = 0;
    virtual std::optional<PlatformEncoderPreset>
    IndependentEncoderPreset(const OutputTargetConfig &target) const = 0;

    virtual PlatformTargetEditor *CreateTargetEditor(OutputTargetConfigPtr target,
                                                     QWidget *parent = nullptr) const = 0;
    virtual std::unique_ptr<PlatformSession> CreateSession(
        OutputTargetConfigPtr target,
        PlatformSession::Callbacks callbacks,
        QWidget *parent = nullptr) const = 0;

    virtual void RestoreSession() = 0;
    virtual void MigrateLegacyConfiguration() = 0;
    virtual void Shutdown() = 0;
};

const std::vector<PlatformProvider *> &PlatformProviders();
void RegisterPlatformProvider(PlatformProvider *provider);
PlatformProvider *PlatformProviderForTarget(const OutputTargetConfig &target);
