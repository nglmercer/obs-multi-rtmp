#include "pch.h"

#include "platform/tiktok-session.h"

#include <QCoreApplication>
#include <QMessageBox>

#include "output-config.h"
#include "tiktok/auth-manager.h"
#include "tiktok/cleanup.h"
#include "tiktok/streamlabs-client.h"
#include "tiktok/worker.h"

TikTokPlatformSession::TikTokPlatformSession(
    OutputTargetConfigPtr config,
    PlatformSession::Callbacks callbacks,
    QWidget *parent)
    : QObject(parent)
    , config_(std::move(config))
    , callbacks_(std::move(callbacks))
    , uiParent_(parent)
{
    auto &auth = TikTokAuthManager::instance();
    QObject::connect(&auth, &TikTokAuthManager::stateChanged, this, [this]() {
        if (tiktokSession_.HasEndpoint() &&
            tiktokSession_.state != State::EndingEndpoint &&
            TikTokAuthManager::instance().status() == TikTokAuthManager::Status::SessionExpired) {
            tiktokSession_.state = State::Error;
            if (callbacks_.requestLocalStop)
                callbacks_.requestLocalStop();
            ShowMessage(QString::fromUtf8(obs_module_text("TikTok.SessionExpired")));
            NotifyStateChanged();
        }
    });
    QObject::connect(&auth, &TikTokAuthManager::loginSucceeded, this, [this]() {
        if (tiktokSession_.state != State::Authenticating)
            return;
        tiktokSession_.state = State::Idle;
        CreateEndpoint();
        if (!IsStarting())
            NotifyStartCancelled();
    });
    QObject::connect(&auth, &TikTokAuthManager::loginFailed, this, [this](const QString &error) {
        if (tiktokSession_.state != State::Authenticating)
            return;
        tiktokSession_.state = State::Error;
        NotifyStartCancelled();
        ShowMessage(error);
    });
}

TikTokPlatformSession::~TikTokPlatformSession()
{
    Shutdown();
}

bool TikTokPlatformSession::HasEndpoint() const
{
    return tiktokSession_.HasEndpoint();
}

bool TikTokPlatformSession::IsEndpointReady() const
{
    return tiktokSession_.HasEndpoint() && tiktokSession_.state == State::EndpointReady;
}

bool TikTokPlatformSession::IsStarting() const
{
    return tiktokSession_.state == State::Authenticating ||
           tiktokSession_.state == State::CreatingEndpoint;
}

bool TikTokPlatformSession::IsCreating() const
{
    return tiktokSession_.state == State::CreatingEndpoint;
}

bool TikTokPlatformSession::IsEnding() const
{
    return tiktokSession_.state == State::EndingEndpoint;
}

bool TikTokPlatformSession::IsBusy() const
{
    return tiktokSession_.state == State::Authenticating ||
           tiktokSession_.state == State::CreatingEndpoint ||
           tiktokSession_.state == State::EndingEndpoint;
}

bool TikTokPlatformSession::HasError() const
{
    return tiktokSession_.state == State::Error;
}

bool TikTokPlatformSession::IsCreationUncertain() const
{
    return endpointCreationUncertain_;
}

std::optional<PlatformEndpoint> TikTokPlatformSession::Endpoint() const
{
    if (!tiktokSession_.HasEndpoint())
        return std::nullopt;
    return PlatformEndpoint{tiktokSession_.streamId, tiktokSession_.server,
                            tiktokSession_.streamKey};
}

void TikTokPlatformSession::ShowMessage(const QString &message) const
{
    if (callbacks_.showMessage)
        callbacks_.showMessage(message);
}

void TikTokPlatformSession::NotifyStateChanged() const
{
    if (callbacks_.stateChanged)
        callbacks_.stateChanged();
}

void TikTokPlatformSession::NotifyStartCancelled() const
{
    if (callbacks_.startCancelled)
        callbacks_.startCancelled();
    NotifyStateChanged();
}

const char *TikTokPlatformSession::FailureText(TikTokFailure failure, bool resolvingCategory)
{
    switch (failure) {
    case TikTokFailure::Auth:
    case TikTokFailure::Forbidden:
        return obs_module_text("Error.TikTokAuth");
    case TikTokFailure::RateLimited:
        return obs_module_text("Error.TikTokRateLimit");
    case TikTokFailure::Network:
    case TikTokFailure::Timeout:
        return obs_module_text("Error.TikTokNetwork");
    case TikTokFailure::None:
    case TikTokFailure::BadRequest:
    case TikTokFailure::NotFound:
    case TikTokFailure::Server:
    case TikTokFailure::InvalidResponse:
    default:
        return resolvingCategory ? obs_module_text("Error.TikTokCategory")
                                 : obs_module_text("Error.TikTokCreateLive");
    }
}

void TikTokPlatformSession::SubmitCleanup(std::shared_ptr<StreamlabsTikTokClient> client,
                                          std::string streamId, bool liveCounted)
{
    if (!client || streamId.empty())
        return;
    if (!GlobalCleanupLedger().claim(streamId))
        return;
    auto runEnd = [client, streamId, liveCounted]() {
        TikTokNetworkCancel cleanupScope(nullptr);
        std::string error;
        if (!client->EndStream(streamId, &error))
            blog(LOG_WARNING, TAG "[TikTokStream] Failed to end remote endpoint");
        else
            blog(LOG_INFO, TAG "TikTok LIVE session ended.");
        if (liveCounted)
            TikTokLiveRegistry::removeLive();
    };
    auto job = TikTokWorkerService::instance().submit(
        nullptr,
        [runEnd](const std::shared_ptr<TikTokJobContext> &) { runEnd(); },
        TikTokWorkerService::Done(), true, true);
    if (!job) {
        runEnd();
    }
}

bool TikTokPlatformSession::CreateEndpoint()
{
    if (endpointCreationUncertain_) {
        ShowMessage(QString::fromUtf8(obs_module_text("TikTok.CreationUncertain")));
        return false;
    }
    if (tiktokSession_.HasEndpoint() ||
        tiktokSession_.state == State::CreatingEndpoint ||
        tiktokSession_.state == State::EndingEndpoint ||
        tiktokSession_.state == State::Authenticating)
        return false;
    if (config_->protocol != "RTMP") {
        ShowMessage(QString::fromUtf8(obs_module_text("Error.TikTokRequiresRTMP")));
        return false;
    }
    auto client = TikTokAuthManager::instance().client();
    if (!client || !TikTokAuthManager::instance().isAuthenticated()) {
        tiktokSession_.state = State::Authenticating;
        ShowMessage(QString::fromUtf8(obs_module_text("TikTok.LoggingIn")));
        NotifyStateChanged();
        TikTokAuthManager::instance().login(uiParent_);
        return false;
    }
    return BeginEndpointRequest(std::move(client));
}

bool TikTokPlatformSession::BeginEndpointRequest(std::shared_ptr<StreamlabsTikTokClient> client)
{
    if (tiktokSession_.HasEndpoint() ||
        tiktokSession_.state == State::CreatingEndpoint ||
        tiktokSession_.state == State::EndingEndpoint)
        return false;

    const std::string title = config_->tiktok.title;
    const std::string category = config_->tiktok.category;
    const TikTokAudience audience = config_->tiktok.audience;
    struct Prepared {
        TikTokCategoryResult category;
        TikTokStartResult start;
        std::shared_ptr<StreamlabsTikTokClient> client;
        bool adopted = false;
        ~Prepared()
        {
            if (start.success && !adopted)
                SubmitCleanup(client, start.streamId, false);
        }
    };
    auto outcome = std::make_shared<Prepared>();
    outcome->client = client;
    const auto gen = ++tiktokRequestGen_;
    tiktokSession_.state = State::CreatingEndpoint;
    ShowMessage(QString::fromUtf8(obs_module_text("TikTok.Preparing")));
    NotifyStateChanged();
    blog(LOG_INFO, TAG "[TikTokStream] Creating remote endpoint");

    auto job = TikTokWorkerService::instance().submit(
        this,
        [client, title, category, audience, outcome](const std::shared_ptr<TikTokJobContext> &job) {
            outcome->category = client->ResolveCategory(category);
            if (outcome->category.success && !job->cancelled.load(std::memory_order_relaxed)) {
                outcome->start =
                    client->StartStream(title, outcome->category.categoryId, audience);
            }
        },
        [client, outcome, gen](const std::shared_ptr<TikTokJobContext> &job) {
            auto *self = dynamic_cast<TikTokPlatformSession *>(job->receiver.data());
            if (self == nullptr)
                return;
            outcome->adopted = true;
            self->OnPrepared(gen, client, outcome->category, outcome->start);
        },
        true);
    if (!job) {
        blog(LOG_WARNING, TAG "TikTok preparation rejected during shutdown for '%s'.",
             config_->name.c_str());
        tiktokSession_.state = State::Error;
        NotifyStartCancelled();
        return false;
    }
    return true;
}

void TikTokPlatformSession::OnPrepared(uint64_t generation,
                                       std::shared_ptr<StreamlabsTikTokClient> client,
                                       const TikTokCategoryResult &categoryResult,
                                       const TikTokStartResult &startResult)
{
    if (generation != tiktokRequestGen_ ||
        tiktokSession_.state != State::CreatingEndpoint) {
        if (startResult.success)
            SubmitCleanup(client, startResult.streamId, false);
        return;
    }

    if (!categoryResult.success) {
        tiktokSession_.state = State::Error;
        endAfterCreate_ = false;
        NotifyStartCancelled();
        switch (ClassifyCredentialResult(categoryResult)) {
        case CredentialValidation::InvalidCredential:
            TikTokAuthManager::instance().handleAuthFailure(client->authEpoch());
            break;
        case CredentialValidation::ValidLiveForbidden:
            TikTokAuthManager::instance().noteLiveAccessDenied(client->authEpoch());
            break;
        default:
            break;
        }
        blog(LOG_WARNING, TAG "TikTok category lookup failed for '%s'.", config_->name.c_str());
        QString message = QString::fromUtf8(FailureText(categoryResult.failure, true));
        if (!categoryResult.error.empty())
            message += "\n" + QString::fromUtf8(categoryResult.error.c_str());
        ShowMessage(message);
        return;
    }
    if (!config_->tiktok.category.empty() && !categoryResult.matchedExact)
        blog(LOG_INFO, TAG "TikTok category '%s' not found, streaming under Other.",
             config_->tiktok.category.c_str());

    if (!startResult.success) {
        tiktokSession_.state = State::Error;
        endAfterCreate_ = false;
        NotifyStartCancelled();
        switch (ClassifyCredentialResult(startResult)) {
        case CredentialValidation::InvalidCredential:
            TikTokAuthManager::instance().handleAuthFailure(client->authEpoch());
            break;
        case CredentialValidation::ValidLiveForbidden:
            TikTokAuthManager::instance().noteLiveAccessDenied(client->authEpoch());
            break;
        default:
            break;
        }
        endpointCreationUncertain_ = startResult.failure == TikTokFailure::Network ||
            startResult.failure == TikTokFailure::Timeout || startResult.failure == TikTokFailure::Server ||
            startResult.failure == TikTokFailure::InvalidResponse;
        blog(LOG_WARNING, TAG "TikTok LIVE creation failed for '%s'.", config_->name.c_str());
        QString message = QString::fromUtf8(FailureText(startResult.failure, false));
        if (!startResult.error.empty())
            message += "\n" + QString::fromUtf8(startResult.error.c_str());
        ShowMessage(message);
        if (endpointCreationUncertain_)
            ShowMessage(QString::fromUtf8(obs_module_text("TikTok.CreationUncertain")));
        return;
    }

    tiktokSession_.streamId = startResult.streamId;
    tiktokSession_.server = startResult.server;
    tiktokSession_.streamKey = startResult.key;
    tiktokSession_.client = client;
    tiktokSession_.state = State::EndpointReady;
    TikTokLiveRegistry::addLive();
    TikTokAuthManager::instance().noteLiveStartSucceeded(client->authEpoch());
    blog(LOG_INFO, TAG "[TikTokStream] Endpoint created");
    ShowMessage(QString::fromUtf8(obs_module_text("TikTok.EndpointReady")));
    NotifyStateChanged();

    if (endAfterCreate_) {
        endAfterCreate_ = false;
        NotifyStartCancelled();
        EndEndpoint();
        return;
    }
    if (callbacks_.endpointReady)
        callbacks_.endpointReady();
}

void TikTokPlatformSession::EndEndpoint()
{
    if (tiktokSession_.state == State::EndingEndpoint)
        return;
    if (tiktokSession_.state == State::CreatingEndpoint) {
        endAfterCreate_ = true;
        NotifyStateChanged();
        return;
    }
    if (!tiktokSession_.HasEndpoint()) {
        const bool cancelledStart = tiktokSession_.state == State::Authenticating;
        if (endpointCreationUncertain_) {
            const auto answer = QMessageBox::question(uiParent_,
                QString::fromUtf8(obs_module_text("TikTok.EndLive")),
                QString::fromUtf8(obs_module_text("TikTok.ResolveUncertain")));
            if (answer != QMessageBox::Yes)
                return;
            endpointCreationUncertain_ = false;
        }
        tiktokSession_.Reset();
        ShowMessage(QString::fromUtf8(obs_module_text("TikTok.EndpointEnded")));
        if (cancelledStart)
            NotifyStartCancelled();
        else
            NotifyStateChanged();
        if (callbacks_.endpointEnded)
            callbacks_.endpointEnded();
        return;
    }

    tiktokSession_.state = State::EndingEndpoint;
    ShowMessage(QString::fromUtf8(obs_module_text("TikTok.EndingEndpoint")));
    NotifyStateChanged();
    blog(LOG_INFO, TAG "[TikTokStream] Ending remote endpoint");
    const auto gen = ++tiktokRequestGen_;
    auto client = tiktokSession_.client;
    auto id = tiktokSession_.streamId;
    struct EndOutcome {
        bool success = false;
        TikTokFailure failure = TikTokFailure::None;
        bool retained = false;
        ~EndOutcome() {
            if (!success && !retained)
                TikTokLiveRegistry::removeLive();
        }
    };
    auto outcome = std::make_shared<EndOutcome>();
    auto job = TikTokWorkerService::instance().submit(this,
        [client, id, outcome](const std::shared_ptr<TikTokJobContext> &) {
            outcome->success = client->EndStream(id, nullptr, &outcome->failure);
            if (outcome->failure == TikTokFailure::NotFound)
                outcome->success = true;
            if (outcome->success)
                TikTokLiveRegistry::removeLive();
            else
                blog(LOG_WARNING, TAG "[TikTokStream] Remote end failed; retry available while owner exists");
        },
        [gen, client, outcome](const std::shared_ptr<TikTokJobContext> &job) {
            auto *self = dynamic_cast<TikTokPlatformSession *>(job->receiver.data());
            if (!self || gen != self->tiktokRequestGen_)
                return;
            if (outcome->success) {
                self->tiktokSession_.Reset();
                self->ShowMessage(QString::fromUtf8(obs_module_text("TikTok.EndpointEnded")));
                blog(LOG_INFO, TAG "[TikTokStream] Remote endpoint ended");
                if (self->callbacks_.endpointEnded)
                    self->callbacks_.endpointEnded();
            } else {
                outcome->retained = true;
                self->tiktokSession_.state = outcome->failure == TikTokFailure::Auth ?
                    State::Error : State::EndpointReady;
                if (outcome->failure == TikTokFailure::Auth)
                    TikTokAuthManager::instance().handleAuthFailure(client->authEpoch());
                self->ShowMessage(QString::fromUtf8(obs_module_text("Error.TikTokEndLive")));
            }
            self->NotifyStateChanged();
        }, true, true);
    if (!job) {
        outcome->retained = true;
        tiktokSession_.state = State::EndpointReady;
        ShowMessage(QString::fromUtf8(obs_module_text("Error.TikTokEndLive")));
        NotifyStateChanged();
    }
}

void TikTokPlatformSession::CancelPendingWork()
{
    ++tiktokRequestGen_;
    endAfterCreate_ = false;
    endpointCreationUncertain_ = false;
    TikTokWorkerService::instance().cancelReceiver(this);
}

void TikTokPlatformSession::Shutdown()
{
    CancelPendingWork();
    if (tiktokSession_.HasEndpoint() &&
        tiktokSession_.state != State::EndingEndpoint)
        SubmitCleanup(tiktokSession_.client, tiktokSession_.streamId, true);
    tiktokSession_.Reset();
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
}

void TikTokPlatformSession::UpdateConfig(OutputTargetConfigPtr config)
{
    config_ = std::move(config);
}
