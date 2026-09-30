#include "hydra/internal/strict_json.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void testNodeBudgetRejectsWideInput() {
    std::string json = "[";
    for (int index = 0; index < 64; ++index) {
        if (index != 0) json += ',';
        json += "null";
    }
    json += ']';

    bool rejected = false;
    try {
        (void)hydra::internal::json::parse(
            json, hydra::internal::json::ParseOptions{8u, 32u});
    } catch (const hydra::internal::json::ParseError& error) {
        rejected = std::string_view(error.what()).find("node count") != std::string_view::npos;
    }
    check(rejected, "wide shallow JSON is rejected by the total node budget");
}

void testNodeBudgetAllowsBoundedDocument() {
    const auto value = hydra::internal::json::parse(
        R"json({"a":[1,true,null],"b":{"c":"ok"}})json",
        hydra::internal::json::ParseOptions{8u, 16u});
    const auto* object = std::get_if<hydra::internal::json::Value::Object>(&value.value);
    check(object != nullptr && object->size() == 2u,
          "bounded normal JSON still parses under the node budget");
}

void testDepthAndNodeBudgetsAreIndependent() {
    bool depthRejected = false;
    try {
        (void)hydra::internal::json::parse(
            "[[[0]]]", hydra::internal::json::ParseOptions{1u, 64u});
    } catch (const hydra::internal::json::ParseError& error) {
        depthRejected = std::string_view(error.what()).find("nesting too deep") !=
                        std::string_view::npos;
    }
    check(depthRejected, "depth limit remains independent from node budget");
}

} // namespace

int main() {
    testNodeBudgetRejectsWideInput();
    testNodeBudgetAllowsBoundedDocument();
    testDepthAndNodeBudgetsAreIndependent();
    std::cout << "Strict JSON parser tests passed.\n";
    return EXIT_SUCCESS;
}
