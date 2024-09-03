#ifndef BOT_H
#define BOT_H

#include <string>
#include <nlohmann/json.hpp>

class Bot {
public:
    Bot(const std::string& token);

    void processUpdate(const nlohmann::json& update);
    std::string getFileUrl(const std::string& fileId);
    void sendMessage(const std::string& chatId, const std::string& message);
    void handleWebhook(const nlohmann::json& webhookRequest);
    int getSavedOffset();
    void saveOffset(int offset);
    std::string getApiToken() const {
        return apiToken;
    }

private:
    std::string apiToken;
    std::string getOffsetFile();
};

#endif
