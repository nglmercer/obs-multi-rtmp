#pragma once

#include <functional>

#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>

#include "platform/platform-provider.h"
#include "streamlabs-tiktok.h"

class TikTokTargetEditor final : public PlatformTargetEditor {
public:
    explicit TikTokTargetEditor(OutputTargetConfigPtr target, QWidget *parent = nullptr);
    ~TikTokTargetEditor() override;

    void Load(const OutputTargetConfig &target) override;
    void Save(OutputTargetConfig &target) override;
    void SetEnabledChangedCallback(std::function<void()> callback) override;

private:
    void UpdateAuth();
    void RunCategorySearch();
    void ApplyCategories(uint64_t generation, const TikTokCategoryListResult &result,
                         uint64_t epoch);

    std::function<void()> enabledChanged_;

    QCheckBox *enable_ = nullptr;
    QLabel *authStatus_ = nullptr;
    QPushButton *loginButton_ = nullptr;
    QPushButton *logoutButton_ = nullptr;
    QPushButton *applyButton_ = nullptr;
    QLineEdit *title_ = nullptr;
    QComboBox *category_ = nullptr;
    QComboBox *audience_ = nullptr;
    QTimer *categoryDebounce_ = nullptr;
    uint64_t categoryGeneration_ = 0;
};
