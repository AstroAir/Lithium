/*
 * component_manager.hpp
 *
 * Copyright (C) 2023-2024 Max Qian <lightapt.com>
 */

/*************************************************

Date: 2024-1-4

Description: Component Manager (the core of the plugin system)

**************************************************/

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "atom/components/component.hpp"
#include "atom/components/types.hpp"
#include "atom/system/env.hpp"

#include "dependency.hpp"

#include "atom/type/json_fwd.hpp"
using json = nlohmann::json;

namespace lithium {

class AddonManager;
class Compiler;
class ModuleLoader;
class Sandbox;

struct ComponentEntry {
    std::string name;
    std::string func_name;
    std::string component_type;
    std::string module_name;
    std::vector<std::string> dependencies;

    ComponentEntry(std::string name, std::string func_name,
                   std::string component_type, std::string module_name)
        : name(std::move(name)),
          func_name(std::move(func_name)),
          component_type(std::move(component_type)),
          module_name(std::move(module_name)) {}
};
class ComponentManagerImpl;
class ComponentManager {
public:
    explicit ComponentManager();
    ~ComponentManager();

    auto initialize() -> bool;
    auto destroy() -> bool;

    static auto createShared() -> std::shared_ptr<ComponentManager>;

    auto loadComponent(const json& params) -> bool;
    auto unloadComponent(const json& params) -> bool;
    auto reloadComponent(const json& params) -> bool;
    auto reloadAllComponents() -> bool;

    auto getComponent(const std::string& component_name)
        -> std::optional<std::weak_ptr<Component>>;
    auto getComponentInfo(const std::string& component_name)
        -> std::optional<json>;
    auto getComponentList() -> std::vector<std::string>;

private:
    auto getFilesInDir(const std::string& path) -> std::vector<std::string>;
    auto getQualifiedSubDirs(const std::string& path)
        -> std::vector<std::string>;
    auto checkComponent(const std::string& module_name,
                        const std::string& module_path) -> bool;
    auto loadComponentInfo(const std::string& module_path,
                           const std::string& component_name) -> bool;
    auto checkComponentInfo(const std::string& module_name,
                            const std::string& component_name) -> bool;
    auto loadSharedComponent(
        const std::string& component_name, const std::string& addon_name,
        const std::string& module_path, const std::string& entry,
        const std::vector<std::string>& dependencies) -> bool;
    auto unloadSharedComponent(const std::string& component_name,
                               bool forced) -> bool;
    auto reloadSharedComponent(const std::string& component_name) -> bool;

    // Max: 特意增加的对INDIDriver的支持，也可以作为管道组件的通用的支持
    auto loadStandaloneComponent(
        const std::string& component_name, const std::string& addon_name,
        const std::string& module_path, const std::string& entry,
        const std::vector<std::string>& dependencies) -> bool;
    auto unloadStandaloneComponent(const std::string& component_name,
                               bool forced) -> bool;
    auto reloadStandaloneComponent(const std::string& component_name) -> bool;

    std::unique_ptr<ComponentManagerImpl> impl_;
};

}  // namespace lithium
