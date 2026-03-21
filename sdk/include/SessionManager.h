#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "common.h"
#include "DataManager.h"

namespace Ai_Chat_SDK {
class SessionManager {
   public:
    SessionManager(const std::string& dbName = "chat.db");
    // 创建会话，提供模型名称
    std::string createSession(const std::string& modelName);
    // 通过会话ID获取会话信息
    std::shared_ptr<Session> getSession(const std::string& sessionId);
    // 往某个会话中添加消息
    bool addMessage(const std::string& sessionId, const Message& message);
    // 获取某个会话的所有历史消息
    std::vector<Message> getHistroyMessages(const std::string& sessionId) const;
    // 更新会话时间戳
    void updateSessionTimestamp(const std::string& sessionId);
    // 获取会话所有会话列表 会话id
    std::vector<std::string> getSessionLists() const;
    // 删除某个会话
    bool deleteSession(const std::string& sessionId);
    // 清空所有会话
    void clearAllSessions();
    // 获取会话总数
    size_t getSessionCount() const;

   private:
    // 生成回话id session_时间戳_会话计数
    std::string generaterSessionId();
    // 生成消息id msg_会话id_时间戳_会话计数
    std::string generaterMessageId(const std::string& sessionId, size_t messageCounter);

   private:
    std::unordered_map<std::string, std::shared_ptr<Session> > _session;  // key: 会话ID value: 会话信息
    mutable std::mutex _mutex;                                            // mutable 作用是让该变量在 const 成员函数或 const 对象中仍可被修改
    std::atomic<int64_t> _sessionCounter = {0};
    DataManager _dataManager;
};

}  // namespace Ai_Chat_SDK