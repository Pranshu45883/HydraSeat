#include "hydra/compatibility_result.hpp"
#include "hydra/setup_package.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

constexpr std::size_t kMaximumContributionBytes = 4u * 1024u * 1024u;

bool readBoundedFile(const char* path, std::string& output, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open contribution file";
        return false;
    }
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0 || static_cast<unsigned long long>(length) > kMaximumContributionBytes) {
        error = "contribution file exceeds the bounded maximum";
        return false;
    }
    input.seekg(0, std::ios::beg);
    try {
        output.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    } catch (...) {
        error = "contribution file allocation/read failed";
        return false;
    }
    if (!input.good() && !input.eof()) {
        error = "contribution file read failed";
        return false;
    }
    return true;
}

void usage() {
    std::cerr
        << "Usage:\n"
        << "  hydraseat_community_validate result <compatibility-result.json>\n"
        << "  hydraseat_community_validate setup-package <portable-setup.package>\n"
        << "\n"
        << "Successful validation prints the canonical privacy-safe representation.\n";
}

int fail(std::string_view code, std::string_view message) {
    std::cerr << code << ": " << message << '\n';
    return 2;
}

int validateResult(std::string_view bytes) {
    hydra::compat::CompatibilityResult decoded;
    const auto parsed = hydra::compat::decodeCompatibilityResultJson(bytes, decoded);
    if (!parsed.succeeded()) {
        return fail(hydra::compat::compatibilityResultCodeName(parsed.code), parsed.message);
    }
    std::string canonical;
    const auto encoded = hydra::compat::encodeCompatibilityResultJson(decoded, canonical);
    if (!encoded.succeeded()) {
        return fail(hydra::compat::compatibilityResultCodeName(encoded.code), encoded.message);
    }
    std::cout << canonical << '\n';
    return 0;
}

int validateSetup(std::string_view bytes) {
    hydra::portable::SetupPackage decoded;
    const auto parsed = hydra::portable::decodePackage(bytes, decoded);
    if (!parsed.succeeded()) {
        return fail(hydra::portable::packageResultName(parsed.result), parsed.message);
    }
    std::string canonical;
    const auto encoded = hydra::portable::encodePackage(decoded, canonical);
    if (!encoded.succeeded()) {
        return fail(hydra::portable::packageResultName(encoded.result), encoded.message);
    }
    std::cout << canonical;
    if (canonical.empty() || canonical.back() != '\n') std::cout << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        usage();
        return 2;
    }
    std::string bytes;
    std::string error;
    if (!readBoundedFile(argv[2], bytes, error)) return fail("Input", error);

    const std::string_view kind = argv[1];
    if (kind == "result") return validateResult(bytes);
    if (kind == "setup-package") return validateSetup(bytes);
    usage();
    return 2;
}
