/*
 * _component.cpp
 *
 * Copyright (C) 2023-2024 Max Qian <lightapt.com>
 */

/*************************************************

Date: 2024-4-13

Description: Component of Atom-System

**************************************************/

#include "_component.hpp"

#include "atom/log/loguru.hpp"

#include "module/battery.hpp"
#include "module/cpu.hpp"
#include "module/disk.hpp"
#include "module/gpu.hpp"
#include "module/memory.hpp"
#include "module/os.hpp"
#include "module/wifi.hpp"

#include "command.hpp"
#include "crash.hpp"
#include "os.hpp"
#include "pidwatcher.hpp"
#include "platform.hpp"
#include "register.hpp"
#include "user.hpp"

#include "_constant.hpp"

using namespace atom::system;

SystemComponent::SystemComponent(const std::string &name) : Component(name) {
    DLOG_F(INFO, "SystemComponent::SystemComponent");
    def("cpu_usage", &getCurrentCpuUsage, "cpu",
                    "Get current CPU usage percentage");
    def("cpu_temperature", &getCurrentCpuTemperature, "cpu",
                    "Get current CPU temperature");
    def("memory_usage", &getMemoryUsage, "memory",
                    "Get current memory usage percentage");
    def("is_charging", &isBatteryCharging, PointerSentinel(this),
                    "battery", "Check if the battery is charging");
    def("battery_level", &getCurrentBatteryLevel,
                    PointerSentinel(this), "battery",
                    "Get current battery level");
    def("disk_usage", &getDiskUsage, "disk",
                    "Get current disk usage percentage");
    def("is_hotspot_connected", &isHotspotConnected, "wifi",
                    "Check if the hotspot is connected");
    def("wired_network", &getCurrentWiredNetwork, "wifi",
                    "Get current wired network");
    def("wifi_name", &getCurrentWifi, "wifi",
                    "Get current wifi name");
    def("current_ip", &getHostIPs, "network",
                    "Get current IP address");
    def("gpu_info", &getGPUInfo, "gpu", "Get GPU info");
    def("os_name", &getOSName, PointerSentinel(this), "os",
                    "Get OS name");
    def("os_version", &getOSVersion, PointerSentinel(this), "os",
                    "Get OS version");
    def("run_commands", &executeCommands, "os",
                    "Run a list of system commands");
    def("run_command_env", &executeCommandWithEnv, "os",
                    "Run a system command with environment variables");
    def("run_command_status", &executeCommandWithStatus, "os",
                    "Run a system command and get its status");
    def("kill_process", &killProcess, "os",
                    "Kill a process by its PID");

    def("walk", &walk, "os", "Walk a directory");
    def("fwalk", &fwalk, "os", "Walk a directory");
    def("uname", &uname, "os", "Get uname information");
    def("ctermid", &ctermid, "os", "Get current terminal ID");
    def("jwalk", &jwalk, "os", "Walk a directory");
    def("getpriority", &getpriority, "os",
                    "Get current process priority");
    def("getlogin", &getlogin, "os", "Get current user name");
    def("Environ", &Environ, "os", "Get environment variables");

    def("user_group", &getUserGroups, "user",
                    "Get current user groups");
    def("user_id", &getUserId, "user", "Get current user ID");
    def("user_host", &getHostname, "user",
                    "Get current user hostname");
    def("user_name", &getUsername, "user", "Get current user name");
    def("user_home", &getHomeDirectory, "user",
                    "Get current user home directory");

    def("user_shell", &getLoginShell, "user",
                    "Get current user login shell");

    def("user_groups", &getUserGroups, "user",
                    "Get current user groups");

    addVariable("platform", platform, "Platform", "os_name", "os");
    addVariable("architecture", architecture, "Architecture", "os_arch", "os");
    addVariable("os_version", os_version, "OS Version", "kernel_version", "os");
    addVariable("compiler", compiler, "Compiler", "builder", "os");

    def("make_pidwatcher", &SystemComponent::makePidWatcher, PointerSentinel(this),
                    "os", "Make a PID watcher");
    def("start_watcher", &SystemComponent::startPidWatcher, PointerSentinel(this),
                    "os", "Start a PID watcher");
    def("stop_watcher", &SystemComponent::stopPidWatcher, PointerSentinel(this),
                    "os", "Stop a PID watcher");
    def("switch_watcher", &SystemComponent::switchPidWatcher, PointerSentinel(this),
                    "os", "Switch a PID watcher");
    def("set_watcher_exit", &SystemComponent::setPidWatcherExitCallback,
                    PointerSentinel(this), "os",
                    "Set a PID watcher exit callback");
    def("set_watcher_monitor", &SystemComponent::setPidWatcherMonitorFunction,
                    PointerSentinel(this), "os",
                    "Set a PID watcher monitor callback");

#if ENABLE_REGISTRY_SUPPORT
    def("get_registry_subkeys", &getRegistrySubKeys, "os",
                    "Get registry subkeys");
    def("get_registry_values", &getRegistryValues, "os",
                    "Get registry values");
    def("delete_registry_subkey", &deleteRegistrySubKey, "os",
                    "Delete registry subkey");
    def("modify_registry_value", &modifyRegistryValue, "os",
                    "Modify registry value");
    def("recursively_enumerate_registry_subkeys",
                    &recursivelyEnumerateRegistrySubKeys, "os",
                    "Recursively enumerate registry subkeys");
    def("find_registry_key", &findRegistryKey, "os",
                    "Find registry key");
    def("find_registry_value", &findRegistryValue, "os",
                    "Find registry value");
    def("backup_registry", &backupRegistry, "os",
                    "Backup registry");
    def("export_registry", &exportRegistry, "os",
                    "Export registry");
    def("delete_registry_value", &deleteRegistryValue, "os",
                    "Delete registry value");
#endif

    def("save_crashreport", &saveCrashLog, "os",
                    "Save crash report");
}

SystemComponent::~SystemComponent() {
    DLOG_F(INFO, "SystemComponent::~SystemComponent");
}

bool SystemComponent::initialize() { return true; }

bool SystemComponent::destroy() { return true; }

double SystemComponent::getCurrentBatteryLevel() {
    return atom::system::getBatteryInfo().currentNow;
}

bool SystemComponent::isBatteryCharging() {
    return atom::system::getBatteryInfo().isCharging;
}

std::string SystemComponent::getOSName() {
    return atom::system::getOperatingSystemInfo().osName;
}

std::string SystemComponent::getOSVersion() {
    return atom::system::getOperatingSystemInfo().osVersion;
}

std::string SystemComponent::getKernelVersion() {
    return atom::system::getOperatingSystemInfo().kernelVersion;
}

std::string SystemComponent::getArchitecture() {
    return atom::system::getOperatingSystemInfo().architecture;
}

void SystemComponent::makePidWatcher(const std::string &name) {
    if (m_pidWatchers.find(name) != m_pidWatchers.end()) {
        return;
    }
    m_pidWatchers[name] = std::make_shared<PidWatcher>();
}

bool SystemComponent::startPidWatcher(const std::string &name,
                                      const std::string &pid) {
    return m_pidWatchers[name]->Start(pid);
}

void SystemComponent::stopPidWatcher(const std::string &name) {
    m_pidWatchers[name]->Stop();
}

bool SystemComponent::switchPidWatcher(const std::string &name,
                                       const std::string &pid) {
    return m_pidWatchers[name]->Switch(pid);
}
void SystemComponent::setPidWatcherExitCallback(
    const std::string &name, const std::function<void()> &callback) {
    m_pidWatchers[name]->SetExitCallback(callback);
}

void SystemComponent::setPidWatcherMonitorFunction(
    const std::string &name, const std::function<void()> &callback,
    std::chrono::milliseconds interval) {
    m_pidWatchers[name]->SetMonitorFunction(callback, interval);
}

void SystemComponent::getPidByName(const std::string &name,
                                   const std::string &pid) {
    m_pidWatchers[name]->GetPidByName(pid);
}