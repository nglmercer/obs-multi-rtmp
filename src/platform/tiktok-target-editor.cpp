#include "pch.h"

#include "platform/tiktok-target-editor.h"

#include <QGridLayout>
#include <QMessageBox>
#include <QSignalBlocker>

#include "tiktok/auth-manager.h"
#include "tiktok/streamlabs-client.h"
#include "tiktok/worker.h"

TikTokTargetEditor::TikTokTargetEditor(OutputTargetConfigPtr, QWidget *parent)
    : PlatformTargetEditor(parent)
{
    auto *layout = new QGridLayout(this);
    layout->setColumnStretch(1, 1);
    layout->addWidget(enable_ = new QCheckBox(obs_module_text("TikTok.Enable"), this), 0, 0, 1, 3);
    layout->addWidget(new QLabel(obs_module_text("TikTok.Status"), this), 1, 0);
    layout->addWidget(authStatus_ = new QLabel(this), 1, 1);
    authStatus_->setWordWrap(true);
    layout->addWidget(applyButton_ = new QPushButton(obs_module_text("TikTok.ApplyAccess"), this), 1, 2);
    QObject::connect(applyButton_, &QPushButton::clicked, [this]() {
        TikTokAuthManager::instance().openLiveAccessApplication(this);
    });
    layout->addWidget(loginButton_ = new QPushButton(obs_module_text("TikTok.Login"), this), 2, 0);
    layout->addWidget(logoutButton_ = new QPushButton(obs_module_text("TikTok.Logout"), this), 2, 1, 1, 2);
    QObject::connect(loginButton_, &QPushButton::clicked, [this]() {
        TikTokAuthManager::instance().login(this);
    });
    QObject::connect(logoutButton_, &QPushButton::clicked, [this]() {
        TikTokAuthManager::instance().logout(this);
    });
    layout->addWidget(new QLabel(obs_module_text("TikTok.Title"), this), 3, 0);
    layout->addWidget(title_ = new QLineEdit("", this), 3, 1, 1, 2);
    layout->addWidget(new QLabel(obs_module_text("TikTok.Category"), this), 4, 0);
    layout->addWidget(category_ = new QComboBox(this), 4, 1, 1, 2);
    category_->setEditable(true);
    category_->setInsertPolicy(QComboBox::NoInsert);
    categoryDebounce_ = new QTimer(this);
    categoryDebounce_->setSingleShot(true);
    QObject::connect(category_->lineEdit(), &QLineEdit::textEdited, [this]() {
        categoryDebounce_->start(400);
    });
    QObject::connect(categoryDebounce_, &QTimer::timeout, [this]() {
        RunCategorySearch();
    });
    layout->addWidget(new QLabel(obs_module_text("TikTok.Audience"), this), 5, 0);
    layout->addWidget(audience_ = new QComboBox(this), 5, 1, 1, 2);
    audience_->addItem(obs_module_text("TikTok.AudienceEveryone"), "0");
    audience_->addItem(obs_module_text("TikTok.AudienceAdult"), "1");

    QObject::connect(enable_, &QCheckBox::toggled, this, [this](bool) {
        if (enabledChanged_)
            enabledChanged_();
    });
    QObject::connect(&TikTokAuthManager::instance(), &TikTokAuthManager::authenticationChanged,
                     this, [this]() { UpdateAuth(); });
    QObject::connect(&TikTokAuthManager::instance(), &TikTokAuthManager::loginFailed,
                     this, [this](const QString &message) {
                         if (message.isEmpty() ||
                             message == QString::fromUtf8(obs_module_text("TikTok.LoginCancelled")))
                             return;
                         QMessageBox::warning(this,
                             QString::fromUtf8(obs_module_text("TikTok.Settings")), message);
                     });
    UpdateAuth();
    TikTokAuthManager::instance().refreshAccessState();
    layout->setRowStretch(6, 1);
    setLayout(layout);
}

TikTokTargetEditor::~TikTokTargetEditor()
{
    TikTokWorkerService::instance().cancelReceiver(this);
}

void TikTokTargetEditor::SetEnabledChangedCallback(std::function<void()> callback)
{
    enabledChanged_ = std::move(callback);
}

void TikTokTargetEditor::Load(const OutputTargetConfig &target)
{
    const QSignalBlocker enableBlocker(enable_);
    enable_->setChecked(target.tiktok.enabled);
    title_->setText(QString::fromUtf8(target.tiktok.title));
    category_->blockSignals(true);
    category_->clear();
    if (!target.tiktok.category.empty())
        category_->addItem(QString::fromUtf8(target.tiktok.category));
    category_->setEditText(QString::fromUtf8(target.tiktok.category));
    category_->blockSignals(false);
    const int audienceIndex =
        audience_->findData(target.tiktok.matureContent() ? "1" : "0");
    audience_->setCurrentIndex(audienceIndex < 0 ? 0 : audienceIndex);
}

void TikTokTargetEditor::Save(OutputTargetConfig &target)
{
    target.tiktok.enabled = enable_->isChecked();
    target.tiktok.title = tostdu8(title_->text());
    target.tiktok.category = tostdu8(category_->currentText());
    target.tiktok.audience = (audience_->currentData().toString() == QStringLiteral("1"))
        ? TikTokAudience::AdultOnly
        : TikTokAudience::Everyone;
}

void TikTokTargetEditor::UpdateAuth()
{
    auto &auth = TikTokAuthManager::instance();
    authStatus_->setText(auth.statusText());
    const bool authenticated = auth.isAuthenticated();
    const auto status = auth.status();
    const bool busy = status == TikTokAuthManager::Status::LoggingIn ||
                      status == TikTokAuthManager::Status::Validating;
    loginButton_->setVisible(!authenticated);
    logoutButton_->setVisible(authenticated);
    loginButton_->setEnabled(!busy);
    logoutButton_->setEnabled(!busy);
    applyButton_->setVisible(authenticated && !auth.isLiveAccessGranted());
    applyButton_->setEnabled(!busy);
}

void TikTokTargetEditor::RunCategorySearch()
{
    auto client = TikTokAuthManager::instance().client();
    if (!client || !category_)
        return;
    const std::string query = tostdu8(category_->currentText());
    if (query.size() < 2)
        return;
    const uint64_t generation = ++categoryGeneration_;
    auto result = std::make_shared<TikTokCategoryListResult>();
    TikTokWorkerService::instance().submit(
        this,
        [client, query, result](const std::shared_ptr<TikTokJobContext> &) {
            *result = client->SearchCategories(query);
        },
        [result, generation, epoch = client->authEpoch()](const std::shared_ptr<TikTokJobContext> &job) {
            auto *self = dynamic_cast<TikTokTargetEditor *>(job->receiver.data());
            if (self == nullptr)
                return;
            self->ApplyCategories(generation, *result, epoch);
        },
        false);
}

void TikTokTargetEditor::ApplyCategories(uint64_t generation,
                                         const TikTokCategoryListResult &result,
                                         uint64_t epoch)
{
    if (generation != categoryGeneration_ || !category_)
        return;
    if (!result.success) {
        switch (ClassifyCredentialResult(result)) {
        case CredentialValidation::InvalidCredential:
            TikTokAuthManager::instance().handleAuthFailure(epoch);
            break;
        case CredentialValidation::ValidLiveForbidden:
            TikTokAuthManager::instance().noteLiveAccessDenied(epoch);
            break;
        default:
            break;
        }
        return;
    }
    const QString current = category_->currentText();
    category_->blockSignals(true);
    category_->clear();
    for (const auto &entry : result.categories) {
        category_->addItem(QString::fromUtf8(entry.fullName),
                           QString::fromUtf8(entry.gameMaskId));
    }
    category_->setEditText(current);
    category_->blockSignals(false);
    if (!result.categories.empty() && category_->hasFocus())
        category_->showPopup();
}
