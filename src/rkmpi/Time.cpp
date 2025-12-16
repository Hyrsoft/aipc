#include "Time.hpp"

#include <time.h>

namespace aipc::rkmpi {

    RK_U64 now_us() {
        struct timespec time = {0, 0};
        clock_gettime(CLOCK_MONOTONIC, &time);
        return (RK_U64) time.tv_sec * 1000000 + (RK_U64) time.tv_nsec / 1000;
    }

} // namespace aipc::rkmpi
