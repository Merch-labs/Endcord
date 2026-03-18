#include "bridge_support.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {

void expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main()
{
    using bridge_support::AvatarProviderOptions;
    using bridge_support::PlayerIdentity;

    expect(bridge_support::normalizeAvatarMode("self_hosted") == "rendered", "avatar mode normalization");
    expect(bridge_support::normalizeAvatarMode("none") == "disabled", "avatar disabled normalization");
    expect(bridge_support::normalizeAvatarProvider("mc-heads") == "mcheads", "provider normalization");

    expect(bridge_support::isAllowedRemoteAddress("172.19.0.5", {"172.19.0.0/16"}), "cidr allowlist");
    expect(!bridge_support::isAllowedRemoteAddress("10.0.0.5", {"172.19.0.0/16"}), "cidr denylist");

    const PlayerIdentity identity{
        .username = "Player One",
        .xuid = "2535441133062698",
        .uuid = "12345678-1234-5678-1234-567812345678",
        .skin_id = "skin-123",
    };

    const auto tab_url = bridge_support::buildProviderAvatarUrl(
        AvatarProviderOptions{.provider = "tabavatars", .provider_prefer_xuid = true, .provider_render_type = "helm", .size = 64},
        identity);
    expect(tab_url.has_value(), "tabavatars url generated");
    expect(tab_url->find("xuid=2535441133062698") != std::string::npos, "tabavatars xuid path");

    const auto mcheads_url = bridge_support::buildProviderAvatarUrl(
        AvatarProviderOptions{.provider = "mcheads", .provider_prefer_xuid = false, .provider_bedrock_username_prefix = ".", .size = 32},
        identity);
    expect(mcheads_url.has_value(), "mcheads url generated");
    expect(mcheads_url->find("/head/.Player%20One/32") != std::string::npos, "mcheads username path");

    const auto custom_url = bridge_support::buildProviderAvatarUrl(
        AvatarProviderOptions{.provider = "custom",
                              .provider_url_template = "https://avatars.example/{xuid}/{uuid_nodashes}/{size}"},
        identity);
    expect(custom_url == std::optional<std::string>(
                             "https://avatars.example/2535441133062698/12345678123456781234567812345678/64"),
           "custom provider template");

    std::cout << "bridge_support tests passed\n";
    return 0;
}
