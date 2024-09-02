/**
 * @file task_interpreter.cpp
 * @brief Task Interpreter for managing and executing scripts.
 *
 * This file defines the `TaskInterpreter` class, which is responsible for
 * loading, managing, and executing tasks represented as JSON scripts. The
 * `TaskInterpreter` class provides functionality to register functions and
 * exception handlers, set and retrieve variables, and control script execution
 * flow (e.g., pause, resume, stop). It supports various script operations such
 * as parsing labels, executing steps, handling exceptions, and evaluating
 * expressions.
 *
 * The class also supports asynchronous operations and event handling, making it
 * suitable for dynamic and complex scripting environments.
 *
 * @date 2023-04-03
 * @author Max Qian <lightapt.com>
 * @copyright Copyright (C) 2023-2024 Max Qian
 */

#include "config.h"

#include "generator.hpp"
#include "manager.hpp"
#include "task.hpp"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <stack>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atom/async/pool.hpp"
#include "atom/error/exception.hpp"
#include "atom/function/abi.hpp"
#include "atom/function/global_ptr.hpp"
#include "atom/log/loguru.hpp"
#include "task/loader.hpp"

#include "utils/constant.hpp"

#include "matchit/matchit.h"

// #define ENABLE_DEBUG 1

#if ENABLE_DEBUG
#include <iostream>
#endif

using namespace std::literals;

auto operator<<(std::ostream& os, const std::error_code& ec) -> std::ostream& {
    os << "Error Code: " << ec.value() << ", Category: " << ec.category().name()
       << ", Message: " << ec.message();
    return os;
}

namespace lithium {

auto determineType(const json& value) -> VariableType {
    if (value.is_number()) {
        return VariableType::NUMBER;
    }
    if (value.is_string()) {
        return VariableType::STRING;
    }
    if (value.is_boolean()) {
        return VariableType::BOOLEAN;
    }
    if (value.is_object() || value.is_array()) {
        return VariableType::JSON;
    }
    return VariableType::UNKNOWN;
}

class TaskInterpreterImpl {
public:
    std::unordered_map<std::string, json> scripts_;
    std::unordered_map<std::string, json> scriptHeaders_;  // 存储脚本头部信息
    std::unordered_map<std::string, std::pair<VariableType, json>> variables_;
    std::unordered_map<std::string, std::error_code> customErrors_;
    std::unordered_map<std::string, std::function<json(const json&)>>
        functions_;
    std::unordered_map<std::string, size_t> labels_;
    std::unordered_map<std::string, std::function<void(const std::exception&)>>
        exceptionHandlers_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> pauseRequested_{false};
    std::atomic<bool> isRunning_{false};
    std::jthread executionThread_;
    std::vector<std::string> callStack_;
    mutable std::shared_timed_mutex mtx_;
    std::condition_variable_any cv_;
    std::queue<std::pair<std::string, json>> eventQueue_;

    std::shared_ptr<TaskGenerator> taskGenerator_;
    std::shared_ptr<atom::async::ThreadPool<>> threadPool_;
};

TaskInterpreter::TaskInterpreter()
    : impl_(std::make_unique<TaskInterpreterImpl>()) {
    if (auto ptr = GetPtrOrCreate<atom::async::ThreadPool<>>(
            "lithium.task.pool",
            [] { return std::make_shared<atom::async::ThreadPool<>>(); });
        ptr) {
        impl_->threadPool_ = ptr;
    } else {
        THROW_RUNTIME_ERROR("Failed to create task pool.");
    }
    if (auto ptr = GetPtrOrCreate<TaskGenerator>("lithium.task.generator", [] {
            return std::make_shared<TaskGenerator>();
        })) {
        impl_->taskGenerator_ = ptr;
    } else {
        THROW_RUNTIME_ERROR("Failed to create task generator.");
    }
}

TaskInterpreter::~TaskInterpreter() {
    if (impl_->executionThread_.joinable()) {
        stop();
        // impl_->executionThread_.join();
    }
}

auto TaskInterpreter::createShared() -> std::shared_ptr<TaskInterpreter> {
    return std::make_shared<TaskInterpreter>();
}

void TaskInterpreter::loadScript(const std::string& name, const json& script) {
#if ENABLE_DEBUG
    LOG_F(INFO, "Loading script: {} with {}", name, script.dump());
#endif
    std::unique_lock lock(impl_->mtx_);
    impl_->scripts_[name] = script.contains("steps") ? script["steps"] : script;
    lock.unlock();
    if (prepareScript(impl_->scripts_[name])) {
        parseLabels(impl_->scripts_[name]);
        if (script.contains("header")) {
            const auto& header = script["header"];
            LOG_F(INFO, "Loading script: {} (version: {}, author: {})",
                  header.contains("name") ? header["name"].get<std::string>()
                                          : name,
                  header.contains("version")
                      ? header["version"].get<std::string>()
                      : "unknown",
                  header.contains("author")
                      ? header["author"].get<std::string>()
                      : "unknown");
            // 存储头部信息
            impl_->scriptHeaders_[name] = header;
            if (header.contains("auto_execute") &&
                header["auto_execute"].is_boolean() &&
                header["auto_execute"].get<bool>()) {
                LOG_F(INFO, "Auto-executing script '{}'.", name);
                execute(name);
            }
        } else {
            LOG_F(INFO, "Loading script: {} (no header information)", name);
        }
    } else {
        THROW_RUNTIME_ERROR("Failed to prepare script: " + name);
    }
}

void TaskInterpreter::unloadScript(const std::string& name) {
    std::unique_lock lock(impl_->mtx_);
    impl_->scripts_.erase(name);
}

auto TaskInterpreter::hasScript(const std::string& name) const -> bool {
    std::shared_lock lock(impl_->mtx_);
    return impl_->scripts_.contains(name);
}

auto TaskInterpreter::getScript(const std::string& name) const
    -> std::optional<json> {
    std::shared_lock lock(impl_->mtx_);
    if (impl_->scripts_.contains(name)) {
        return impl_->scripts_.at(name);
    }
    return std::nullopt;
}

auto TaskInterpreter::prepareScript(json& script) -> bool {
    try {
        impl_->taskGenerator_->processJson(script);
    } catch (const json::parse_error& e) {
        LOG_F(ERROR, "Failed to parse script: {}", e.what());
        return false;
    } catch (const std::exception& e) {
        LOG_F(ERROR, "Failed to process script: {}", e.what());
        return false;
    }
    return true;
}

void TaskInterpreter::registerFunction(const std::string& name,
                                       std::function<json(const json&)> func) {
    std::unique_lock lock(impl_->mtx_);
    if (impl_->functions_.find(name) != impl_->functions_.end()) {
        THROW_RUNTIME_ERROR("Function '" + name + "' is already registered.");
    }
    impl_->functions_[name] = std::move(func);
    LOG_F(INFO, "Function registered: {}", name);
}

void TaskInterpreter::registerExceptionHandler(
    const std::string& name,
    std::function<void(const std::exception&)> handler) {
    std::unique_lock lock(impl_->mtx_);
    impl_->exceptionHandlers_[name] = std::move(handler);
}

void TaskInterpreter::setVariable(const std::string& name, const json& value,
                                  VariableType type) {
    std::unique_lock lock(impl_->mtx_);
    impl_->cv_.wait(lock, [this]() { return !impl_->isRunning_; });

    VariableType currentType = determineType(value);
    if (currentType != type) {
        THROW_RUNTIME_ERROR(
            "Type mismatch when setting variable '" + name + "'. Expected " +
            std::to_string(static_cast<int>(type)) + ", got " +
            std::to_string(static_cast<int>(currentType)) + ".");
    }

    if (impl_->variables_.find(name) != impl_->variables_.end()) {
        if (impl_->variables_[name].first != type) {
            THROW_RUNTIME_ERROR("Type mismatch: Variable '" + name +
                                "' already exists with a different type.");
        }
    }

    impl_->variables_[name] = {type, value};
}

auto TaskInterpreter::getVariableImmediate(const std::string& name) const
    -> json {
    std::shared_lock lock(impl_->mtx_);
    if (impl_->variables_.find(name) == impl_->variables_.end()) {
        THROW_RUNTIME_ERROR("Variable '" + name + "' is not defined.");
    }
    return impl_->variables_.at(name).second;
}

auto TaskInterpreter::getVariable(const std::string& name) const -> json {
    std::unique_lock lock(impl_->mtx_);
    impl_->cv_.wait(lock, [this]() { return !impl_->isRunning_; });

    if (impl_->variables_.find(name) == impl_->variables_.end()) {
        THROW_RUNTIME_ERROR("Variable '" + name + "' is not defined.");
    }
    return impl_->variables_.at(name).second;
}

void TaskInterpreter::parseLabels(const json& script) {
    std::unique_lock lock(impl_->mtx_);
    LOG_F(INFO, "Parsing labels...");
    for (size_t i = 0; i < script.size(); ++i) {
        if (script[i].contains("label")) {
            impl_->labels_[script[i]["label"]] = i;
        }
    }
}

void TaskInterpreter::execute(const std::string& scriptName) {
    LOG_F(INFO, "Executing script: {}", scriptName);
    impl_->stopRequested_ = false;
    impl_->isRunning_ = true;
    if (impl_->executionThread_.joinable()) {
        impl_->executionThread_.join();
    }

    if (!impl_->scripts_.contains(scriptName)) {
        THROW_RUNTIME_ERROR("Script '" + scriptName + "' not found.");
    }

    impl_->executionThread_ = std::jthread([this, scriptName]() {
        std::exception_ptr exPtr = nullptr;
        try {
            std::shared_lock lock(impl_->mtx_);
            const json& script = impl_->scripts_.at(scriptName);
            lock.unlock();

            size_t i = 0;
            while (i < script.size() && !impl_->stopRequested_) {
                if (!executeStep(script[i], i, script)) {
                    break;
                }
                i++;
            }
        } catch (...) {
            exPtr = std::current_exception();
        }

        impl_->isRunning_ = false;
        impl_->cv_.notify_all();

        if (exPtr) {
            try {
                std::rethrow_exception(exPtr);
            } catch (const std::exception& e) {
                handleException(scriptName, e);
            }
        }
    });
}

void TaskInterpreter::stop() {
    impl_->stopRequested_ = true;
    if (impl_->executionThread_.joinable()) {
        impl_->executionThread_.join();
    }
}

void TaskInterpreter::pause() {
    LOG_F(INFO, "Pausing task interpreter...");
    impl_->pauseRequested_ = true;
}

void TaskInterpreter::resume() {
    LOG_F(INFO, "Resuming task interpreter...");
    impl_->pauseRequested_ = false;
    impl_->cv_.notify_all();
}

void TaskInterpreter::queueEvent(const std::string& eventName,
                                 const json& eventData) {
    std::unique_lock lock(impl_->mtx_);
    impl_->eventQueue_.emplace(eventName, eventData);
    impl_->cv_.notify_all();
}

auto TaskInterpreter::executeStep(const json& step, size_t& idx,
                                  const json& script) -> bool {
    if (impl_->stopRequested_) {
        return false;
    }

    try {
        using namespace matchit;
        std::string type = step["type"];
        match(type)(
            pattern | "call" = [this, &step] { executeCall(step); },
            pattern | "condition" =
                [this, &step, &idx, &script] {
                    executeCondition(step, idx, script);
                },
            pattern | "loop" = [this, &step, &idx,
                                &script] { executeLoop(step, idx, script); },
            pattern |
                "while" = [this, &step, &idx,
                           &script] { executeWhileLoop(step, idx, script); },
            pattern | "goto" = [this, &step, &idx,
                                &script] { executeGoto(step, idx, script); },
            pattern |
                "switch" = [this, &step, &idx,
                            &script] { executeSwitch(step, idx, script); },
            pattern | "delay" = [this, &step] { executeDelay(step); },
            pattern |
                "parallel" = [this, &step, &idx,
                              &script] { executeParallel(step, idx, script); },
            pattern |
                "nested_script" = [this, &step] { executeNestedScript(step); },
            pattern | "assign" = [this, &step] { executeAssign(step); },
            pattern | "import" = [this, &step] { executeImport(step); },
            pattern | "wait_event" = [this, &step] { executeWaitEvent(step); },
            pattern | "print" = [this, &step] { executePrint(step); },
            pattern | "async" = [this, &step] { executeAsync(step); },
            pattern | "try" = [this, &step, &idx,
                               &script] { executeTryCatch(step, idx, script); },
            pattern | "function" = [this, &step] { executeFunction(step); },
            pattern |
                "return" = [this, &step, &idx] { executeReturn(step, idx); },
            pattern |
                "break" = [this, &step, &idx] { executeBreak(step, idx); },
            pattern | "continue" = [this, &step,
                                    &idx] { executeContinue(step, idx); },
            pattern | "message" = [this, &step] { executeMessage(step); },
            pattern | "broadcast_event" =
                [this, &step] { executeBroadcastEvent(step); },
            pattern | "listen_event" =
                [this, &step, &idx] { executeListenEvent(step, idx); },
            pattern | "retry" = [this, &step, &idx,
                                 &script] { executeRetry(step, idx, script); },
            pattern |
                "schedule" = [this, &step, &idx,
                              &script] { executeSchedule(step, idx, script); },
            pattern | "scope" = [this, &step, &idx,
                                 &script] { executeScope(step, idx, script); },
            pattern |
                "function_def" = [this, &step] { executeFunctionDef(step); },
            pattern | "throw" = [this, &step] { executeThrow(step); },
            pattern | _ =
                [&step] {
                    THROW_RUNTIME_ERROR("Unknown step type: " +
                                        step["type"].get<std::string>());
                });
        return true;
    } catch (const std::exception& e) {
        LOG_F(ERROR, "Error during step {} execution: {}",
              step["type"].get<std::string>(), e.what());
        handleException(script["name"], e);
        return false;
    }
}

void TaskInterpreter::executeCondition(const json& step, size_t& idx,
                                       const json& script) {
    try {
        if (!step.contains("condition")) {
            THROW_INVALID_ARGUMENT(
                "Condition step is missing 'condition' field.");
        }

        json conditionResult = evaluate(step["condition"]);

        if (!conditionResult.is_boolean()) {
            THROW_INVALID_ARGUMENT("Condition result must be boolean.");
        }

        // 根据条件执行分支
        if (conditionResult.get<bool>()) {
            executeStep(step["true"], idx, script);
        } else if (step.contains("false")) {
            executeStep(step["false"], idx, script);
        }
    } catch (const std::exception& e) {
        LOG_F(ERROR, "Error during executeCondition: {}", e.what());
        throw;
    }
}

auto TaskInterpreter::executeLoop(const json& step, size_t& idx,
                                  const json& script) -> bool {
    try {
        if (!step.contains("loop_iterations")) {
            THROW_INVALID_ARGUMENT(
                "Loop step is missing 'loop_iterations' field.");
        }

        int iterations = evaluate(step["loop_iterations"]).get<int>();

        for (int i = 0; i < iterations && !impl_->stopRequested_; i++) {
            for (const auto& nestedStep : step["steps"]) {
                if (!executeStep(nestedStep, idx, script)) {
                    return false;
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_F(ERROR, "Error during executeLoop: {}", e.what());
        throw;
    }

    return true;
}

/*
{
    "type": "while",
    "condition": {"type": "greater_than", "left": "$x", "right": 0},
    "steps": [
        {"type": "print", "message": "x is: $x"},
        {"type": "assign", "variable": "x", "value": {"$sub": ["$x", 1]}}
    ]
}
*/
void TaskInterpreter::executeWhileLoop(const json& step, size_t& idx,
                                       const json& script) {
    LOG_F(INFO, "Executing while loop...");
    try {
        while (evaluate(step["condition"]).get<bool>()) {
            executeSteps(step["steps"], idx, script);
        }
    } catch (const std::exception& e) {
        LOG_F(ERROR, "Error during executeWhileLoop: {}", e.what());
        throw;
    }
}

void TaskInterpreter::executeGoto(const json& step, size_t& idx,
                                  const json& script) {
    static const int MAX_GOTO_DEPTH = 100;  // 设置最大跳转深度
    static std::unordered_map<std::string, int>
        gotoDepthCounter;  // 跳转深度计数器
    static std::unordered_map<std::string, size_t> labelCache;  // 标签位置缓存

    // 标签字段验证
    if (!step.contains("label") || !step["label"].is_string()) {
        THROW_INVALID_ARGUMENT("Goto step is missing a valid 'label' field.");
    }

    // 获取标签和当前上下文
    std::string label = step["label"];
    std::string currentContext =
        script.contains("context") ? script["context"].get<std::string>() : "";
    std::string fullLabel =
        currentContext.empty() ? label : currentContext + "::" + label;

    // 检查缓存
    if (labelCache.find(fullLabel) != labelCache.end()) {
        idx = labelCache[fullLabel];
        gotoDepthCounter[fullLabel]++;
        if (gotoDepthCounter[fullLabel] > MAX_GOTO_DEPTH) {
            THROW_RUNTIME_ERROR("Exceeded maximum GOTO depth for label '" +
                                fullLabel + "'. Possible infinite loop.");
        }
        return;
    }

    // 查找标签并验证存在性
    if (impl_->labels_.find(fullLabel) == impl_->labels_.end()) {
        THROW_RUNTIME_ERROR("Label '" + fullLabel +
                            "' not found in the script.");
    }

    // 更新索引并缓存结果
    idx = impl_->labels_.at(fullLabel);
    labelCache[fullLabel] = idx;

    // 更新跳转深度计数器
    gotoDepthCounter[fullLabel] = 1;
}

void TaskInterpreter::executeSwitch(const json& step, size_t& idx,
                                    const json& script) {
    try {
        if (!step.contains("variable") || !step["variable"].is_string()) {
            THROW_MISSING_ARGUMENT("Missing 'variable' parameter.");
        }
        std::string variable = step["variable"];
        if (!impl_->variables_.contains(variable)) {
            THROW_OBJ_NOT_EXIST("Variable '" + variable + "' not found.");
        }

        json value = evaluate(impl_->variables_[variable]);

        bool caseFound = false;

        if (step.contains("cases")) {
            for (const auto& caseBlock : step["cases"]) {
                if (caseBlock["case"] == value) {
                    for (const auto& nestedStep : caseBlock["steps"]) {
                        executeStep(nestedStep, idx, script);
                    }
                    caseFound = true;
                    break;
                }
            }
        }

        if (!caseFound && step.contains("default") &&
            step["default"].contains("steps")) {
            for (const auto& nestedStep : step["default"]["steps"]) {
                executeStep(nestedStep, idx, script);
            }
        } else if (!caseFound) {
            LOG_F(WARNING, "No matching case found for variable '{}'",
                  variable);
        }
    } catch (const std::exception& e) {
        LOG_F(ERROR, "Error during executeSwitch: {}", e.what());
        throw;
    }
}

void TaskInterpreter::executeDelay(const json& step) {
    if (!step.contains("milliseconds")) {
        THROW_MISSING_ARGUMENT("Missing 'milliseconds' parameter.");
    }
    if (!step["milliseconds"].is_number()) {
        THROW_INVALID_ARGUMENT("'milliseconds' must be a number.");
    }
    auto milliseconds =
        std::chrono::milliseconds(evaluate(step["milliseconds"]).get<int>());
    std::this_thread::sleep_for(milliseconds);
}

void TaskInterpreter::executeParallel(const json& step,
                                      [[maybe_unused]] size_t& idx,
                                      const json& script) {
    try {
        if (!step.contains("steps") || !step["steps"].is_array()) {
            THROW_INVALID_ARGUMENT(
                "Parallel step is missing a valid 'steps' array.");
        }
        std::vector<std::future<void>> futures;

        for (const auto& nestedStep : step["steps"]) {
            futures.emplace_back(
                impl_->threadPool_->enqueue([this, nestedStep, &script]() {
                    try {
                        size_t nestedIdx = 0;
                        executeStep(nestedStep, nestedIdx, script);
                    } catch (const std::exception& e) {
                        LOG_F(ERROR, "Error during parallel task execution: {}",
                              e.what());
                        throw;
                    }
                }));
        }

        for (auto& future : futures) {
            future.get();
        }
    } catch (const std::exception& e) {
        LOG_F(ERROR, "Error during executeParallel: {}", e.what());
        std::throw_with_nested(e);
    }
}

void TaskInterpreter::executeCall(const json& step) {
    LOG_F(INFO, "Executing call step");

    try {
        if (!step.contains("function") || !step["function"].is_string()) {
            THROW_MISSING_ARGUMENT(
                "Call step is missing a valid 'function' field.");
        }
        std::string functionName = step["function"];

        json params = step.contains("params") ? step["params"] : json::object();

        // 评估参数的值
        for (const auto& [key, value] : params.items()) {
            params[key] = evaluate(value);
        }

        std::string targetVariable =
            step.contains("result") ? step["result"].get<std::string>() : "";

        json returnValue;

        // 仅在查找函数时加锁，执行时不加锁以避免卡死
        {
            std::shared_lock lock(impl_->mtx_);
            if (impl_->functions_.contains(functionName)) {
                lock.unlock();
                returnValue = impl_->functions_[functionName](params);
            } else {
                THROW_RUNTIME_ERROR("Function '" + functionName +
                                    "' not found.");
            }
        }

        // 如果指定了目标变量名，则将返回值存储到该变量中
        if (!targetVariable.empty()) {
            std::unique_lock ulock(impl_->mtx_);
            impl_->variables_[targetVariable] = {determineType(returnValue),
                                                 returnValue};
        }
    } catch (const std::exception& e) {
        LOG_F(ERROR, "Error during executeCall: {}", e.what());
        throw;
    }
}

/*
{
    "type": "function_def",
    "name": "add",
    "params": ["a", "b"],
    "default_values": {
        "b": 10
    },
    "steps": [
        { "type": "assign", "variable": "result", "value": { "$add": ["$a",
"$b"] } }
    ],
    "return": "$result"
}
{
    "type": "call",
    "function": "add",
    "params": {
        "a": 5
    },
    "result": "sum"
}

*/
void TaskInterpreter::executeFunctionDef(const json& step) {
    LOG_F(INFO, "Executing function_def step");
    if (!step.contains("name") || !step["name"].is_string()) {
        THROW_INVALID_ARGUMENT("Function definition requires a 'name' field.");
    }

    std::string functionName = step["name"];
    std::vector<std::string> paramNames;
    if (step.contains("params") && step["params"].is_array()) {
        paramNames = step["params"].get<std::vector<std::string>>();
    }

    json defaultValues = step.contains("default_values")
                             ? step["default_values"]
                             : json::object();
    json closure = captureClosureVariables();

    impl_->functions_[functionName] =
        [this, step, paramNames, defaultValues,
         closure](const json& passedParams) mutable -> json {
        size_t idx = 0;
        json mergedParams = defaultValues;

        try {
            // 合并传递的参数和默认值
            for (const auto& paramName : paramNames) {
                if (passedParams.contains(paramName)) {
                    mergedParams[paramName] = passedParams[paramName];
                }
            }

            // 恢复闭包变量
            restoreClosureVariables(closure);

            // 设置函数参数
            for (const auto& [key, value] : mergedParams.items()) {
                std::unique_lock lock(impl_->mtx_);
                impl_->variables_[key] = {
                    determineType(value),
                    value,
                };
            }

            // 执行函数体
            json returnValue;
            executeSteps(step["steps"], idx, step);

            // 如果存在返回值
            if (impl_->variables_.contains("__return_value__")) {
                returnValue = impl_->variables_.at("__return_value__").second;
                impl_->variables_.erase("__return_value__");
            }

            return returnValue;  // 返回结果
        } catch (const std::exception& e) {
            LOG_F(ERROR, "Error during function execution: {}", e.what());
            throw;
        }
    };
}

auto TaskInterpreter::captureClosureVariables() const -> json {
    json closure;
    for (const auto& var : impl_->variables_) {
        closure[var.first] =
            var.second.second;  // Capture the current value of the variable
    }
    return closure;
}

void TaskInterpreter::restoreClosureVariables(const json& closure) {
    for (const auto& [key, value] : closure.items()) {
        impl_->variables_[key] = {determineType(value), value};
    }
}

/*
{
    "type": "scope",
    "variables": {
        "local_var": 10
    },
    "functions": [
        {
            "name": "calculate",
            "params": ["x"],
            "steps": [
                { "type": "assign", "variable": "result", "value": { "$add":
["$x", "$local_var"] } }
            ]
        }
    ],
    "steps": [
        { "type": "call", "function": "calculate", "params": { "x": 5 } },
        { "type": "throw", "message": "Something went wrong!" }  // Example
error to trigger error handling
    ],
    "on_error": [
        { "type": "print", "message": "Handled error within scope!" }
    ],
    "cleanup": [
        { "type": "print", "message": "Cleanup after scope." }
    ]
}
*/
void TaskInterpreter::executeScope(const json& step, size_t& idx,
                                   const json& script) {
    // Store old variable states to restore after the scope ends
    std::unordered_map<std::string, std::pair<VariableType, json>> oldVars;
    std::unordered_map<std::string, std::function<json(const json&)>>
        oldFunctions;

    // Capture scope variables
    if (step.contains("variables") && step["variables"].is_object()) {
        for (const auto& [name, value] : step["variables"].items()) {
            if (impl_->variables_.find(name) != impl_->variables_.end()) {
                oldVars[name] = impl_->variables_[name];
            }
            setVariable(name, value, determineType(value));
        }
    }

    // Capture scope-specific functions
    if (step.contains("functions") && step["functions"].is_array()) {
        for (const auto& funcDef : step["functions"]) {
            if (funcDef.contains("name") && funcDef["name"].is_string()) {
                std::string funcName = funcDef["name"];
                if (impl_->functions_.find(funcName) !=
                    impl_->functions_.end()) {
                    oldFunctions[funcName] = impl_->functions_[funcName];
                }
                executeFunctionDef(funcDef);  // Define the new scope function
            }
        }
    }

    try {
        // Execute scope steps
        if (step.contains("steps") && step["steps"].is_array()) {
            executeSteps(step["steps"], idx, script);
        }
    } catch (const std::exception& e) {
        // Scope-specific error handling
        if (step.contains("on_error") && step["on_error"].is_array()) {
            LOG_F(WARNING, "Error occurred within scope: {}", e.what());
            size_t errorIdx = 0;
            executeSteps(step["on_error"], errorIdx, script);
        } else {
            throw;  // Rethrow if no specific error handling is provided
        }
    }

    // Execute scope cleanup
    if (step.contains("cleanup") && step["cleanup"].is_array()) {
        size_t cleanupIdx = 0;
        executeSteps(step["cleanup"], cleanupIdx, script);
    }

    // Restore old functions
    for (const auto& [name, func] : oldFunctions) {
        impl_->functions_[name] = func;  // Restore old function if it existed
    }

    // Restore old variables
    for (const auto& [name, var] : oldVars) {
        impl_->variables_[name] = var;  // Restore old variable
    }

    // Remove variables that were only within the scope
    if (step.contains("variables") && step["variables"].is_object()) {
        for (const auto& [name, _] : step["variables"].items()) {
            if (oldVars.find(name) == oldVars.end()) {
                impl_->variables_.erase(
                    name);  // Remove variables specific to the scope
            }
        }
    }
}

void TaskInterpreter::executeNestedScript(const json& step) {
    LOG_F(INFO, "Executing nested script step");
    std::string scriptName = step["script"];
    std::shared_lock lock(impl_->mtx_);
    if (impl_->scripts_.find(scriptName) != impl_->scripts_.end()) {
        execute(scriptName);
    } else {
        THROW_RUNTIME_ERROR("Script '" + scriptName + "' not found.");
    }
}

void TaskInterpreter::executeAssign(const json& step) {
    try {
        if (!step.contains("variable") || !step["variable"].is_string()) {
            THROW_INVALID_ARGUMENT(
                "Assign step is missing a valid 'variable' field.");
        }

        if (!step.contains("value")) {
            THROW_INVALID_ARGUMENT("Assign step is missing 'value' field.");
        }

        std::string variableName = step["variable"];
        json value = evaluate(step["value"]);

        // Instead of locking the entire method, we update the variable directly
        // since this is executed within the script execution context.
        for (int attempt = 0; attempt < 3; ++attempt) {  // Retry 3 times
            std::unique_lock lock(impl_->mtx_, std::defer_lock);
            if (lock.try_lock_for(
                    std::chrono::milliseconds(50))) {  // Wait for 50ms
                impl_->variables_[variableName] = {determineType(value), value};
                return;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(100));  // Backoff delay
        }
        THROW_RUNTIME_ERROR(
            "Failed to acquire lock after multiple attempts in executeAssign.");
    } catch (const std::exception& e) {
        LOG_F(ERROR, "Error during executeAssign: {}", e.what());
        throw;
    }
}

void TaskInterpreter::executeImport(const json& step) {
    LOG_F(INFO, "Executing import step");

    // Validate the 'script' field
    if (!step.contains("script") || !step["script"].is_string()) {
        THROW_INVALID_ARGUMENT(
            "Import step is missing a valid 'script' field.");
    }

    std::string scriptName = step["script"];
    // Handle namespace if provided
    std::string namespaceName;
    if (step.contains("namespace") && step["namespace"].is_string()) {
        namespaceName = step["namespace"];
    }

    json scriptToImport;

    bool fromFile = step.contains("fromFile") && step["fromFile"].get<bool>();
    // If we are importing from a file, check if the script is already imported
    if (fromFile) {
        if (hasScript(scriptName)) {
            LOG_F(WARNING, "Script '{}' already imported. Skipping import.",
                  scriptName);
            return;
        }

        // Try to read the script from file
        try {
            // Synchronization for async operation
            std::mutex mtx;
            std::condition_variable cv;
            bool callbackCalled = false;

            std::string fullPath = constants::TASK_FOLDER + scriptName +
                                   constants::PATH_SEPARATOR + ".json";
            LOG_F(INFO, "Importing script from file: {}", fullPath);

            // Asynchronously read the script file
            TaskLoader::asyncReadJsonFile(
                fullPath, [&](std::optional<json> data) {
                    std::unique_lock lock(mtx);
                    if (!data) {
                        THROW_FILE_NOT_FOUND("Script '" + scriptName +
                                             "' not found.");
                    }
                    if (data->is_null() || data->empty()) {
                        THROW_JSON_VALUE_ERROR("Script '" + scriptName +
                                               "' is empty or null.");
                    }
                    scriptToImport = std::move(*data);
                    callbackCalled = true;
                    cv.notify_one();  // Notify that the callback has been
                                      // called
                });

            // Wait for the callback to finish
            std::unique_lock lock(mtx);
            cv.wait(lock, [&] { return callbackCalled; });
            // Apply namespace if needed
            if (!namespaceName.empty()) {
                json namespacedScript;
                for (const auto& [key, value] : scriptToImport.items()) {
                    namespacedScript[namespaceName + "::" + key] = value;
                }
                scriptToImport = std::move(namespacedScript);
            }
            // Load the script
            loadScript(scriptName, scriptToImport);
            LOG_F(INFO, "Successfully imported script '{}'.", scriptName);
        } catch (const json ::parse_error& e) {
            THROW_JSON_PARSE_ERROR("Failed to parse script '" + scriptName +
                                   "': " + e.what());
        } catch (const std::exception& e) {
            LOG_F(ERROR, "Failed to import script '{}': {}", scriptName,
                  e.what());
            THROW_RUNTIME_ERROR("Error importing script '" + scriptName +
                                "': " + e.what());
        }
    } else {
        // Here we are importing from a local cache, so we must check
        if (!hasScript(scriptName)) {
            THROW_OBJ_NOT_EXIST("Script '" + scriptName + "' not found.");
        }
        LOG_F(INFO, "Importing script from cache: {}", scriptName);
        // This means this script is not executed yet, so we need to execute it
        // No 'auto_execute' flag found
        if (!impl_->scriptHeaders_.contains(scriptName)) {
            execute(scriptName);
        }
    }

    // Handle nested imports recursively
    if (scriptToImport.contains("imports") &&
        scriptToImport["imports"].is_array()) {
        for (const auto& nestedImport : scriptToImport["imports"]) {
            if (nestedImport.is_string()) {
                json importStep;
                importStep["script"] = nestedImport.get<std::string>();
                if (!namespaceName.empty()) {
                    importStep["namespace"] = namespaceName;
                }
                executeImport(importStep);  // Recursively import nested scripts
            }
        }
    }
}

void TaskInterpreter::executeWaitEvent(const json& step) {
    try {
        if (!step.contains("event") || !step["event"].is_string()) {
            THROW_INVALID_ARGUMENT(
                "WaitEvent step is missing a valid 'event' field.");
        }
        std::string eventName = step["event"];
        std::unique_lock lock(impl_->mtx_);
        impl_->cv_.wait(lock, [this, &eventName]() {
            return !impl_->eventQueue_.empty() &&
                   impl_->eventQueue_.front().first == eventName;
        });
        impl_->eventQueue_.pop();
    } catch (const std::exception& e) {
        LOG_F(ERROR, "Error during executeWaitEvent: {}", e.what());
        std::throw_with_nested(e);
    }
}

void TaskInterpreter::executePrint(const json& step) {
    std::string message = evaluate(step["message"]).get<std::string>();
    LOG_F(INFO, "{}", message);
}

void TaskInterpreter::executeAsync(const json& step) {
    impl_->threadPool_->enqueueDetach([this, step]() {
        size_t idx = 0;
        executeStep(step, idx, step);
    });
}

/*
{
    "type": "try",
    "try": [
        {"type": "print", "message": "Executing try block..."},
        {"type": "call", "function": "someFunction"}  // 假设此处可能抛出异常
    ],
    "catch": {
        "type": "all",
        "steps": [
            {"type": "print", "message": "Exception caught in catch block!"}
        ]
    },
    "finally": [
        {"type": "print", "message": "Finally block executed."}
    ]
}
 */
void TaskInterpreter::executeTryCatch(const json& step, size_t& idx,
                                      const json& script) {
    [[maybe_unused]] bool exceptionOccurred = false;
    if (!step.contains("try") || !step["try"].is_array()) {
        THROW_INVALID_ARGUMENT("TryCatch step is missing a valid 'try' field.");
    }
    try {
        executeSteps(step["try"], idx, script);
    } catch (const std::exception& e) {
        exceptionOccurred = true;
        LOG_F(ERROR, "Exception caught: {}", e.what());

        if (step.contains("catch")) {
            const auto& catchBlock = step["catch"];
            auto abiName =
                atom::meta::DemangleHelper::demangle(typeid(e).name());
            // 遍历 catch block, 匹配异常类型
            for (const auto& catchEntry : catchBlock) {
                std::string catchType =
                    catchEntry.contains("type")
                        ? catchEntry["type"].get<std::string>()
                        : "all";
                LOG_F(INFO, "Checking catch block for type: {} {}", catchType,
                      catchEntry.dump());

                // 检查异常类型是否匹配
                if (catchType == "all" || catchType == abiName) {
                    LOG_F(INFO, "Catch block step: {}", catchEntry.dump());
                    executeSteps(catchEntry["steps"], idx, script);
                    break;  // 执行匹配的 catch 分支后跳出
                }
            }
        }
    }

    // 执行 finally 块，无论是否发生异常
    if (step.contains("finally")) {
        const auto& finallyBlock = step["finally"];
        executeSteps(finallyBlock, idx, script);
    }

    if (!exceptionOccurred && step.contains("else")) {
        // 如果没有发生异常，执行 else 块
        const auto& elseBlock = step["else"];
        executeSteps(elseBlock, idx, script);
    }
}

void TaskInterpreter::executeThrow(const json& step) {
    if (!step.contains("exception_type") ||
        !step["exception_type"].is_string()) {
        THROW_INVALID_ARGUMENT(
            "Throw step requires an 'exception_type' field.");
    }

    std::string exceptionType = step["exception_type"];
    std::string message =
        step.contains("message") && step["message"].is_string()
            ? step["message"].get<std::string>()
            : "An error occurred";

    if (exceptionType == "runtime_error") {
        throw std::runtime_error(message);
    }
    if (exceptionType == "invalid_argument") {
        throw std::invalid_argument(message);
    }
    if (exceptionType == "out_of_range") {
        throw std::out_of_range(message);
    }
    THROW_RUNTIME_ERROR("Unsupported exception type: " + exceptionType);
}

// TODO: Switch to self implementation of CommandDispatcher
void TaskInterpreter::executeFunction(const json& step) {
    LOG_F(INFO, "Executing step {}", step.dump());
    std::string functionName = step["name"];
    json params = step.contains("params") ? step["params"] : json::object();
    // 用于处理返回值
    std::string targetVariable =
        step.contains("result") ? step["result"].get<std::string>() : "";
    std::shared_lock lock(impl_->mtx_);
    if (impl_->functions_.contains(functionName)) {
        json returnValue = impl_->functions_[functionName](params);
        // 如果指定了目标变量名，则将返回值存储到该变量中
        if (!targetVariable.empty()) {
            std::unique_lock ulock(impl_->mtx_);
            impl_->variables_[targetVariable] = returnValue;
        }
    } else {
        THROW_RUNTIME_ERROR("Function '" + functionName + "' not found.");
    }
}

void TaskInterpreter::executeReturn(const json& step, size_t& idx) {
    if (step.contains("value")) {
        impl_->variables_["__return_value__"] = {determineType(step["value"]),
                                                 evaluate(step["value"])};
    }
    idx = std::numeric_limits<size_t>::max();  // Terminate the script execution
}

void TaskInterpreter::executeBreak(const json& /*step*/, size_t& idx) {
    idx = std::numeric_limits<size_t>::max();  // Terminate the loop
}

void TaskInterpreter::executeContinue(const json& /*step*/, size_t& idx) {
    idx = std::numeric_limits<size_t>::max() - 1;  // Skip to the next iteration
}

void TaskInterpreter::executeSteps(const json& steps, size_t& idx,
                                   const json& script) {
    for (const auto& step : steps) {
        if (!executeStep(step, idx, script)) {
            break;
        }
    }
}

void TaskInterpreter::executeMessage(const json& step) {
    std::string message = evaluate(step["label"]).get<std::string>();
    LOG_F(INFO, "{}", message);
#if ENABLE_DEBUG
    std::cout << message << std::endl;
#endif
}

void TaskInterpreter::executeListenEvent(const json& step, size_t& idx) {
    LOG_F(INFO, "Listening for events: {}", step.dump());

    if (!step.contains("event_names") || !step["event_names"].is_array()) {
        THROW_INVALID_ARGUMENT("Listen event requires an 'event_names' array.");
    }

    std::vector<std::string> eventNames =
        step["event_names"].get<std::vector<std::string>>();
    std::string channel =
        step.contains("channel") && step["channel"].is_string()
            ? step["channel"].get<std::string>()
            : "default";
    int timeout = step.contains("timeout") && step["timeout"].is_number()
                      ? step["timeout"].get<int>()
                      : -1;

    std::unique_lock lock(impl_->mtx_);

    bool eventReceived = false;
    if (timeout < 0) {
        // 无超时等待事件发生
        impl_->cv_.wait(lock, [&]() {
            for (const auto& eventName : eventNames) {
                if (!impl_->eventQueue_.empty() &&
                    impl_->eventQueue_.front().first ==
                        eventName + "@" + channel) {
                    eventReceived = true;
                    return true;
                }
            }
            return false;
        });
    } else {
        // 带超时的等待
        impl_->cv_.wait_for(lock, std::chrono::milliseconds(timeout), [&]() {
            for (const auto& eventName : eventNames) {
                if (!impl_->eventQueue_.empty() &&
                    impl_->eventQueue_.front().first ==
                        eventName + "@" + channel) {
                    eventReceived = true;
                    return true;
                }
            }
            return false;
        });
    }

    if (!eventReceived) {
        LOG_F(INFO, "Timeout occurred while waiting for events on channel '{}'",
              channel);
        return;
    }

    auto eventData = impl_->eventQueue_.front().second;
    std::string receivedEvent = impl_->eventQueue_.front().first;

    // 事件数据过滤（如果适用）
    if (step.contains("filter")) {
        const json& filter = step["filter"];
        if (!evaluate(filter).get<bool>()) {
            impl_->eventQueue_.pop();
            return;  // 如果过滤条件不满足，跳过步骤
        }
    }

    // 根据接收到的特定事件执行步骤
    if (step.contains("event_steps") && step["event_steps"].is_object()) {
        std::string eventKey = receivedEvent.substr(0, receivedEvent.find('@'));
        if (step["event_steps"].contains(eventKey)) {
            executeSteps(step["event_steps"][eventKey], idx, step);
        } else if (step["event_steps"].contains("default")) {
            executeSteps(step["event_steps"]["default"], idx, step);
        }
    } else if (step.contains("steps")) {
        // 如果没有特定的事件处理逻辑，执行通用步骤
        executeSteps(step["steps"], idx, step);
    }

    impl_->eventQueue_.pop();
}

void TaskInterpreter::executeBroadcastEvent(const json& step) {
    LOG_F(INFO, "Broadcasting event: {}", step.dump());
    if (!step.contains("event_name") || !step["event_name"].is_string()) {
        THROW_INVALID_ARGUMENT("Broadcast event requires an 'event_name'.");
    }

    std::string eventName = step["event_name"];
    std::string channel =
        step.contains("channel") && step["channel"].is_string()
            ? step["channel"].get<std::string>()
            : "default";

    std::unique_lock lock(impl_->mtx_);
    impl_->eventQueue_.emplace(
        eventName + "@" + channel,
        step.contains("event_data") ? step["event_data"] : json());
    impl_->cv_.notify_all();
}

/*
{
    "type": "schedule",
    "delay": 3000,
    "parallel": true,
    "steps": [
        { "type": "print", "message": "This message is delayed by 3 seconds and
runs in parallel" }
    ]
}
*/
void TaskInterpreter::executeSchedule(const json& step, size_t& idx,
                                      const json& script) {
    if (!step.contains("delay") || !step["delay"].is_number_integer()) {
        THROW_INVALID_ARGUMENT(
            "Schedule step requires an integer 'delay' field.");
    }
    int delay = step["delay"];
    bool parallel = step.contains("parallel") && step["parallel"].is_boolean()
                        ? step["parallel"].get<bool>()
                        : false;

    if (parallel) {
        // Non-blocking parallel execution
        impl_->threadPool_->enqueueDetach(
            [this, step, idx, script, delay]() mutable {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                executeSteps(step["steps"], idx, script);
            });
    } else {
        // Blocking execution
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        executeSteps(step["steps"], idx, script);
    }
}

void TaskInterpreter::executeRetry(const json& step, size_t& idx,
                                   const json& script) {
    if (!step.contains("retries") || !step["retries"].is_number_integer()) {
        THROW_INVALID_ARGUMENT(
            "Retry step requires an integer 'retries' field.");
    }
    int retries = step["retries"];
    int delay = step.contains("delay") && step["delay"].is_number_integer()
                    ? step["delay"].get<int>()
                    : 0;
    bool exponentialBackoff = step.contains("exponential_backoff") &&
                                      step["exponential_backoff"].is_boolean()
                                  ? step["exponential_backoff"].get<bool>()
                                  : false;

    std::string retryOnErrorType =
        step.contains("error_type") && step["error_type"].is_string()
            ? step["error_type"].get<std::string>()
            : "";

    auto logError = [&](int attempt, const std::exception& e) {
        LOG_F(WARNING, "Retry {} failed, attempt {}/{}. Error: {}",
              step["type"].get<std::string>(), attempt + 1, retries, e.what());
        if (step.contains("on_retry")) {
            // Execute the on_retry steps before the next retry
            size_t retryIdx = 0;
            executeSteps(step["on_retry"], retryIdx, script);
        }
    };

    for (int i = 0; i <= retries; ++i) {
        try {
            executeSteps(step["steps"], idx, script);
            return;  // Break on success
        } catch (const std::exception& e) {
            // Check if we should retry based on the error type
            if (!retryOnErrorType.empty()) {
                try {
                    std::rethrow_if_nested(e);
                } catch (const std::system_error& se) {
                    if (se.code().category().name() != retryOnErrorType) {
                        throw;  // Rethrow if the error type doesn't match
                    }
                } catch (const std::exception&) {
                    if (typeid(e).name() != retryOnErrorType) {
                        throw;  // Rethrow if the error type doesn't match
                    }
                }
            }

            if (i == retries) {
                logError(i, e);
                throw;  // Rethrow if all retries failed
            }

            logError(i, e);

            if (delay > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }

            // Increase delay if exponential backoff is enabled
            if (exponentialBackoff) {
                delay *= 2;
            }
        }
    }
}

auto TaskInterpreter::evaluate(const json& value) -> json {
    if (value.is_string()) {
        std::string valStr = value.get<std::string>();

        // 检查是否是变量
        if (impl_->variables_.contains(valStr)) {
            std::shared_lock lock(impl_->mtx_);
            return impl_->variables_.at(valStr).second;
        }

        // 检查是否是表达式
        if (valStr.find_first_of("+-*/%^!&|<=>") != std::string::npos) {
            return evaluateExpression(valStr);  // 解析并评估表达式
        }

        // 检查是否是 $ 开头的运算符表达式
        if (valStr.starts_with("$")) {
            return evaluateExpression(
                valStr.substr(1));  // 去掉 $ 前缀后评估表达式
        }
    }

    if (value.is_number() || value.is_boolean()) {
        return value;
    }

    if (value.is_object()) {
        if (value.contains("$")) {
            if (value["$"].is_string()) {
                std::string expr = value["$"].get<std::string>();
                return evaluateExpression(expr);
            } else {
                THROW_RUNTIME_ERROR(
                    "Invalid format: '$' key must map to a string expression.");
            }
        }
        if (value.contains("$eq")) {  // 等于比较
            auto operands = value["$eq"];
            if (operands.is_array() && operands.size() == 2) {
                auto left = evaluate(operands[0]);
                auto right = evaluate(operands[1]);
#if ENABLE_DEBUG
                std::cout << "Left type: " << left.type_name()
                          << ", Right type: " << right.type_name() << std::endl;
                std::cout << "Evaluating equality comparison: " << left.dump()
                          << " == " << right.dump() << std::endl;
#endif
                LOG_F(INFO, "{} == {}", left.dump(), right.dump());
                if (determineType(left) != determineType(right)) {
                    THROW_RUNTIME_ERROR(
                        "Type mismatch in equality comparison: " +
                        value.dump());
                }
                return left == right;
            }
            THROW_RUNTIME_ERROR("Invalid equality comparison: " + value.dump());
        }
        if (value.contains("$gt")) {  // 大于比较
            auto operands = value["$gt"];
            if (operands.is_array() && operands.size() == 2) {
                auto left = evaluate(operands[0]);
                auto right = evaluate(operands[1]);
                return left > right;
            }
        } else if (value.contains("$lt")) {  // 小于比较
            auto operands = value["$lt"];
            if (operands.is_array() && operands.size() == 2) {
                auto left = evaluate(operands[0]);
                auto right = evaluate(operands[1]);
                return left < right;
            }
        } else if (value.contains("$gte")) {  // 大于等于比较
            auto operands = value["$gte"];
            if (operands.is_array() && operands.size() == 2) {
                auto left = evaluate(operands[0]);
                auto right = evaluate(operands[1]);
                return left >= right;
            }
        } else if (value.contains("$lte")) {  // 小于等于比较
            auto operands = value["$lte"];
            if (operands.is_array() && operands.size() == 2) {
                auto left = evaluate(operands[0]);
                auto right = evaluate(operands[1]);
                return left <= right;
            }
        } else if (value.contains("$ne")) {  // 不等于比较
            auto operands = value["$ne"];
            if (operands.is_array() && operands.size() == 2) {
                auto left = evaluate(operands[0]);
                auto right = evaluate(operands[1]);
                return left != right;
            }
        } else if (value.contains("$add")) {  // 加法运算
            auto operands = value["$add"];
            if (operands.is_array() && operands.size() == 2) {
                auto left = evaluate(operands[0]).get<int>();
                auto right = evaluate(operands[1]).get<int>();
                return left + right;
            }
        } else if (value.contains("$sub")) {  // 减法运算
            auto operands = value["$sub"];
            if (operands.is_array() && operands.size() == 2) {
                LOG_F(INFO, "{}", operands.dump());
                LOG_F(INFO, "{}", evaluate(operands[0]).dump());
                auto left = evaluate(operands[0]).get<int>();
                auto right = evaluate(operands[1]).get<int>();
                LOG_F(INFO, "{}", left - right);
                return left - right;
            }
        } else if (value.contains("$mul")) {  // 乘法运算
            auto operands = value["$mul"];
            if (operands.is_array() && operands.size() == 2) {
                auto left = evaluate(operands[0]).get<int>();
                auto right = evaluate(operands[1]).get<int>();
                return left * right;
            }
        } else if (value.contains("$div")) {  // 除法运算
            auto operands = value["$div"];
            if (operands.is_array() && operands.size() == 2) {
                auto left = evaluate(operands[0]).get<int>();
                auto right = evaluate(operands[1]).get<int>();
                if (right == 0) {
                    THROW_RUNTIME_ERROR("Division by zero");
                }
                return left / right;
            }
        } else if (value.contains("$and")) {  // 逻辑与
            auto operands = value["$and"];
            if (operands.is_array()) {
                for (const auto& operand : operands) {
                    if (!evaluate(operand).get<bool>()) {
                        return false;
                    }
                }
                return true;
            }
        } else if (value.contains("$or")) {  // 逻辑或
            auto operands = value["$or"];
            if (operands.is_array()) {
                for (const auto& operand : operands) {
                    if (evaluate(operand).get<bool>()) {
                        return true;
                    }
                }
                return false;
            }
        }
        // Conditional operation
        else if (value.contains("$if")) {
            const auto& cond = value["$if"];
            return evaluate(cond["condition"]).get<bool>()
                       ? evaluate(cond["then"])
                       : evaluate(cond["else"]);
        }
        // Custom function call
        else if (value.contains("$call")) {
            const auto& callInfo = value["$call"];
            std::string functionName = callInfo["function"];
            const json& params = callInfo["params"];
            return impl_->functions_[functionName](params);
        }
    }
    return value;
}

auto TaskInterpreter::evaluateExpression(const std::string& expr) -> json {
    std::istringstream iss(expr);
    std::stack<char> operators;
    std::stack<double> operands;
    std::string token;

    auto applyOperator = [](char op, double a, double b) -> double {
        switch (op) {
            case '+':
                return a + b;
            case '-':
                return a - b;
            case '*':
                return a * b;
            case '/':
                if (b != 0) {
                    return a / b;
                } else {
                    THROW_INVALID_ARGUMENT("Division by zero.");
                }
            case '%':
                if (b != 0) {
                    return std::fmod(a, b);
                } else {
                    THROW_INVALID_ARGUMENT("Modulo by zero.");
                }
            case '^':
                return std::pow(a, b);
            case '<':
                return static_cast<double>(a < b);
            case '>':
                return static_cast<double>(a > b);
            case '=':
                return static_cast<double>(a == b);
            case '!':
                return static_cast<double>(a != b);
            case '&':
                return static_cast<double>(static_cast<bool>(a) &&
                                           static_cast<bool>(b));
            case '|':
                return static_cast<double>(static_cast<bool>(a) ||
                                           static_cast<bool>(b));
            default:
                THROW_RUNTIME_ERROR("Unknown operator.");
        }
    };

    while (iss >> token) {
        // 处理数字和变量
        if ((std::isdigit(token[0]) != 0) || token[0] == '.') {
            operands.push(std::stod(token));
        } else if (impl_->variables_.contains(token)) {
            operands.push(impl_->variables_.at(token).second.get<double>());
        } else if (token == "+" || token == "-" || token == "*" ||
                   token == "/" || token == "%" || token == "^" ||
                   token == "<" || token == ">" || token == "==" ||
                   token == "!=" || token == "&&" || token == "||") {
            // 处理操作符
            while (!operators.empty() &&
                   precedence(operators.top()) >= precedence(token[0])) {
                double b = operands.top();
                operands.pop();
                double a = operands.top();
                operands.pop();
                char op = operators.top();
                operators.pop();
                operands.push(applyOperator(op, a, b));
            }
            operators.push(token[0]);
        } else if (token == "(") {
            operators.push('(');
        } else if (token == ")") {
            while (!operators.empty() && operators.top() != '(') {
                double b = operands.top();
                operands.pop();
                double a = operands.top();
                operands.pop();
                char op = operators.top();
                operators.pop();
                operands.push(applyOperator(op, a, b));
            }
            if (operators.empty() || operators.top() != '(') {
                THROW_INVALID_ARGUMENT("Mismatched parentheses.");
            }
            operators.pop();  // 移除左括号
        } else {
            THROW_INVALID_ARGUMENT("Invalid token in expression: " + token);
        }
    }

    // 处理剩余操作符
    while (!operators.empty()) {
        double b = operands.top();
        operands.pop();
        double a = operands.top();
        operands.pop();
        char op = operators.top();
        operators.pop();
        operands.push(applyOperator(op, a, b));
    }

    if (operands.size() != 1) {
        THROW_INVALID_ARGUMENT("Invalid expression: " + expr);
    }

    return operands.top();
}

auto TaskInterpreter::precedence(char op) -> int {
    switch (op) {
        case '+':
        case '-':
            return 1;
        case '*':
        case '/':
        case '%':
            return 2;
        case '^':
            return 3;
        case '<':
        case '>':
        case '=':
        case '!':
            return 4;
        case '&':
            return 5;
        case '|':
            return 6;
        default:
            return 0;
    }
}

void TaskInterpreter::registerCustomError(const std::string& name,
                                          const std::error_code& errorCode) {
    std::unique_lock lock(impl_->mtx_);
    impl_->customErrors_[name] = errorCode;
}

void TaskInterpreter::throwCustomError(const std::string& name) {
    std::shared_lock lock(impl_->mtx_);
    if (impl_->customErrors_.contains(name)) {
        throw std::system_error(impl_->customErrors_.at(name));
    }
    THROW_RUNTIME_ERROR("Custom error '" + name + "' not found.");
}

void TaskInterpreter::handleException(const std::string& scriptName,
                                      const std::exception& e) {
    std::shared_lock lock(impl_->mtx_);
    if (impl_->exceptionHandlers_.contains(scriptName)) {
        impl_->exceptionHandlers_.at(scriptName)(e);
    } else {
        LOG_F(ERROR, "Unhandled exception in script '{}': {}", scriptName,
              e.what());
        std::rethrow_if_nested(e);
    }
}

}  // namespace lithium
