#include "hydra/profile_cli.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

constexpr std::size_t kMaximumInputBytes = 4u * 1024u * 1024u;

bool readBoundedFile(const char* path, std::string& output, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open input file";
        return false;
    }
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0 || static_cast<unsigned long long>(length) > kMaximumInputBytes) {
        error = "input file exceeds the bounded maximum";
        return false;
    }
    input.seekg(0, std::ios::beg);
    try {
        output.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    } catch (...) {
        error = "input file could not be read";
        return false;
    }
    if (!input.good() && !input.eof()) {
        error = "input file read failed";
        return false;
    }
    return true;
}

void usage() {
    std::cerr
        << "Usage:\n"
        << "  hydraseat_profilectl list <games|players|setups|plan> <file> [--json]\n"
        << "  hydraseat_profilectl validate <games|players|setups|plan> <file>\n"
        << "  hydraseat_profilectl export <games|players|setups|plan> <file> [--json]\n"
        << "\n"
        << "Player provider account-reference values are always redacted.\n";
}

int fail(std::string_view category, std::string_view message) {
    std::cerr << category << ": " << message << '\n';
    return 2;
}

int handleGames(std::string_view bytes,
                hydra::cli::OutputFormat format,
                bool validateOnly) {
    hydra::profile::GameRecordDocument document;
    const auto schema = hydra::profile::decodeGameRecordDocument(bytes, document);
    if (!schema.succeeded()) return fail("GameRecord", schema.message);
    if (validateOnly) {
        std::cout << "OK GameRecordDocument v" << document.schemaVersion << '\n';
        return 0;
    }
    std::string output;
    const auto result = hydra::cli::renderGames(document, format, output);
    if (!result.succeeded()) return fail(hydra::cli::cliResultName(result.result), result.message);
    std::cout << output;
    if (output.empty() || output.back() != '\n') std::cout << '\n';
    return 0;
}

int handlePlayers(std::string_view bytes,
                  hydra::cli::OutputFormat format,
                  bool validateOnly) {
    hydra::profile::PlayerProfileDocument document;
    const auto schema = hydra::profile::decodePlayerProfileDocument(bytes, document);
    if (!schema.succeeded()) return fail("PlayerProfile", schema.message);
    if (validateOnly) {
        std::cout << "OK PlayerProfileDocument v" << document.schemaVersion << '\n';
        return 0;
    }
    std::string output;
    const auto result = hydra::cli::renderPlayers(document, format, output);
    if (!result.succeeded()) return fail(hydra::cli::cliResultName(result.result), result.message);
    std::cout << output;
    if (output.empty() || output.back() != '\n') std::cout << '\n';
    return 0;
}

int handleSetups(std::string_view bytes,
                 hydra::cli::OutputFormat format,
                 bool validateOnly) {
    hydra::profile::TwoPlayerSetupDocument document;
    const auto schema = hydra::profile::decodeTwoPlayerSetupDocument(bytes, document);
    if (!schema.succeeded()) return fail("TwoPlayerSetup", schema.message);
    if (validateOnly) {
        std::cout << "OK TwoPlayerSetupDocument v" << document.schemaVersion << '\n';
        return 0;
    }
    std::string output;
    const auto result = hydra::cli::renderSetups(document, format, output);
    if (!result.succeeded()) return fail(hydra::cli::cliResultName(result.result), result.message);
    std::cout << output;
    if (output.empty() || output.back() != '\n') std::cout << '\n';
    return 0;
}

int handlePlan(std::string_view bytes,
               hydra::cli::OutputFormat format,
               bool validateOnly) {
    hydra::cli::PlanSnapshot snapshot;
    const auto decoded = hydra::cli::decodePlanSnapshot(bytes, snapshot);
    if (!decoded.succeeded()) return fail(hydra::cli::cliResultName(decoded.result), decoded.message);
    if (validateOnly) {
        std::cout << "OK PlanSnapshot v" << snapshot.version
                  << " fingerprint=" << snapshot.fingerprint << '\n';
        return 0;
    }
    std::string output;
    const auto rendered = hydra::cli::renderPlan(snapshot, format, output);
    if (!rendered.succeeded()) {
        return fail(hydra::cli::cliResultName(rendered.result), rendered.message);
    }
    std::cout << output;
    if (output.empty() || output.back() != '\n') std::cout << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4 || argc > 5) {
        usage();
        return 2;
    }

    const std::string_view command = argv[1];
    const std::string_view kind = argv[2];
    const bool validateOnly = command == "validate";
    if (command != "list" && command != "validate" && command != "export") {
        usage();
        return 2;
    }
    hydra::cli::OutputFormat format = hydra::cli::OutputFormat::Human;
    if (command == "export") format = hydra::cli::OutputFormat::Json;
    if (argc == 5) {
        if (std::string_view(argv[4]) != "--json" || validateOnly) {
            usage();
            return 2;
        }
        format = hydra::cli::OutputFormat::Json;
    }

    std::string bytes;
    std::string error;
    if (!readBoundedFile(argv[3], bytes, error)) return fail("Input", error);

    if (kind == "games") return handleGames(bytes, format, validateOnly);
    if (kind == "players") return handlePlayers(bytes, format, validateOnly);
    if (kind == "setups") return handleSetups(bytes, format, validateOnly);
    if (kind == "plan") return handlePlan(bytes, format, validateOnly);

    usage();
    return 2;
}
