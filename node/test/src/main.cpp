/*
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "llm_server.h"

#include "glog/logging.h"

#include <atomic>
#include <csignal>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static std::atomic<bool> main_exit_flag{false};

static void __sigint(int)
{
    main_exit_flag.store(true, std::memory_order_relaxed);
}

int main(int argc, char *argv[])
{
    google::InitGoogleLogging(argv[0]);
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = __sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    mkdir("/tmp/llm", 0777);
    llm_llm llm;
    while (!main_exit_flag.load(std::memory_order_relaxed))
    {
        sleep(1);
    }
    return 0;
}
