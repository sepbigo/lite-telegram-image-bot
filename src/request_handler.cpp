#include "request_handler.h"
#include "http_client.h"
#include "utils.h"
#include "config.h"
#include "db_manager.h"
#include <nlohmann/json.hpp>
#include <regex>
#include <curl/curl.h>
#include <algorithm>
#include <future>
#include <sstream>

std::string getMimeType(const std::string& filePath, const std::map<std::string, std::string>& mimeTypes, const std::string& defaultMimeType = "application/octet-stream") {
    try {
        // 查找文件扩展名
        size_t pos = filePath.find_last_of(".");
        std::string extension;
        
        if (pos != std::string::npos && pos != filePath.length() - 1) {
            extension = filePath.substr(pos);
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        }
        if (extension.empty() || extension == "bin") {
            if (filePath.find("photo") != std::string::npos) {
                return "image/jpeg";
            }
            if (filePath.find("video") != std::string::npos) {
                return "video/mp4";
            }
        }
        auto it = mimeTypes.find(extension);
        if (it != mimeTypes.end()) {
            return it->second;
        } else {
            return defaultMimeType;
        }
    } catch (const std::exception& e) {
        return defaultMimeType;
    }
}

std::string getFileExtension(const std::string& filePath) {
    std::size_t pos = filePath.find_last_of(".");
    if (pos != std::string::npos) {
        return filePath.substr(pos);
    }
    return ""; // No extension found
}

// 处理流式传输的回调函数，支持分段传输
size_t streamWriteCallback(void* ptr, size_t size, size_t nmemb, httplib::Response* res) {
    size_t totalSize = size * nmemb;
    if (totalSize > 0) {
        res->body.append(static_cast<char*>(ptr), totalSize);

        // 增加内存控制，防止过度积累
        if (res->body.size() > 102400) { // 超过100KB时清空body
            res->body.clear();  // 清理body内容，防止内存占用过多
        }
    }
    return totalSize;
}

// 处理视频文件的流式传输，支持分段请求
void handleStreamRequest(const httplib::Request& req, httplib::Response& res, const std::string& fileDownloadUrl, const std::string& mimeType) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        res.status = 500;
        res.set_content("Failed to initialize CURL", "text/plain");
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, fileDownloadUrl.c_str());

    // 启用 HTTP Keep-Alive
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 120L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);

    // 增加缓冲区大小
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 102400L);  // 增加缓冲区至100KB

    // 设置请求超时
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    // 设置回调函数，流式传输数据
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res);

    // 处理分段请求（Range 请求）
    if (req.has_header("Range")) {
        std::string rangeHeader = req.get_header_value("Range");
        curl_easy_setopt(curl, CURLOPT_RANGE, rangeHeader.c_str());
    }

    // 执行请求
    CURLcode res_code = curl_easy_perform(curl);
    
    if (res_code != CURLE_OK) {
        log(LogLevel::LOGERROR, "CURL error: " + std::string(curl_easy_strerror(res_code)));
        res.status = 500;
        res.set_content("Failed to stream file", "text/plain");
    }

    // 获取文件总大小并设置头信息
    curl_off_t contentLength = 0;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &contentLength);

    std::ostringstream contentLengthStr;
    contentLengthStr << static_cast<int64_t>(contentLength);
    res.set_header("Content-Length", contentLengthStr.str());
    
    // 设置视频文件类型和支持分段下载
    res.set_header("Content-Type", mimeType);
    res.set_header("Accept-Ranges", "bytes");  // 支持分段下载

    curl_easy_cleanup(curl);
}

void handleImageRequest(const httplib::Request& req, httplib::Response& res, const std::string& apiToken, const std::map<std::string, std::string>& mimeTypes, ImageCacheManager& cacheManager, CacheManager& memoryCache, const std::string& telegramApiUrl, const Config& config, DBManager& dbManager) {
    log(LogLevel::INFO, "Received request for image.");
    if (req.matches.size() < 2) {
        res.status = 400;
        res.set_content("Bad Request", "text/plain");
        log(LogLevel::LOGERROR, "Bad request: URL does not match expected format.");
        return;
    }

    std::string shortId = req.matches[1];
    std::string fileId = (shortId.length() > 6) ? shortId : dbManager.getFileIdByShortId(shortId);

    // 验证 fileId 的合法性
    std::regex fileIdRegex("^[A-Za-z0-9_-]+$");
    if (!std::regex_match(fileId, fileIdRegex)) {
        res.status = 400;
        res.set_content("Invalid File ID", "text/plain");
        log(LogLevel::LOGERROR, "Invalid file ID received: " + fileId);
        return;
    }

    log(LogLevel::INFO, "Checking file path from memory cache for file ID: " + fileId);

    // Step 1: 从 memoryCache 中获取 filePath 是否存在
    std::string cachedFilePath;
    bool isMemoryCacheHit = memoryCache.getFilePathCache(fileId, cachedFilePath);

    // 获取文件的扩展名，默认为空字符串
    std::string preferredExtension = (req.has_header("Accept") && req.get_header_value("Accept").find("image/webp") != std::string::npos) ? "webp" : getFileExtension(cachedFilePath);

    // 如果 memory 缓存命中，检查 image 缓存（磁盘）是否命中
    if (isMemoryCacheHit) {
        log(LogLevel::INFO, "Memory cache hit for file ID: " + fileId + ". Checking image cache.");
        std::string cachedImageData = cacheManager.getCachedImage(fileId, preferredExtension);

        if (!cachedImageData.empty()) {
            log(LogLevel::INFO, "Image cache hit for file ID: " + fileId);
            // 获取文件的 MIME 类型
            std::string mimeType = getMimeType(cachedFilePath, mimeTypes);
            // 返回缓存的文件数据
            setHttpResponse(res, cachedImageData, mimeType, req);
            return;
        } else {
            log(LogLevel::INFO, "Image cache miss for file ID: " + fileId + ". Downloading from Telegram.");
        }
    } else {
        log(LogLevel::INFO, "Memory cache miss. Requesting file information from Telegram for file ID: " + fileId);

        // 如果 memoryCache 中没有 filePath，调用 getFile 接口获取文件路径
        std::string telegramFileUrl = telegramApiUrl + "/bot" + apiToken + "/getFile?file_id=" + fileId;
        std::string fileResponse = sendHttpRequest(telegramFileUrl);

        if (fileResponse.empty()) {
            res.status = 500;
            res.set_content("Failed to get file information from Telegram", "text/plain");
            log(LogLevel::LOGERROR, "Failed to retrieve file information from Telegram.");
            return;
        }

        nlohmann::json jsonResponse = nlohmann::json::parse(fileResponse);
        if (jsonResponse.contains("result") && jsonResponse["result"].contains("file_path")) {
            cachedFilePath = jsonResponse["result"]["file_path"];
            log(LogLevel::INFO, "Retrieved file path: " + cachedFilePath);

            // 将 filePath 存入 memoryCache
            memoryCache.addFilePathCache(fileId, cachedFilePath, 3600);
        } else {
            res.status = 404;
            res.set_content("File Not Found", "text/plain");
            log(LogLevel::LOGERROR, "File not found in Telegram for ID: " + fileId);
            return;
        }
    }

    // 从 Telegram 下载文件
    std::string telegramFileDownloadUrl = telegramApiUrl + "/file/bot" + apiToken + "/" + cachedFilePath;
    std::string fileData = sendHttpRequest(telegramFileDownloadUrl);

    if (fileData.empty()) {
        res.status = 500;
        res.set_content("Failed to download file from Telegram", "text/plain");
        log(LogLevel::LOGERROR, "Failed to download file from Telegram for file path: " + cachedFilePath);
        return;
    }

    // 异步将文件内容缓存到磁盘
    auto future = std::async(std::launch::async, [&cacheManager, fileId, fileData, preferredExtension]() {
        cacheManager.cacheImage(fileId, fileData, preferredExtension);
    });

    future.get();

    // 返回文件，添加 HTTP 缓存控制和 Gzip 支持
    std::string mimeType = getMimeType(cachedFilePath, mimeTypes);
    setHttpResponse(res, fileData, mimeType, req);
    log(LogLevel::INFO, "Successfully served and cached file for file ID: " + fileId);
}


void setHttpResponse(httplib::Response& res, const std::string& fileData, const std::string& mimeType, const httplib::Request& req) {
    // 添加 HTTP 缓存控制头
    res.set_header("Cache-Control", "max-age=3600");

    // 对小文件启用压缩以节省内存占用
    if (fileData.size() < 1048576 && req.has_header("Accept-Encoding") && req.get_header_value("Accept-Encoding").find("gzip") != std::string::npos) {
        std::string compressedData = gzipCompress(fileData);
        res.set_content(compressedData, mimeType);
        res.set_header("Content-Encoding", "gzip");
    } else {
        res.set_content(fileData, mimeType);
    }
}

std::string getBaseUrl(const std::string& url) {
    std::regex urlRegex(R"((https?:\/\/[^\/:]+(:\d+)?))");
    std::smatch match;
    if (std::regex_search(url, match, urlRegex)) {
        return match.str(0);
    }
    return "";
}