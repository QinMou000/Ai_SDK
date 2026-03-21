#include "../include/SessionManager.h"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "../include/util/Log.h"

namespace Ai_Chat_SDK {

SessionManager::SessionManager(const std::string& dbName) : _dataManager(dbName) {
    auto sessions = _dataManager.listAllSessions();
    for(auto& session : sessions) {
        _session[session->_sessionId] = session;
    }
}

// 生成回话id session_时间戳_会话计数
std::string SessionManager::generaterSessionId() {
    // 会话计数++
    _sessionCounter.fetch_add(1);
    std::time_t time = std::time(nullptr);

    std::ostringstream os;
    os << "session_" << std::to_string(time) << "_" << std::to_string(_sessionCounter);
    return os.str();
}

// 生成消息id msg_会话id_时间戳_会话计数
std::string SessionManager::generaterMessageId(const std::string& sessionId, size_t messageCounter) {
    std::time_t time = std::time(nullptr);

    std::ostringstream os;
    os << "msg_" << sessionId << "_" << std::to_string(time) << "_" << std::to_string(++messageCounter);
    return os.str();
}

// 创建会话，提供模型名称
std::string SessionManager::createSession(const std::string& modelName) {
    std::shared_ptr<Session> session;
    std::string sessionId;
    {
        // 对临界区加锁
        std::lock_guard<std::mutex> lock(_mutex);
        // 生成回话id
        sessionId = generaterSessionId();
        // 根据模型 初始化回话
        session = std::make_shared<Session>(modelName);
        session->_sessionId = sessionId;
        session->_createAt = session->_updateAt = std::time(nullptr);
        // 将回话加入map
        _session[sessionId] = session;
    }
    //  同步到数据库 (数据库操作不加锁，避免长时间阻塞其他内存操作)
    _dataManager.insertNewSession(*session);
    return sessionId;
}
// 通过会话ID获取会话信息
std::shared_ptr<Session> SessionManager::getSession(const std::string& sessionId) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _session.find(sessionId);
        if(it != _session.end()) {
            if(it->second->_messages.empty()) {
                // 如果内存里的 messages 为空，尝试从数据库加载一次
                it->second->_messages = _dataManager.getSessionMessages(sessionId);
            }
            return it->second;
        }
    }
    // 内存里面没有 去数据库找
    auto session = _dataManager.getSession(sessionId);
    if(session) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _session.find(sessionId);
        if(it == _session.end()) {
            // 内存里面没找到 添加到内存
            _session[sessionId] = session;
        } else {
            session = it->second;
        }
        return session;
    }
    WARN("SessionManager::getSession session id:{} is not exsist", sessionId);
    return nullptr;
}
// 往某个会话中添加消息
bool SessionManager::addMessage(const std::string& sessionId, const Message& message) {
    Message msg(message._role, message._content);
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _session.find(sessionId);
        if(it == _session.end()) {
            WARN("SessionManager::addMessage session id:{} is not exsist", sessionId);
            return false;
        }
        // 生成消息id
        msg._messageId = generaterMessageId(sessionId, it->second->_messages.size());
        msg._timestamp = std::time(nullptr);
        // 更新回话
        it->second->_messages.push_back(msg);
        it->second->_updateAt = std::time(nullptr);
        INFO("SessionManager::addMessage addMessage successed sessionId:{} msgContent:{}", it->second->_sessionId, msg._content);
    }

    // 将新消息保存到数据库
    _dataManager.insertNewMessage(sessionId, msg);

    return true;
}
// 获取某个会话的所有历史消息
std::vector<Message> SessionManager::getHistroyMessages(const std::string& sessionId) const {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _session.find(sessionId);
        if(it != _session.end()) {
            if(it->second->_messages.empty()) {
                // 如果内存里的 messages 为空，尝试从数据库加载一次
                it->second->_messages = _dataManager.getSessionMessages(sessionId);
            }
            return it->second->_messages;
        }
    }
    WARN("SessionManager::getHistroyMessages session id:{} is not exsist", sessionId);
    // 如果内存里面没有 去数据库查
    return _dataManager.getSessionMessages(sessionId);
}
// 更新会话时间戳
void SessionManager::updateSessionTimestamp(const std::string& sessionId) {
    std::time_t updateTime;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _session.find(sessionId);
        if(it != _session.end()) {
            it->second->_updateAt = std::time(nullptr);
            updateTime = it->second->_updateAt;
            found = true;
        }
    }
    // 只有找到了才更新数据库中的回话时间戳，避免解引用越界迭代器
    if(found) {
        _dataManager.updateSessionTimeStamp(sessionId, updateTime);
    }
}
// 获取会话所有会话列表 会话id
std::vector<std::string> SessionManager::getSessionLists() const {
    std::vector<std::pair<std::string, std::time_t>> tmp;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if(_session.empty()) {
            WARN("session lists is empty");
        } else {
            tmp.reserve(_session.size());  // 提前开好空间
            // 将内存中的数据放到tmp中等待排序
            for(auto& pair : _session) {
                tmp.push_back({pair.first, pair.second->_updateAt});
            }
        }
    }

    // 将数据库中的数据放到tmp中等待排序
    // 将数据库中的会话添加到临时列表中
    auto sessions = _dataManager.listAllSessions();

    {
        std::lock_guard<std::mutex> lock(_mutex);
        for(const auto& session : sessions) {
            if(_session.find(session->_sessionId) == _session.end()) {
                // 内存里面没有才添加
                tmp.push_back({session->_sessionId, session->_updateAt});
            }
        }
    }

    std::sort(tmp.begin(), tmp.end(),
              [](const std::pair<std::string, std::time_t>& x, const std::pair<std::string, std::time_t>& y) { return x.second > y.second; });
    std::vector<std::string> sessionIds;
    for(auto& pair : tmp) {
        sessionIds.push_back(pair.first);
    }
    return sessionIds;
}
// 删除某个会话
bool SessionManager::deleteSession(const std::string& sessionId) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _session.find(sessionId);
        if(it == _session.end()) {
            WARN("SessionManager::deleteSession session id:{} is not exsist", sessionId);
            return false;
        }
        _session.erase(it);
    }
    // 从数据库中删除
    // bool deleteSession(const std::string& SessionId);
    _dataManager.deleteSession(sessionId);
    return true;
}

// 清空所有会话
void SessionManager::clearAllSessions() {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _session.clear();
    }

    // 从数据库中清除所有回话
    _dataManager.clearAllSessions();
}
// 获取会话总数
size_t SessionManager::getSessionCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _session.size();
    // return _sessionCounter; ????// TODO
}

}  // namespace Ai_Chat_SDK