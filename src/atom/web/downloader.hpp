/*
 * downloader.hpp
 *
 * Copyright (C) 2023-2024 Max Qian <lightapt.com>
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace atom::web {
/**
 * @brief DownloadManager 类，用于管理下载任务，使用 Pimpl 模式隐藏实现细节
 */
class DownloadManager {
public:
    /**
     * @brief 构造函数
     * @param task_file 保存下载任务列表的文件路径
     */
    explicit DownloadManager(const std::string& task_file);

    /**
     * @brief 析构函数，用于释放资源
     */
    ~DownloadManager();

    /**
     * @brief 添加下载任务
     * @param url 下载链接
     * @param filepath 本地保存文件路径
     * @param priority 下载任务优先级，数字越大优先级越高
     */
    void add_task(const std::string& url, const std::string& filepath,
                  int priority = 0);

    /**
     * @brief 删除下载任务
     * @param index 要删除的任务在任务列表中的索引
     * @return 是否成功删除任务
     */
    bool remove_task(size_t index);

    /**
     * @brief 开始下载任务
     * @param thread_count 下载线程数，默认为 CPU 核心数
     * @param download_speed 下载速度限制，单位为字节/秒，0 表示不限制下载速度
     */
    void start(size_t thread_count = std::thread::hardware_concurrency(),
               size_t download_speed = 0);

    /**
     * @brief 暂停下载任务
     * @param index 要暂停的任务在任务列表中的索引
     */
    void pause_task(size_t index);

    /**
     * @brief 恢复下载任务
     * @param index 要恢复的任务在任务列表中的索引
     */
    void resume_task(size_t index);

    /**
     * @brief 获取已下载的字节数
     * @param index 下载任务在任务列表中的索引
     * @return 已下载的字节数
     */
    size_t get_downloaded_bytes(size_t index);

    /**
     * @brief 取消下载任务
     * @param index 要取消的任务在任务列表中的索引
     */
    void cancel_task(size_t index);

    /**
     * @brief 动态调整下载线程数
     * @param thread_count 新的下载线程数
     */
    void set_thread_count(size_t thread_count);

    /**
     * @brief 设置下载错误重试次数
     * @param retries 每个任务失败时的最大重试次数
     */
    void set_max_retries(size_t retries);

    /**
     * @brief 注册下载完成回调函数
     * @param callback 下载完成时的回调函数，参数为任务索引
     */
    void on_download_complete(const std::function<void(size_t)>& callback);

    /**
     * @brief 注册下载进度更新回调函数
     * @param callback 下载进度更新时的回调函数，参数为任务索引和下载百分比
     */
    void on_progress_update(
        const std::function<void(size_t, double)>& callback);

    class Impl;
private:
    
    std::unique_ptr<Impl> impl_;  ///< 使用 Pimpl 隐藏实现细节
};

}  // namespace atom::web
