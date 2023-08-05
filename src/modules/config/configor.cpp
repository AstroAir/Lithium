/*
 * configor.cpp
 *
 * Copyright (C) 2023 Max Qian <lightapt.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*************************************************

Copyright: 2023 Max Qian. All rights reserved

Author: Max Qian

E-mail: astro_air@126.com

Date: 2023-4-30

Description: Configor

**************************************************/

#include <iostream>
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <sstream>
#include <regex>

#include "configor.hpp"

#include "loguru/loguru.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace Lithium::Config
{
    ConfigManager::ConfigManager()
    {
        m_AchievementManager = std::make_unique<AAchievement::AchievementList>();
        loadFromFile("config.json");
        LOG_F(INFO, "%s", config_.dump(4));
    }

    ConfigManager::~ConfigManager()
    {
        saveToFile("config.json");
    }

    std::shared_ptr<ConfigManager> ConfigManager::createShared()
    {
        return std::make_shared<ConfigManager>();
    }

    void ConfigManager::loadFromFile(const std::string &path)
    {
        std::ifstream ifs(path);
        if (!ifs.is_open())
        {
            LOG_F(ERROR, "Failed to open file: %s", path.c_str());
            return;
        }
        json j;
        try
        {
            ifs >> j;
            LOG_F(INFO, "%s", j.dump(4).c_str());
            const std::string basename = path.substr(path.find_last_of("/\\") + 1);
            const std::string name_without_ext = basename.substr(0, basename.find_last_of('.'));
            config_[name_without_ext] = j["config"];
            LOG_F(INFO, "Loaded config file %s successfully", path.c_str());
        }
        catch (const json::exception &e)
        {
            LOG_F(ERROR, "Failed to parse file: %s, error message: %s", path.c_str(), e.what());
        }
    }

    void ConfigManager::loadFromDir(const std::string &dir_path, bool recursive)
    {
        for (const auto &file : fs::directory_iterator(dir_path))
        {
            if (file.path().extension() == ".json")
            {
                loadFromFile(file.path().string());
            }
            else if (recursive && file.is_directory())
            {
                const std::string subdir_path = file.path().string();
                const std::string basename = file.path().filename().string();
                const std::string config_file_path = subdir_path + "/config.json";
                if (fs::exists(config_file_path))
                {
                    json j;
                    try
                    {
                        std::ifstream ifs(config_file_path);
                        ifs >> j;
                        auto merged_j = json::object_t{};
                        merged_j[dir_path][basename] = j;
                        mergeConfig(merged_j);
                    }
                    catch (const json::exception &e)
                    {
                        LOG_F(ERROR, "Failed to parse file: %s, error message: %s", config_file_path.c_str(), e.what());
                    }
                }
                loadFromDir(subdir_path, true);
            }
        }
    }

    void ConfigManager::setValue(const std::string &key_path, const json &value)
    {
        std::vector<std::string> keys = split(key_path, "/");
        json *p = &config_;
        for (const auto &key : keys)
        {
            if (!p->is_object())
            {
                LOG_F(ERROR, "Invalid key path: %s", key_path.c_str());
                return;
            }
            p = &(*p)[key];
        }
        *p = value;
    }

    json ConfigManager::getValue(const std::string &key_path) const
    {
        std::vector<std::string> keys = split(key_path, "/");
        const json *p = &config_;
        for (const auto &key : keys)
        {
            if (p->is_object() && p->contains(key))
            {
                p = &(*p)[key];
            }
            else
            {
                LOG_F(ERROR, "Key not found: %s", key_path.c_str());
                return nullptr;
            }
        }
        return *p;
    }

    void ConfigManager::deleteValue(const std::string &key_path)
    {
        std::vector<std::string> keys = split(key_path, "/");
        json *p = &config_;
        for (const auto &key : keys)
        {
            if (!p->is_object())
            {
                LOG_F(ERROR, "Invalid key path: %s", key_path.c_str());
                return;
            }
            p = &(*p)[key];
        }
        if (p->is_null())
        {
            LOG_F(ERROR, "Key not found: %s", key_path.c_str());
            return;
        }
        p->clear();
    }

    void ConfigManager::printValue(const std::string &key, const json &value) const
    {
        if (value.is_object())
        {
            for (auto &[sub_key, sub_value] : value.items())
            {
                std::stringstream ss;
                ss << key << "/" << sub_key;
                printValue(ss.str(), sub_value);
            }
        }
        else
        {
            LOG_F(INFO, "%s: %s", key.c_str(), value.dump().c_str());
        }
    }

    std::vector<std::string> ConfigManager::split(const std::string &s, const std::string &delimiter) const
    {
        std::vector<std::string> tokens;
        std::regex regex(delimiter);
        std::copy(std::sregex_token_iterator(s.begin(), s.end(), regex, -1),
                  std::sregex_token_iterator(),
                  std::back_inserter(tokens));
        return tokens;
    }

}
