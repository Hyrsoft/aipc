#pragma once

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "sample_comm.h"

#define TEST_ARGB32_PIX_SIZE 4
#define TEST_ARGB32_RED 0xFF0000FF
#define TEST_ARGB32_GREEN 0x00FF00FF
#define TEST_ARGB32_BLUE 0x0000FFFF
#define TEST_ARGB32_TRANS 0x00000000
#define TEST_ARGB32_BLACK 0x000000FF

int venc_init(int chnId, int width, int height, RK_CODEC_ID_E enType);
