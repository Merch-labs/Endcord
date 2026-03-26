#include "endcord_integrated_bot.h"

#include <dpp/dpp.h>

#include <memory>
#include <utility>

namespace endcord {

struct IntegratedBot::Impl {
    explicit Impl(std::unique_ptr<dpp::cluster> cluster) : cluster(std::move(cluster)) {}

    std::unique_ptr<dpp::cluster> cluster;
    bool running = false;
};

IntegratedBot::IntegratedBot(LogCallback info_logger, LogCallback warning_logger)
    : info_logger_(std::move(info_logger)), warning_logger_(std::move(warning_logger))
{
}

IntegratedBot::~IntegratedBot()
{
    stop();
}

bool IntegratedBot::start(const BotConfig &config)
{
    if (isRunning()) {
        return true;
    }

    if (config.discord.token.empty() || config.discord.token == "replace-me") {
        warning_logger_("Skipping integrated Discord bot startup because discord.token is not configured.");
        return false;
    }

    auto cluster = std::make_unique<dpp::cluster>(config.discord.token, dpp::i_default_intents | dpp::i_message_content);
    cluster->on_log([this](const dpp::log_t &event) {
        if (event.severity <= dpp::ll_warning) {
            warning_logger_("Discord runtime: " + event.message);
        }
    });
    cluster->on_ready([this](const dpp::ready_t &) { info_logger_("Integrated Discord bot connected to Discord."); });
    cluster->start(dpp::st_return);

    impl_ = std::make_unique<Impl>(std::move(cluster));
    impl_->running = true;
    info_logger_("Started integrated Discord bot runtime.");
    return true;
}

void IntegratedBot::stop()
{
    if (!impl_) {
        return;
    }

    if (impl_->cluster) {
        impl_->cluster->shutdown();
    }
    impl_.reset();
    info_logger_("Stopped integrated Discord bot runtime.");
}

bool IntegratedBot::isRunning() const
{
    return impl_ != nullptr && impl_->running;
}

}  // namespace endcord
