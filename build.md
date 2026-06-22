# 
cd rmdb_client
mkdir build
cd build
cmake .. [-DCMAKE_BUILD_TYPE=Debug]|[-DCMAKE_BUILD_TYPE=Release]
make rmdb_client <-j4>|<-j8> # 选择4 or 8线程编译

# execution
cd build
./bin/rmdb <database_name> # 如果存在该数据库,直接加载;若不存在该数据库,自动创建

cd rmdb_client/build
./rmdb_client

RMDB> exit;

ctrl+c退出服务端


# unit test
cd build
make unit_test
./bin/unit_test

# 
flex --header-file=lex.yy.hpp -o lex.yy.cpp lex.l
bison --defines=yacc.tab.hpp -o yacc.tab.cpp yacc.y
