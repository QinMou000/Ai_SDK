#include "../include/DataManager.h"

#include "../include/util/Log.h"

namespace Ai_Chat_SDK {

DataManager::DataManager(const std::string& dbName) : _dbName(dbName), _db(NULL) {
    int rc = sqlite3_open(_dbName.c_str(), &_db);
    if(rc != SQLITE_OK) {
        ERR("open database {} failed, error message : {}", _dbName, sqlite3_errmsg(_db));
    }
    INFO("open database {} success", _dbName);

    // 启用外键约束，保证级联删除 (ON DELETE CASCADE) 生效
    if(!executeSQL("PRAGMA foreign_keys = ON;")) {
        WARN("enable foreign keys constraint failed");
    }

    if(!initDataBase()) {
        ERR("init database {} failed", _dbName);
        sqlite3_close(_db);
        _db = nullptr;
    }
}
DataManager::~DataManager() {
    if(_db) {
        sqlite3_close(_db);
        INFO("close database {} success", _dbName);
        _db = nullptr;
    }
}

bool DataManager::initDataBase() {
    // 创建会话表
    std::string createSessionTableSQL =
        "CREATE TABLE IF NOT EXISTS Sessions ("
        "SessionId TEXT PRIMARY KEY,"
        "ModelName TEXT NOT NULL,"
        "CreateTime INTEGER NOT NULL,"
        "LastActiveTime INTEGER NOT NULL"
        ");";
    if(!executeSQL(createSessionTableSQL)) {
        ERR("create session table failed");
        return false;
    }
    // 创建消息表
    std::string createMessageTableSQL =
        "CREATE TABLE IF NOT EXISTS Messages ("
        "MessageId TEXT PRIMARY KEY,"
        "SessionId TEXT NOT NULL,"
        "Role TEXT NOT NULL,"
        "Content TEXT NOT NULL,"
        "CreateTime INTEGER NOT NULL,"
        "FOREIGN KEY(SessionId) REFERENCES Sessions(SessionId) ON DELETE CASCADE"
        ");";
    if(!executeSQL(createMessageTableSQL)) {
        ERR("create message table failed");
        return false;
    }
    return true;
}
bool DataManager::executeSQL(const std::string& SQL) {
    if(!_db) {
        ERR("database not open");
        return false;
    }
    char* errMsg = nullptr;
    int rc = sqlite3_exec(_db, SQL.c_str(), nullptr, nullptr, &errMsg);
    if(rc != SQLITE_OK) {
        ERR("execute SQL {} failed, error message : {}", SQL, errMsg);
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

// 对session 操作
// 插入一个会话
bool DataManager::insertNewSession(const Session& session) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string insertSessionSQL =
        "INSERT INTO Sessions (SessionId, ModelName, CreateTime, LastActiveTime) "
        "VALUES (?, ?, ?, ?);";
    sqlite3_stmt* smt;
    int rc = sqlite3_prepare_v2(_db, insertSessionSQL.c_str(), -1, &smt, nullptr);
    if(rc != SQLITE_OK) {
        ERR("prepare SQL {} failed, error message : {}", insertSessionSQL, sqlite3_errmsg(_db));
        return false;
    }
    // 绑定参数
    rc = sqlite3_bind_text(smt, 1, session._sessionId.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_bind_text(smt, 2, session._modelName.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_bind_int64(smt, 3, static_cast<int64_t>(session._createAt));
    rc = sqlite3_bind_int64(smt, 4, static_cast<int64_t>(session._updateAt));

    // 执行SQL
    rc = sqlite3_step(smt);
    if(rc != SQLITE_DONE) {
        ERR("execute SQL {} failed, error message : {}", insertSessionSQL, sqlite3_errmsg(_db));
        sqlite3_finalize(smt);
        return false;
    }
    sqlite3_finalize(smt);
    INFO("insert session {} success", session._sessionId);
    return true;
}
// 获取指定会话
std::shared_ptr<Session> DataManager::getSession(const std::string& SessionId) const {
    std::lock_guard<std::mutex> lock(_mutex);

    std::string selectSessionSQL = "SELECT ModelName, CreateTime, LastActiveTime FROM Sessions WHERE SessionId = ?;";

    sqlite3_stmt* smt;
    int rc = sqlite3_prepare_v2(_db, selectSessionSQL.c_str(), -1, &smt, nullptr);
    if(rc != SQLITE_OK) {
        ERR("prepare SQL {} failed, error message : {}", selectSessionSQL, sqlite3_errmsg(_db));
        return nullptr;
    }
    // 绑定参数
    rc = sqlite3_bind_text(smt, 1, SessionId.c_str(), -1, SQLITE_TRANSIENT);
    // 执行SQL
    rc = sqlite3_step(smt);
    if(rc == SQLITE_DONE) {
        // 正常执行完毕，但没有查到该记录
        sqlite3_finalize(smt);
        return nullptr;
    } else if(rc != SQLITE_ROW) {
        ERR("execute SQL {} failed, error message : {}", selectSessionSQL, sqlite3_errmsg(_db));
        sqlite3_finalize(smt);
        return nullptr;
    }
    // 从结果中提取数据
    std::string modelName = reinterpret_cast<const char*>(sqlite3_column_text(smt, 0));
    std::time_t createTime = static_cast<std::time_t>(sqlite3_column_int64(smt, 1));
    std::time_t lastActiveTime = static_cast<std::time_t>(sqlite3_column_int64(smt, 2));
    sqlite3_finalize(smt);
    // 创建会话对象
    auto session = std::make_shared<Session>(modelName);
    session->_sessionId = SessionId;
    session->_createAt = static_cast<std::time_t>(createTime);
    session->_updateAt = static_cast<std::time_t>(lastActiveTime);
    INFO("get session {} success", session->_sessionId);

    // 获取该会话的所有消息
    // 注意：getSessionMessages 内部会获取锁，这里如果 _mutex 是非递归锁会导致死锁
    // 解决方案：这里不直接调用 getSessionMessages，或者将加锁逻辑提取出来
    // 这里采用直接查询的方式避免死锁
    session->_messages = getSessionMessagesNoLock(session->_sessionId);

    return session;
}
// 更新指定会话的时间戳
bool DataManager::updateSessionTimeStamp(const std::string& sessionId, std::time_t timeStamp) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string updateSessionSQL = "UPDATE Sessions SET LastActiveTime = ? WHERE SessionId = ?;";

    sqlite3_stmt* smt;
    int rc = sqlite3_prepare_v2(_db, updateSessionSQL.c_str(), -1, &smt, nullptr);
    if(rc != SQLITE_OK) {
        ERR("prepare SQL {} failed, error message : {}", updateSessionSQL, sqlite3_errmsg(_db));
        return false;
    }
    // 绑定参数
    rc = sqlite3_bind_int64(smt, 1, static_cast<int64_t>(timeStamp));
    rc = sqlite3_bind_text(smt, 2, sessionId.c_str(), -1, SQLITE_TRANSIENT);
    // 执行SQL
    rc = sqlite3_step(smt);
    if(rc != SQLITE_DONE) {
        ERR("execute SQL {} failed, error message : {}", updateSessionSQL, sqlite3_errmsg(_db));
        sqlite3_finalize(smt);
        return false;
    }
    sqlite3_finalize(smt);

    INFO("update session {} time stamp success", sessionId);
    return true;
}
// 删除指定会话 同时删除所有消息
bool DataManager::deleteSession(const std::string& SessionId) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string deleteSessionSQL = "DELETE FROM Sessions WHERE SessionId = ?;";

    sqlite3_stmt* smt;
    int rc = sqlite3_prepare_v2(_db, deleteSessionSQL.c_str(), -1, &smt, nullptr);
    if(rc != SQLITE_OK) {
        ERR("prepare SQL {} failed, error message : {}", deleteSessionSQL, sqlite3_errmsg(_db));
        return false;
    }
    // 绑定参数
    rc = sqlite3_bind_text(smt, 1, SessionId.c_str(), -1, SQLITE_TRANSIENT);
    // 执行SQL
    rc = sqlite3_step(smt);
    if(rc != SQLITE_DONE) {
        ERR("execute SQL {} failed, error message : {}", deleteSessionSQL, sqlite3_errmsg(_db));
        sqlite3_finalize(smt);
        return false;
    }
    sqlite3_finalize(smt);

    INFO("delete session {} success", SessionId);
    return true;
}
// 列出所有会话id 并按照更新时间 降序排列
std::vector<std::string> DataManager::listAllSessionIds() const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<std::string> sessionIds;
    std::string selectSessionIdsSQL = "SELECT SessionId FROM Sessions ORDER BY LastActiveTime DESC;";

    sqlite3_stmt* smt;
    int rc = sqlite3_prepare_v2(_db, selectSessionIdsSQL.c_str(), -1, &smt, nullptr);
    if(rc != SQLITE_OK) {
        ERR("prepare SQL {} failed, error message : {}", selectSessionIdsSQL, sqlite3_errmsg(_db));
        return sessionIds;
    }
    // 执行SQL
    rc = sqlite3_step(smt);
    while(rc == SQLITE_ROW) {
        const char* sessionId = reinterpret_cast<const char*>(sqlite3_column_text(smt, 0));
        sessionIds.push_back(sessionId);
        rc = sqlite3_step(smt);
    }
    sqlite3_finalize(smt);

    INFO("list all session ids success size : {}", sessionIds.size());
    return sessionIds;
}
// 列出所有会话 并按照更新时间 降序排列
std::vector<std::shared_ptr<Session>> DataManager::listAllSessions() const {
    std::lock_guard<std::mutex> lock(_mutex);

    std::vector<std::shared_ptr<Session>> sessions;
    std::string selectSessionsSQL = "SELECT SessionId, ModelName, CreateTime, LastActiveTime FROM Sessions ORDER BY LastActiveTime DESC;";

    sqlite3_stmt* smt;
    int rc = sqlite3_prepare_v2(_db, selectSessionsSQL.c_str(), -1, &smt, nullptr);
    if(rc != SQLITE_OK) {
        ERR("prepare SQL {} failed, error message : {}", selectSessionsSQL, sqlite3_errmsg(_db));
        return sessions;
    }
    // 执行SQL
    rc = sqlite3_step(smt);
    while(rc == SQLITE_ROW) {
        const char* sessionId = reinterpret_cast<const char*>(sqlite3_column_text(smt, 0));
        std::string modelName = reinterpret_cast<const char*>(sqlite3_column_text(smt, 1));
        int64_t createTime = sqlite3_column_int64(smt, 2);
        int64_t lastActiveTime = sqlite3_column_int64(smt, 3);

        // 创建会话对象
        auto session = std::make_shared<Session>(modelName);
        session->_sessionId = sessionId;
        session->_createAt = static_cast<std::time_t>(createTime);
        session->_updateAt = static_cast<std::time_t>(lastActiveTime);
        // 历史消息暂时不获取
        session->_messages = getSessionMessagesNoLock(session->_sessionId);
        sessions.push_back(session);

        rc = sqlite3_step(smt);
    }
    sqlite3_finalize(smt);

    INFO("list all sessions success size : {}", sessions.size());
    return sessions;
}
// 获取会话总数
int DataManager::getSessionCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string selectCountSQL = "SELECT COUNT(*) FROM Sessions;";

    sqlite3_stmt* smt;
    int rc = sqlite3_prepare_v2(_db, selectCountSQL.c_str(), -1, &smt, nullptr);
    if(rc != SQLITE_OK) {
        ERR("prepare SQL {} failed, error message : {}", selectCountSQL, sqlite3_errmsg(_db));
        return 0;
    }
    // 执行SQL
    rc = sqlite3_step(smt);
    if(rc != SQLITE_ROW) {
        ERR("execute SQL {} failed, error message : {}", selectCountSQL, sqlite3_errmsg(_db));
        sqlite3_finalize(smt);
        return 0;
    }
    int count = sqlite3_column_int(smt, 0);
    sqlite3_finalize(smt);

    INFO("get session count success count : {}", count);
    return count;
}
// 清空所有会话 同时删除所有消息
bool DataManager::clearAllSessions() {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string clearSessionsSQL = "DELETE FROM Sessions;";
    if(!executeSQL(clearSessionsSQL)) {
        ERR("clear all sessions failed");
        return false;
    }
    INFO("clear all sessions success");
    return true;
}
/************************************************************************************************/
// 对message 操作
// 获取指定回话的历史消息 (无锁版本，供内部调用)
std::vector<Message> DataManager::getSessionMessagesNoLock(const std::string& sessionId) const {
    std::string selectMessagesSQL = "SELECT MessageId, Role, Content, CreateTime FROM Messages WHERE SessionId = ?;";

    sqlite3_stmt* smt;
    int rc = sqlite3_prepare_v2(_db, selectMessagesSQL.c_str(), -1, &smt, nullptr);
    if(rc != SQLITE_OK) {
        ERR("prepare SQL {} failed, error message : {}", selectMessagesSQL, sqlite3_errmsg(_db));
        return {};
    }
    // 绑定参数
    rc = sqlite3_bind_text(smt, 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
    // 执行SQL
    rc = sqlite3_step(smt);
    if(rc != SQLITE_ROW && rc != SQLITE_DONE) {
        ERR("execute SQL {} failed, error message : {}", selectMessagesSQL, sqlite3_errmsg(_db));
        sqlite3_finalize(smt);
        return {};
    }
    std::vector<Message> messages;
    while(rc == SQLITE_ROW) {
        Message message;
        message._messageId = std::string(reinterpret_cast<const char*>(sqlite3_column_text(smt, 0)));
        message._role = std::string(reinterpret_cast<const char*>(sqlite3_column_text(smt, 1)));
        message._content = std::string(reinterpret_cast<const char*>(sqlite3_column_text(smt, 2)));
        message._timestamp = sqlite3_column_int64(smt, 3);
        messages.push_back(message);
        rc = sqlite3_step(smt);
    }
    if(rc != SQLITE_DONE) {
        ERR("execute SQL {} failed, error message : {}", selectMessagesSQL, sqlite3_errmsg(_db));
        sqlite3_finalize(smt);
        return {};
    }
    sqlite3_finalize(smt);
    INFO("get session messages success size : {}", messages.size());
    return messages;
}

// 插入新消息 需要更新会话时间戳
bool DataManager::insertNewMessage(const std::string& sessionId, Message& message) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string insertMessageSQL = "INSERT INTO Messages (MessageId, SessionId, Role, Content, CreateTime) VALUES (?, ?, ?, ?, ?);";

    sqlite3_stmt* smt;
    int rc = sqlite3_prepare_v2(_db, insertMessageSQL.c_str(), -1, &smt, nullptr);
    if(rc != SQLITE_OK) {
        ERR("prepare SQL {} failed, error message : {}", insertMessageSQL, sqlite3_errmsg(_db));
        return false;
    }

    // 绑定参数
    rc = sqlite3_bind_text(smt, 1, message._messageId.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_bind_text(smt, 2, sessionId.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_bind_text(smt, 3, message._role.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_bind_text(smt, 4, message._content.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_bind_int64(smt, 5, static_cast<int64_t>(message._timestamp));
    // 执行SQL
    rc = sqlite3_step(smt);
    if(rc != SQLITE_DONE) {
        ERR("execute SQL {} failed, error message : {}", insertMessageSQL, sqlite3_errmsg(_db));
        sqlite3_finalize(smt);
        return false;
    }

    // 更新会话时间戳
    std::string updateSQL = R"(
        UPDATE Sessions SET LastActiveTime = ? WHERE SessionId = ?;
    )";
    // 准备SQL语句
    sqlite3_stmt* updateStmt;
    rc = sqlite3_prepare_v2(_db, updateSQL.c_str(), -1, &updateStmt, nullptr);
    if(rc != SQLITE_OK) {
        ERR("prepare SQL {} failed, error message : {}", updateSQL, sqlite3_errmsg(_db));
        sqlite3_finalize(updateStmt);
        return false;
    }

    // 绑定参数
    sqlite3_bind_int64(updateStmt, 1, static_cast<int64_t>(message._timestamp));
    sqlite3_bind_text(updateStmt, 2, sessionId.c_str(), -1, SQLITE_TRANSIENT);

    // 执行SQL语句
    rc = sqlite3_step(updateStmt);
    if(rc != SQLITE_DONE) {
        ERR("execute SQL {} failed, error message : {}", updateSQL, sqlite3_errmsg(_db));
        sqlite3_finalize(updateStmt);
        return false;
    }

    // 释放语句
    sqlite3_finalize(smt);
    sqlite3_finalize(updateStmt);
    INFO("insert new message {} success", message._content);
    return true;
}
// 获取指定回话的历史消息
std::vector<Message> DataManager::getSessionMessages(const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return getSessionMessagesNoLock(sessionId);
}
// 删除指定回话的所有消息
bool DataManager::deleteSessionMessages(const std::string& SessionId) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string deleteMessagesSQL = "DELETE FROM Messages WHERE SessionId = ?;";

    sqlite3_stmt* smt;
    int rc = sqlite3_prepare_v2(_db, deleteMessagesSQL.c_str(), -1, &smt, nullptr);
    if(rc != SQLITE_OK) {
        ERR("prepare SQL {} failed, error message : {}", deleteMessagesSQL, sqlite3_errmsg(_db));
        return false;
    }
    // 绑定参数
    rc = sqlite3_bind_text(smt, 1, SessionId.c_str(), -1, SQLITE_TRANSIENT);
    // 执行SQL
    rc = sqlite3_step(smt);
    if(rc != SQLITE_DONE) {
        ERR("execute SQL {} failed, error message : {}", deleteMessagesSQL, sqlite3_errmsg(_db));
        sqlite3_finalize(smt);
        return false;
    }
    sqlite3_finalize(smt);
    INFO("delete session messages success");
    return true;
}
}  // namespace Ai_Chat_SDK