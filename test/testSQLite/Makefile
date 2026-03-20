TEST_SQLITE=testSqlite
cc=g++

$(TEST_SQLITE):main.cpp
	$(cc) -o $@ $^ -std=c++14 -lsqlite3

.PHONY:clean
clean:
	rm -f $(TEST_SQLITE)