/*!
 * \file type_caster.hpp
 * \brief Auto type caster, for better dispatch
 * \author Max Qian <lightapt.com>
 * \date 2023-04-05
 * \copyright Copyright (C) 2023-2024 Max Qian <lightapt.com>
 */

#ifndef ATOM_META_TYPE_CASTER_HPP
#define ATOM_META_TYPE_CASTER_HPP

#include <any>
#include <functional>
#include <memory>
#include <queue>
#include <typeinfo>
#include <vector>

#if ENABLE_FASTHASH
#include "emhash/hash_set8.hpp"
#include "emhash/hash_table8.hpp"
#else
#include <unordered_map>
#include <unordered_set>
#endif

#include "atom/error/exception.hpp"
#include "type_info.hpp"

namespace atom::meta {

class TypeCaster {
public:
    using ConvertFunc = std::function<std::any(const std::any&)>;
    using ConvertMap = std::unordered_map<TypeInfo, ConvertFunc>;

    TypeCaster() { registerBuiltinTypes(); }

    static auto createShared() -> std::shared_ptr<TypeCaster> {
        return std::make_shared<TypeCaster>();
    }

    template <typename SourceType, typename DestinationType>
    void registerConversion(ConvertFunc func) {
        auto srcInfo = userType<SourceType>();
        auto destInfo = userType<DestinationType>();
        registerType<SourceType>(srcInfo.bareName());
        registerType<DestinationType>(destInfo.bareName());
        if (srcInfo == destInfo) {
            THROW_INVALID_ARGUMENT(
                "Source and destination types must be different.");
        }
        conversions_[srcInfo][destInfo] = std::move(func);
        clearCache();
    }

    template <typename SourceType, typename DestinationType>
    auto hasConversion() const -> bool {
        auto srcInfo = userType<SourceType>();
        auto destInfo = userType<DestinationType>();
        return hasConversion(srcInfo, destInfo);
    }

    auto hasConversion(TypeInfo src, TypeInfo dst) const -> bool {
        return conversions_.find(src) != conversions_.end() &&
               conversions_.at(src).find(dst) != conversions_.at(src).end();
    }

    auto convert(const std::vector<std::any>& input,
                 const std::vector<std::string>& target_type_names) const
        -> std::vector<std::any> {
        if (input.size() != target_type_names.size()) {
            THROW_INVALID_ARGUMENT(
                "Input and target type names must be of the same length.");
        }

        std::vector<std::any> output;
        for (size_t i = 0; i < input.size(); ++i) {
            auto destInfo = userTypeByName(target_type_names[i]);
            auto srcInfo = getTypeInfo(input[i].type().name());
            if (!srcInfo.has_value()) {
                THROW_INVALID_ARGUMENT("Type " + target_type_names[i] +
                                       " not found.");
            }

            if (srcInfo.value() == destInfo) {
                output.push_back(input[i]);
                continue;
            }

            auto path = findConversionPath(srcInfo.value(), destInfo);
            std::any result = input[i];
#pragma unroll
            for (size_t j = 0; j < path.size() - 1; ++j) {
                result = conversions_.at(path[j]).at(path[j + 1])(result);
            }
            output.push_back(result);
        }
        return output;
    }

    auto getRegisteredTypes() const -> std::vector<std::string> {
        std::vector<std::string> typeNames;
        typeNames.reserve(type_name_map_.size());
#pragma unroll
        for (const auto& [name, info] : type_name_map_) {
            typeNames.push_back(name);
        }
        return typeNames;
    }

    template <typename T>
    void registerType(const std::string& name) {
        type_name_map_[name] = userType<T>();
        type_name_map_[typeid(T).name()] = userType<T>();
        detail::getTypeRegistry()[typeid(T).name()] = userType<T>();
    }

    template <typename EnumType>
    void registerEnumValue(const std::string& enum_name,
                           const std::string& string_value,
                           EnumType enum_value) {
        if (!m_enumMaps_.contains(enum_name)) {
            m_enumMaps_[enum_name] =
                std::unordered_map<std::string, EnumType>();
        }

        auto& enumMap =
            std::any_cast<std::unordered_map<std::string, EnumType>&>(
                m_enumMaps_[enum_name]);

        enumMap[string_value] = enum_value;
    }

    template <typename EnumType>
    auto getEnumMap(const std::string& enum_name) const
        -> const std::unordered_map<std::string, EnumType>& {
        return std::any_cast<const std::unordered_map<std::string, EnumType>&>(
            m_enumMaps_.at(enum_name));
    }

    template <typename EnumType>
    auto enumToString(EnumType value,
                      const std::string& enum_name) -> std::string {
        const auto& enumMap = getEnumMap<EnumType>(enum_name);
#pragma unroll
        for (const auto& [key, enumValue] : enumMap) {
            if (enumValue == value) {
                return key;
            }
        }
        THROW_INVALID_ARGUMENT("Invalid enum value");
    }

    template <typename EnumType>
    auto stringToEnum(const std::string& string_value,
                      const std::string& enum_name) -> EnumType {
        const auto& enumMap = getEnumMap<EnumType>(enum_name);
        auto iterator = enumMap.find(string_value);
        if (iterator != enumMap.end()) {
            return iterator->second;
        }
        THROW_INVALID_ARGUMENT("Invalid enum string");
    }

private:
    std::unordered_map<TypeInfo, ConvertMap> conversions_;
    mutable std::unordered_map<std::string, std::vector<TypeInfo>>
        conversion_paths_cache_;
    std::unordered_map<std::string, TypeInfo> type_name_map_;
    std::unordered_map<std::string, std::any> m_enumMaps_;

    void registerBuiltinTypes() {
        registerType<int>("int");
        registerType<double>("double");
        registerType<std::string>("std::string");
    }

    static auto makeCacheKey(TypeInfo src, TypeInfo dst) -> std::string {
        return src.bareName() + "->" + dst.bareName();
    }

    void clearCache() { conversion_paths_cache_.clear(); }

    auto findConversionPath(TypeInfo src,
                            TypeInfo dst) const -> std::vector<TypeInfo> {
        std::string cacheKey = makeCacheKey(src, dst);
        if (conversion_paths_cache_.find(cacheKey) !=
            conversion_paths_cache_.end()) {
            return conversion_paths_cache_.at(cacheKey);
        }

        std::queue<std::vector<TypeInfo>> paths;
        paths.push({src});

        std::unordered_set<TypeInfo> visited;
        visited.insert(src);

        while (!paths.empty()) {
            auto currentPath = paths.front();
            paths.pop();
            auto last = currentPath.back();

            if (last == dst) {
                conversion_paths_cache_[cacheKey] = currentPath;
                return currentPath;
            }

            auto findIt = conversions_.find(last);
            if (findIt != conversions_.end()) {
#pragma unroll
                for (const auto& [next_type, _] : findIt->second) {
                    if (visited.insert(next_type).second) {
                        auto newPath = currentPath;
                        newPath.push_back(next_type);
                        paths.push(std::move(newPath));
                    }
                }
            }
        }

        THROW_RUNTIME_ERROR("No conversion path found for these types.");
    }

    auto userTypeByName(const std::string& name) const -> TypeInfo {
        auto findIt = type_name_map_.find(name);
        if (findIt != type_name_map_.end()) {
            return findIt->second;
        }
        THROW_INVALID_ARGUMENT("Unknown type name: " + name);
    }

    static auto getTypeInfo(const std::string& name)
        -> std::optional<TypeInfo> {
        auto& registry = detail::getTypeRegistry();
        if (auto iterator = registry.find(name); iterator != registry.end()) {
            return iterator->second;
        }
        return std::nullopt;
    }
};

}  // namespace atom::meta

#endif  // ATOM_META_TYPE_CASTER_HPP