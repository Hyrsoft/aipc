/*****************************************************************************
 * | Author      :   Luckfox team
 * | Function    :
 * | Info        :
 *
 *----------------
 * | This version:   V1.1
 * | Date        :   2024-08-26
 * | Info        :   Basic version
 *
 ******************************************************************************/

#include "luckfox_mpi.h"

RK_U64 TEST_COMM_GetNowUs() {
    struct timespec time = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (RK_U64) time.tv_sec * 1000000 + (RK_U64) time.tv_nsec / 1000; /* microseconds */
}
