#pragma once

#include "endcord_bot_config.h"

#include <functional>
#include <memory>
#include <string>

namespace endcord {

class IntegratedBot {
public:
    using LogCallback = std::function<void(const std::string &)>;

    IntegratedBot(LogCallback info_logger, LogCallback warning_logger);
    ~IntegratedBot();

    IntegratedBot(const IntegratedBot &) = delete;
    IntegratedBot &operator=(const IntegratedBot &) = delete;

    bool start(const BotConfig &config);
    void stop();
    bool isRunning() const;

private:
    struct Impl;

    LogCallback info_logger_;
    LogCallback warning_logger_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace endcord
