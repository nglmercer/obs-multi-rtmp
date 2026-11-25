#include "platform/platform-provider.h"

#include <algorithm>

namespace {

std::vector<PlatformProvider *> &MutablePlatformProviders()
{
    static std::vector<PlatformProvider *> providers;
    return providers;
}

} // namespace

const std::vector<PlatformProvider *> &PlatformProviders()
{
    return MutablePlatformProviders();
}

void RegisterPlatformProvider(PlatformProvider *provider)
{
    if (!provider)
        return;

    auto &providers = MutablePlatformProviders();
    if (std::find(providers.begin(), providers.end(), provider) == providers.end())
        providers.push_back(provider);
}

PlatformProvider *PlatformProviderForTarget(const OutputTargetConfig &target)
{
    for (auto *provider : PlatformProviders()) {
        if (provider && provider->IsTargetEnabled(target))
            return provider;
    }
    return nullptr;
}
