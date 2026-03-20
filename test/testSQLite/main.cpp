#include <sqlite3.h>

#include <iostream>
#include <string>

struct StudentInfo {
    std::string name;
    std::string gender;
    int age;
    double gap;

    StudentInfo(const std::string& name, const std::string& gender, int age, double gap) : name(name), gender(gender), age(age), gap(gap) {}
};

class StudentDB {
   public:
    StudentDB(const std::string& dbName) {
        // 创建并打开数据库
        int rc = sqlite3_open(dbName.c_str(), &_db);
        if(rc != SQLITE_OK) {
            std::cerr << "打开数据库失败：" << sqlite3_errmsg(_db) << std::endl;
            sqlite3_close(_db);
        }
        // 初始化数据库表 - 创建学生信息表
        if(!initDataBase()) {
            sqlite3_close(_db);
        }
    }

    ~StudentDB() {
        if(_db != nullptr) {
            sqlite3_close(_db);
        }
    }

    bool insertStudentInfo(const StudentInfo& studentInfo) {
        // 插入学生信息
        std::string insertSQL = R"(
            INSERT INTO Student (name, gender, age, gap)
            VALUES (?, ?, ?, ?);
        )";

        // 准备SQL语句
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(_db, insertSQL.c_str(), -1, &stmt, nullptr);
        if(rc != SQLITE_OK) {
            std::cerr << "准备语句失败：" << sqlite3_errmsg(_db) << std::endl;
            return false;
        }

        // 绑定参数
        sqlite3_bind_text(stmt, 1, studentInfo.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, studentInfo.gender.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, studentInfo.age);
        sqlite3_bind_double(stmt, 4, studentInfo.gap);

        // 执行SQL语句
        rc = sqlite3_step(stmt);
        if(rc != SQLITE_DONE) {
            std::cerr << "执行语句失败：" << sqlite3_errmsg(_db) << std::endl;
            sqlite3_finalize(stmt);
            return false;
        }

        // 清理
        sqlite3_finalize(stmt);
        return true;
    }
    bool queryStudentInfo(const std::string& name) {
        // 查询学生信息
        std::string querySQL = R"(
            SELECT stuid, name, gender, age, gap FROM Student WHERE name = ?;
        )";

        // 准备SQL语句
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(_db, querySQL.c_str(), -1, &stmt, nullptr);
        if(rc != SQLITE_OK) {
            std::cerr << "准备语句失败：" << sqlite3_errmsg(_db) << std::endl;
            return false;
        }

        // 绑定参数
        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);

        // 执行SQL语句
        rc = sqlite3_step(stmt);
        if(rc != SQLITE_ROW) {
            std::cerr << "执行语句失败：" << sqlite3_errmsg(_db) << std::endl;
            sqlite3_finalize(stmt);
            return false;
        }

        // 提取结果
        int stuid = sqlite3_column_int(stmt, 0);
        std::string queryName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        std::string queryGender = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        int queryAge = sqlite3_column_int(stmt, 3);
        double queryGap = sqlite3_column_double(stmt, 4);

        // 打印结果
        std::cout << "查询到学生信息：" << std::endl;
        std::cout << "stuid: " << stuid << std::endl;
        std::cout << "name: " << queryName << std::endl;
        std::cout << "gender: " << queryGender << std::endl;
        std::cout << "age: " << queryAge << std::endl;
        std::cout << "gap: " << queryGap << std::endl;

        // 清理
        sqlite3_finalize(stmt);
        return true;
    }
    bool queryAllStudentInfo() {
        // 查询所有学生信息
        std::string querySQL = R"(
            SELECT * FROM Student;
        )";

        // 准备SQL语句
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(_db, querySQL.c_str(), -1, &stmt, nullptr);
        if(rc != SQLITE_OK) {
            std::cerr << "准备语句失败：" << sqlite3_errmsg(_db) << std::endl;
            return false;
        }

        // 执行SQL语句
        rc = sqlite3_step(stmt);
        if(rc != SQLITE_ROW && rc != SQLITE_DONE) {
            std::cerr << "执行语句失败：" << sqlite3_errmsg(_db) << std::endl;
            sqlite3_finalize(stmt);
            return false;
        }

        // 提取结果
        std::cout << "-------------所有学生信息-----------------" << std::endl;
        while(rc == SQLITE_ROW) {
            int stuid = sqlite3_column_int(stmt, 0);
            std::string queryName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            std::string queryGender = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            int queryAge = sqlite3_column_int(stmt, 3);
            double queryGap = sqlite3_column_double(stmt, 4);

            // 打印结果
            std::cout << "查询到学生信息：" << std::endl;
            std::cout << "stuid: " << stuid << " "
                      << "name: " << queryName << " "
                      << "gender: " << queryGender << " "
                      << "age: " << queryAge << " "
                      << "gap: " << queryGap << std::endl;

            // 继续提取下一行
            rc = sqlite3_step(stmt);
        }

        // 检查是否还有更多行
        if(rc != SQLITE_DONE) {
            std::cerr << "提取结果失败：" << sqlite3_errmsg(_db) << std::endl;
            sqlite3_finalize(stmt);
            return false;
        }

        // 清理
        sqlite3_finalize(stmt);
        return true;
    }
    // 修改学生信息
    bool updateStudentInfo(const std::string& name, const StudentInfo& info) {
        // 更新学生信息
        std::string updateSQL = R"(
            UPDATE Student SET gender = ?, age = ?, gap = ? WHERE name = ?;
        )";

        // 准备SQL语句
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(_db, updateSQL.c_str(), -1, &stmt, nullptr);
        if(rc != SQLITE_OK) {
            std::cerr << "准备语句失败：" << sqlite3_errmsg(_db) << std::endl;
            return false;
        }

        // 绑定参数
        sqlite3_bind_text(stmt, 1, info.gender.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, info.age);
        sqlite3_bind_double(stmt, 3, info.gap);
        sqlite3_bind_text(stmt, 4, name.c_str(), -1, SQLITE_TRANSIENT);

        // 执行SQL语句
        rc = sqlite3_step(stmt);
        if(rc != SQLITE_DONE && rc != SQLITE_ROW) {
            std::cerr << "执行语句失败：" << sqlite3_errmsg(_db) << std::endl;
            sqlite3_finalize(stmt);
            return false;
        }

        // 清理
        sqlite3_finalize(stmt);
        return true;
    }

    // 删除学生信息
    bool deleteStudentInfo(const std::string& name) {
        // 删除学生信息
        std::string deleteSQL = R"(
            DELETE FROM Student WHERE name = ?;
        )";

        // 准备SQL语句
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(_db, deleteSQL.c_str(), -1, &stmt, nullptr);
        if(rc != SQLITE_OK) {
            std::cerr << "准备语句失败：" << sqlite3_errmsg(_db) << std::endl;
            return false;
        }

        // 绑定参数
        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);

        // 执行SQL语句
        rc = sqlite3_step(stmt);
        if(rc != SQLITE_DONE && rc != SQLITE_ROW) {
            std::cerr << "执行语句失败：" << sqlite3_errmsg(_db) << std::endl;
            sqlite3_finalize(stmt);
            return false;
        }

        // 清理
        sqlite3_finalize(stmt);
        return true;
    }

   private:
    bool initDataBase() {
        const std::string createTableSQL = R"(
            CREATE TABLE IF NOT EXISTS Student (
                stuid INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT,
                gender TEXT,
                age INTEGER,
                gap REAL
            );
        )";
        int rc = sqlite3_exec(_db, createTableSQL.c_str(), nullptr, nullptr, nullptr);
        if(rc != SQLITE_OK) {
            std::cerr << "创建表失败：" << sqlite3_errmsg(_db) << std::endl;
            return false;
        }
        return true;
    }

   private:
    sqlite3* _db = nullptr;
};

int main() {
    StudentInfo info1 = {"张三", "男", 18, 3.5};
    StudentInfo info2 = {"李四", "女", 19, 3.8};
    StudentInfo info3 = {"王五", "男", 20, 4.0};
    StudentInfo info4 = {"赵六", "女", 21, 4.2};

    StudentDB db("studentDB.db");
    db.insertStudentInfo(info1);
    db.insertStudentInfo(info2);
    db.insertStudentInfo(info3);
    db.insertStudentInfo(info4);

    // 查询所有学生信息
    db.queryAllStudentInfo();

    info3.gap = 4.5;
    db.updateStudentInfo(info3.name, info3);
    db.queryStudentInfo(info3.name);

    // 删除学生信息
    db.deleteStudentInfo(info4.name);
    db.queryAllStudentInfo();

    return 0;
}
