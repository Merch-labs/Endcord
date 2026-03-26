#pragma once

#include "endcord_bot_config.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>

namespace endcord {

struct IntegratedBotCallbacks {
    std::function<nlohmann::json()> get_status;
    std::function<nlohmann::json(const std::string &, const std::string &, const std::string &, const std::string &,
                                 const std::string &, const std::string &, const std::string &, const std::string &,
                                 const std::string &)>
        relay_chat;
    std::function<nlohmann::json(const std::string &, const std::string &)> execute_command;
    std::function<nlohmann::json()> drain_system_messages;
    std::function<nlohmann::json(const std::string &)> configure_webhook;
};

class IntegratedBot {
public:
    using LogCallback = std::function<void(const std::string &)>;

    IntegratedBot(LogCallback info_logger, LogCallback warning_logger);
    ~IntegratedBot();

    IntegratedBot(const IntegratedBot &) = delete;
    IntegratedBot &operator=(const IntegratedBot &) = delete;

    bool start(const BotConfig &config, IntegratedBotCallbacks callbacks);
    void stop();
    bool isRunning() const;

private:
    struct Impl;

    LogCallback info_logger_;
    LogCallback warning_logger_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace endcord
