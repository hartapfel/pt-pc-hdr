#pragma once

// The public half of the release signing key (tools/ci/release_signing.py keygen; docs/updates.md). The game and the
// setups accept an update manifest only with a signature this key verifies. The private half never enters a repository.

#include <string_view>

namespace pt::update {
inline constexpr std::string_view kReleasePublicKeyHex = "6a85e3ed9d6ced86b87b65ba8e71e20ecf1962088d1bead4310775f1b93ff661";
}
