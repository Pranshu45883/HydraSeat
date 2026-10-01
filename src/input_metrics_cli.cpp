#include "hydra/input_metrics.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

hydra::InputMetricSample makeSample(std::uint64_t correlation,
                                    hydra::InputMetricStage stage,
                                    std::uint64_t timestamp,
                                    std::uint32_t seat,
                                    std::uint32_t process) {
    hydra::InputMetricSample sample;
    sample.correlationId = correlation;
    sample.stage = stage;
    sample.timestampMicros = timestamp;
    sample.expectedSeatId = seat;
    sample.receivingSeatId = seat;
    sample.targetProcessId = process;
    sample.receivingProcessId = process;
    sample.eventClass = hydra::InputMetricEventClass::Key;
    return sample;
}

hydra::InputMetricsSnapshot deterministicFixture() {
    hydra::InputMetricsSnapshot snapshot;
    for (std::uint64_t event = 0; event < 4u; ++event) {
        const std::uint64_t correlation = event + 1u;
        const std::uint64_t base = 1000u + event * 1000u;
        const std::uint64_t end = base + (event + 1u) * 50u;
        snapshot.samples.push_back(makeSample(
            correlation, hydra::InputMetricStage::PhysicalObserved,
            base, 1u, 101u));
        snapshot.samples.push_back(makeSample(
            correlation, hydra::InputMetricStage::RouteEnqueued,
            base + 10u, 1u, 101u));
        snapshot.samples.push_back(makeSample(
            correlation, hydra::InputMetricStage::RouteDequeued,
            base + 20u, 1u, 101u));
        snapshot.samples.push_back(makeSample(
            correlation, hydra::InputMetricStage::RouteWritten,
            base + 30u, 1u, 101u));
        snapshot.samples.push_back(makeSample(
            correlation, hydra::InputMetricStage::TargetApplied,
            base + 40u, 1u, 101u));
        snapshot.samples.push_back(makeSample(
            correlation, hydra::InputMetricStage::TargetQueried,
            end, 1u, 101u));
    }
    return snapshot;
}

int runFixture(bool printReport) {
    const auto snapshot = deterministicFixture();
    hydra::InputMetricsReport report;
    const auto result = hydra::buildInputMetricsReport(snapshot, report);
    if (result != hydra::InputMetricsReportResult::Success ||
        report.uniqueInputEvents != 4u ||
        report.completeInputEvents != 4u ||
        report.endToEnd.count != 4u ||
        report.endToEnd.p50Micros != 100u ||
        report.endToEnd.p95Micros != 200u ||
        report.endToEnd.p99Micros != 200u ||
        report.receiverVerifiedEvents != 4u ||
        report.missingReceiverEvidenceEvents != 0u ||
        report.crossSeatEvents != 0u ||
        report.crossProcessEvents != 0u) {
        std::cerr << "P3-MET-01 deterministic fixture failed: "
                  << hydra::inputMetricsReportResultName(result) << '\n';
        return 2;
    }
    if (printReport) {
        std::cout << hydra::encodeInputMetricsReportJson(report) << '\n';
    } else {
        std::cout
            << "HydraSeat P3-MET-01 input metrics self-test passed: "
               "4 complete receiver-verified events, p50=100us, "
               "p95/p99=200us, zero verified bleed.\n";
    }
    return EXIT_SUCCESS;
}

void printUsage() {
    std::cout
        << "HydraSeat input metrics harness\n\n"
        << "Usage:\n"
        << "  hydra_input_metrics --self-test\n"
        << "  hydra_input_metrics --fixture-report\n\n"
        << "The live recorder is consumed by HydraSeat host/acceptance code. "
           "Report generation occurs off the latency-sensitive input path.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
        return runFixture(false);
    }
    if (argc == 2 && std::string_view(argv[1]) == "--fixture-report") {
        return runFixture(true);
    }
    printUsage();
    return argc == 1 ? EXIT_SUCCESS : 1;
}
