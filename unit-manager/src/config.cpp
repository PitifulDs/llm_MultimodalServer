/*
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <fstream>
#include <iostream>
#include <cstdlib>

#include "all.h"
#include "json.hpp"

void load_default_config()
{
    // defaults to avoid bad_any_cast when config is missing
    key_sql["config_tcp_server"] = 10001;
    key_sql["config_zmq_min_port"] = 5010;
    key_sql["config_zmq_max_port"] = 5555;
    key_sql["config_zmq_s_format"] = std::string("ipc:///tmp/llm/%i.sock");
    key_sql["config_zmq_c_format"] = std::string("ipc:///tmp/llm/%i.sock");

    const char *cfg_env = std::getenv("UNIT_MANAGER_CONFIG");
    std::string cfg_path = cfg_env && *cfg_env ? std::string(cfg_env)
                                                : std::string("unit-manager/master_config.json");

    std::ifstream file(cfg_path);
    if (!file.is_open()) {
        // legacy relative path
        file.open("../master_config.json");
    }
    if (!file.is_open()) {
        return;
    }
    nlohmann::json req_body;
    try {
        file >> req_body;
    } catch (...) {
        file.close();
        return;
    }
    file.close();

    for (auto it = req_body.begin(); it != req_body.end(); ++it) {
        if (req_body[it.key()].is_number()) {
            key_sql[(std::string)it.key()] = (int)it.value();
        }
        if (req_body[it.key()].is_string()) {
            key_sql[(std::string)it.key()] = (std::string)it.value();
        }
    }
}
