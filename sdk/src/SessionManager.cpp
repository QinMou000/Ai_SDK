#include "../include/SessionManager.h"

#include <cstddef>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "../include/util/Log.h"

namespace Ai_Chat_SDK {

SessionManager::SessionManager(const std::string& dbName) {}

// 生成回话id session_时间戳_会话计数
std::string SessionManager::generaterSessionId() {
    // 会话计数++
    _sessionCounter.fetch_add(1);
    std::time_t time = std::time(nullptr);

    std::ostringstream os;
    os << "session_" << std::to_string(time) << "_" << std::to_string(_sessionCounter);
    return os.str();
}

// 生成消息id msg_时间戳_会话计数
std::string SessionManager::generaterMessageId(size_t messageCounter) {
    std::time_t time = std::time(nullptr);

    std::ostringstream os;
    os << "msg_" << std::to_string(time) << "_" << std::to_string(++messageCounter);
    return os.str();
}

// 创建会话，提供模型名称
std::string SessionManager::createSession(const std::string& modelName) {
    // 对临界区加锁
    _mutex.lock();
    // 生成回话id
    std::string sessionId = generaterSessionId();
    // 根据模型 初始化回话
    auto session = std::make_shared<Session>(modelName);
    session->_sessionId = sessionId;
    session->_createAt = session->_updateAt = std::time(nullptr);
    // 将回话加入map
    _session[sessionId] = session;
    _mutex.unlock();
    // TODO 同步到数据库

    return sessionId;
}
// 通过会话ID获取会话信息
std::shared_ptr<Session> SessionManager::getSession(const std::string& sessionId) {
    _mutex.lock();
    auto it = _session.find(sessionId);
    if(it != _session.end()) {
        _mutex.unlock();
        // 从数据库中获取回话历史消息 TODO
        return it->second;
    }
    // 内存里面没有 去数据库找
    // TODO

    WARN("SessionManager::getSession session id:{} is not exsist", sessionId);
    return nullptr;
}
// 往某个会话中添加消息
bool SessionManager::addMessage(const std::string& sessionId, const Message& message) {
    _mutex.lock();
    auto it = _session.find(sessionId);
    if(it == _session.end()) {
        _mutex.unlock();
        WARN("SessionManager::addMessage session id:{} is not exsist", sessionId);
        return false;
    }
    // 新建消息
    Message msg(message._role, message._content);
    // 生成消息id
    msg._messageId = generaterMessageId(it->second->_messages.size());
    msg._timestamp = std::time(nullptr);
    // 更新回话
    it->second->_messages.push_back(msg);
    it->second->_updateAt = std::time(nullptr);
    INFO("SessionManager::addMessage addMessage successed sessionId:{} msgContent:{}", it->second->_sessionId, msg._content);
    _mutex.unlock();

    // 将新消息保存到数据库 TODO
    return true;
}
// 获取某个会话的所有历史消息
std::vector<Message> SessionManager::getHistroyMessages(const std::string& sessionId) const {
    _mutex.lock();
    auto it = _session.find(sessionId);
    if(it != _session.end()) {
        _mutex.unlock();
        return it->second->_messages;
    }
    WARN("SessionManager::getHistroyMessages session id:{} is not exsist", sessionId);
    // 如果内存里面没有 去数据库查 TODO
    return {};
}
// 更新会话时间戳
void SessionManager::updateSessionTimestamp(const std::string& sessionId) {
    _mutex.lock();
    auto it = _session.find(sessionId);
    if(it != _session.end()) {
        it->second->_updateAt = std::time(nullptr);
    }
    _mutex.unlock();
    // 更新数据库中的回话时间戳 TODO
}
// 获取会话所有会话列表 会话id
std::vector<std::string> SessionManager::getSessionLists() const {
    if(_session.empty()) {
        WARN("session lists is empty");
        return {};
    }
    std::vector<std::pair<std::string, std::time_t>> tmp;
    tmp.reserve(_session.size());  // 提前开好空间
    // 将内存中的数据放到tmp中等待排序
    for(auto& pair : _session) {
        tmp.push_back({pair.first, pair.second->_updateAt});
    }
    // 将数据库中的数据放到tmp中等待排序 TODO

    sort(tmp.begin(), tmp.end(), [](const std::pair<std::string, std::time_t> x, const std::pair<std::string, std::time_t> y) { return x.second > y.second; });
    std::vector<std::string> sessionIds;
    for(auto& pair : tmp) {
        sessionIds.push_back(pair.first);
    }
    return sessionIds;
}
// 删除某个会话
bool SessionManager::deleteSession(const std::string& sessionId) {
    _mutex.lock();
    auto it = _session.find(sessionId);
    if(it == _session.end()) {
        WARN("SessionManager::deleteSession session id:{} is not exsist", sessionId);
        return false;
    }
    _session.erase(it);
    _mutex.unlock();
    // 从数据库中删除
    // TODO

    return true;
}

// 清空所有会话
void SessionManager::clearAllSessions() {
    _mutex.lock();
    _session.clear();
    _mutex.unlock();

    // 从数据库中清除所有回话
    // TODO
}
// 获取会话总数
size_t SessionManager::getSessionCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _session.size();
    // return _sessionCounter; ????// TODO
}

}  // namespace Ai_Chat_SDK