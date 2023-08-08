/*
 * LithiumApp.hpp
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

Date: 2023-7-13

Description: Lithium App Enter

**************************************************/

#pragma once

#define LITHIUM_APP_MAIN

#include <memory>

#include "modules/thread/thread.hpp"
#include "modules/config/configor.hpp"
#include "modules/device/device_manager.hpp"
#include "modules/system/process.hpp"
#include "modules/task/task_manager.hpp"
#include "modules/task/task_generator.hpp"
#include "modules/server/message_bus.hpp"
#include "modules/server/message_queue.hpp"
#include "modules/property/imessage.hpp"
#include "modules/plugin/plugin_manager.hpp"

namespace Lithium
{
    class LithiumApp
    {
    public:
        LithiumApp();
        ~LithiumApp();

    public:
        nlohmann::json GetConfig(const std::string &key_path) const;
        void SetConfig(const std::string &key_path, const nlohmann::json &value);

    public:
        std::vector<std::string> getDeviceList(DeviceType type);
        bool addDevice(DeviceType type, const std::string &name, const std::string &lib_name = "");
        bool addDeviceLibrary(const std::string &lib_path, const std::string &lib_name);
        void addDeviceObserver(DeviceType type, const std::string &name);
        bool removeDevice(DeviceType type, const std::string &name);
        bool removeDevicesByName(const std::string &name);
        bool removeDeviceLibrary(const std::string &lib_name);
        std::shared_ptr<Device> getDevice(DeviceType type, const std::string &name);
        size_t findDevice(DeviceType type, const std::string &name);
        std::shared_ptr<Device> findDeviceByName(const std::string &name) const;
        std::shared_ptr<SimpleTask> getTask(DeviceType type, const std::string &device_name, const std::string &task_name, const nlohmann::json &params);

    public:
        bool createProcess(const std::string &command, const std::string &identifier);
        bool runScript(const std::string &script, const std::string &identifier);
        bool terminateProcess(pid_t pid, int signal = SIGTERM);
        bool terminateProcessByName(const std::string &name, int signal = SIGTERM);
        std::vector<Process::Process> getRunningProcesses();
        std::vector<std::string> getProcessOutput(const std::string &identifier);

    public:
        bool addTask(const std::shared_ptr<BasicTask> &task);
        bool insertTask(const std::shared_ptr<BasicTask> &task, int position);
        bool executeAllTasks();
        bool stopTask();
        bool executeTaskByName(const std::string &name);
        bool modifyTask(int index, const std::shared_ptr<BasicTask> &task);
        bool modifyTaskByName(const std::string &name, const std::shared_ptr<BasicTask> &task);
        bool deleteTask(int index);
        bool deleteTaskByName(const std::string &name);
        bool queryTaskByName(const std::string &name);
        const std::vector<std::shared_ptr<BasicTask>> &getTaskList() const;
        bool saveTasksToJson() const;

    public:
        template <typename T>
        void MSSubscribe(const std::string &topic, std::function<void(const T &)> callback, int priority = 0)
        {
            m_MessageBus->Subscribe(topic, callback, priority);
        }

        template <typename T>
        void MSUnsubscribe(const std::string &topic, std::function<void(const T &)> callback)
        {
            m_MessageBus->Unsubscribe(topic, callback);
        }

    public:
        void addThread(std::function<void()> func, const std::string &name);
        void joinAllThreads();
        void joinThreadByName(const std::string &name);
        bool sleepThreadByName(const std::string &name, int seconds);
        bool isThreadRunning(const std::string &name);

    public:
        template <typename T>
        std::vector<std::shared_ptr<T>> loadControllers()
        {
            std::vector<std::shared_ptr<T>> res;
            for (auto lib_name : m_cModuleLoader->GetAllExistedModules())
            {
                res.push_back(m_cModuleLoader->GetInstance<T>(lib_name, {}, "GetController"));
            }
            return res;
        }

    private:
        std::shared_ptr<Thread::ThreadManager> m_ThreadManager;
        std::shared_ptr<Config::ConfigManager> m_ConfigManager;
        std::shared_ptr<DeviceManager> m_DeviceManager;
        std::shared_ptr<Process::ProcessManager> m_ProcessManager;
        std::shared_ptr<Task::TaskManager> m_TaskManager;
        std::shared_ptr<TaskGenerator> m_TaskGenerator;
        std::shared_ptr<MessageBus> m_MessageBus;
        std::shared_ptr<PluginManager> m_PluginManager;
        // This is special for dynamic load oatpp server controller before starting server
        std::shared_ptr<ModuleLoader> m_cModuleLoader;

        struct QueueWrapper
        {
            moodycamel::ConcurrentQueue<IMessage> queue;
        };
        std::shared_ptr<QueueWrapper> m_MessageQueue;
    };
    extern std::shared_ptr<LithiumApp> MyApp;
} // namespace Lithium
