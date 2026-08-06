#include "metrics_sampler.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void ExpectNear(double actual, double expected, const char* message) {
    if (std::fabs(actual - expected) > 0.001) {
        std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    media_worker::MetricsSampler sampler;

    auto sample = sampler.Sample(50, 500000, 2.0);
    ExpectNear(sample.fps, 25.0, "first sample fps");
    ExpectNear(sample.bitrate_kbps, 2000.0, "first sample bitrate");

    sample = sampler.Sample(80, 800000, 3.0);
    ExpectNear(sample.fps, 30.0, "interval fps");
    ExpectNear(sample.bitrate_kbps, 2400.0, "interval bitrate");

    sample = sampler.Sample(100, 900000, 3.0);
    ExpectNear(sample.fps, 0.0, "zero interval fps");
    ExpectNear(sample.bitrate_kbps, 0.0, "zero interval bitrate");

    sample = sampler.Sample(130, 1200000, 4.0);
    ExpectNear(sample.fps, 50.0, "sample after zero interval fps");
    ExpectNear(sample.bitrate_kbps, 3200.0, "sample after zero interval bitrate");

    media_worker::MetricsSampler reset_sampler;
    reset_sampler.Sample(100, 1000000, 2.0);
    sample = reset_sampler.Sample(10, 100000, 3.0);
    ExpectNear(sample.fps, 10.0, "counter reset fps");
    ExpectNear(sample.bitrate_kbps, 800.0, "counter reset bitrate");

    std::cout << "all metrics sampler tests passed\n";
    return 0;
}
