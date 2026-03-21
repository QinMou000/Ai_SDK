#pragma once
#include <sqlite3.h>

#include <memory>
#include <mutex>
#include <string>

#include "common.h"

namespace Ai_Chat_SDK {
class DataManager {
   public:
    DataManager(const std::string& dbName);
    ~DataManager();
    // 对session 操作
    // 插入一个会话
    bool insertNewSession(const Session& session);
    // 获取指定会话
    std::shared_ptr<Session> getSession(const std::string& SessionId) const;
    // 更新指定会话的时间戳
    bool updateSessionTimeStamp(const std::string& sessionId, std::time_t timeStamp);
    // 删除指定会话 同时删除所有消息
    bool deleteSession(const std::string& SessionId);
    // 列出所有会话id
    std::vector<std::string> listAllSessionIds();
    // 列出所有会话
    std::vector<std::shared_ptr<Session>> listAllSessions();
    // 获取会话总数
    int getSessionCount() const;
    // 清空所有会话 同时删除所有消息
    bool clearAllSessions();
    /************************************************************************************************/
    // 对message 操作
    // 插入新消息 需要更新会话时间戳
    bool insertNewMessage(const std::string& sessionId, Message& message);
    // 获取指定回话的历史消息
    std::vector<Message> getSessionMessages(const std::string& sessionId) const;
    // 删除指定回话的所有消息
    bool deleteSessionMessages(const std::string& SessionId);

   private:
    bool initDataBase();
    bool executeSQL(const std::string& SQL);
    // 无锁版本的获取消息，供内部调用，避免死锁
    std::vector<Message> getSessionMessagesNoLock(const std::string& sessionId) const;

   private:
    sqlite3* _db = nullptr;
    std::string _dbName;
    mutable std::mutex _mutex;
};

}  // namespace Ai_Chat_SDK