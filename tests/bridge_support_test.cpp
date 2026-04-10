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

    // --- truncateUtf8Bytes ---

    // ASCII: shorter than limit → unchanged
    expect(bridge_support::truncateUtf8Bytes("hello", 10) == "hello", "truncate: ascii passthrough");
    // ASCII: exact boundary
    expect(bridge_support::truncateUtf8Bytes("hello", 5) == "hello", "truncate: ascii exact boundary");
    // ASCII: truncation
    expect(bridge_support::truncateUtf8Bytes("hello", 3) == "hel", "truncate: ascii truncation");
    // max_bytes == 0
    expect(bridge_support::truncateUtf8Bytes("hello", 0) == "", "truncate: zero limit");
    // 2-byte sequence (U+00E9 é = 0xC3 0xA9): limit falls in middle of sequence
    expect(bridge_support::truncateUtf8Bytes("\xC3\xA9z", 1) == "", "truncate: 2-byte seq not split");
    // 3-byte sequence (U+4E2D 中 = 0xE4 0xB8 0xAD): limit falls after full sequence
    expect(bridge_support::truncateUtf8Bytes("\xE4\xB8\xADx", 3) == "\xE4\xB8\xAD", "truncate: 3-byte seq full");
    // 3-byte sequence: limit falls in middle
    expect(bridge_support::truncateUtf8Bytes("\xE4\xB8\xADx", 2) == "", "truncate: 3-byte seq not split");
    // 4-byte sequence (U+1F600 😀 = 0xF0 0x9F 0x98 0x80): limit falls in middle
    expect(bridge_support::truncateUtf8Bytes("\xF0\x9F\x98\x80z", 2) == "", "truncate: 4-byte seq not split");
    // 4-byte sequence: limit falls after full sequence
    expect(bridge_support::truncateUtf8Bytes("\xF0\x9F\x98\x80z", 5) == "\xF0\x9F\x98\x80z", "truncate: 4-byte seq full");
    // Mixed: ASCII before multibyte, cut just before multibyte
    expect(bridge_support::truncateUtf8Bytes("ab\xE4\xB8\xAD", 2) == "ab", "truncate: cut before multibyte");

    // --- replaceAll ---

    expect(bridge_support::replaceAll("hello world", "world", "there") == "hello there",
           "replaceAll: basic replacement");
    expect(bridge_support::replaceAll("aaa", "a", "bb") == "bbbbbb", "replaceAll: repeated replacement");
    expect(bridge_support::replaceAll("hello", "x", "y") == "hello", "replaceAll: no match");
    expect(bridge_support::replaceAll("hello", "", "y") == "hello", "replaceAll: empty needle unchanged");
    expect(bridge_support::replaceAll("", "x", "y") == "", "replaceAll: empty input");
    expect(bridge_support::replaceAll("abcabc", "abc", "") == "", "replaceAll: replace with empty");

    // --- applyTemplate ---

    expect(bridge_support::applyTemplate("{player} joined", {{"{player}", "Alice"}}) == "Alice joined",
           "applyTemplate: single replacement");
    expect(bridge_support::applyTemplate("{a} and {b}", {{"{a}", "X"}, {"{b}", "Y"}}) == "X and Y",
           "applyTemplate: multiple replacements");
    expect(bridge_support::applyTemplate("{unknown} left", {{"{player}", "Alice"}}) == "{unknown} left",
           "applyTemplate: missing placeholder unchanged");
    expect(bridge_support::applyTemplate("", {{"{player}", "Alice"}}) == "", "applyTemplate: empty template");
    expect(bridge_support::applyTemplate("no placeholders", {}) == "no placeholders",
           "applyTemplate: no replacements");

    std::cout << "bridge_support tests passed\n";
    return 0;
}
