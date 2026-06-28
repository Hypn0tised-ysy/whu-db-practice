#!/bin/bash
# Index test script
set -e
cd /home/hypnotised/Desktop/projects/RMDB-os-practice/db2024/rmdb

# Kill any existing server
pkill -f "./build/bin/rmdb" 2>/dev/null || true
sleep 1

rm -rf index_test_db

# Start server in background
./build/bin/rmdb index_test_db &
SERVER_PID=$!
sleep 2

CLIENT="./rmdb_client/build/rmdb_client"

run_sql() {
  echo "$1" | timeout 5 $CLIENT 2>/dev/null | grep -v "^Rucbase>" | grep -v "^Bye\." || true
  sleep 0.3
}

echo "========== Test 1: Create/Drop/Show Index =========="
rm -f index_test_db/output.txt
run_sql "create table warehouse (id int, name char(8));"
run_sql "create index warehouse (id);"
run_sql "show index from warehouse;"
run_sql "create index warehouse (id,name);"
run_sql "show index from warehouse;"
run_sql "drop index warehouse (id);"
run_sql "drop index warehouse (id,name);"
run_sql "show index from warehouse;"
echo "--- Test 1 Output ---"
cat index_test_db/output.txt
echo ""

echo "========== Test 2: Index Query =========="
rm -f index_test_db/output.txt
run_sql "create table warehouse2 (w_id int, name char(8));"
run_sql "insert into warehouse2 values (10 , 'qweruiop');"
run_sql "insert into warehouse2 values (534, 'asdfhjkl');"
run_sql "insert into warehouse2 values (100,'qwerghjk');"
run_sql "insert into warehouse2 values (500,'bgtyhnmj');"
run_sql "create index warehouse2(w_id);"
run_sql "select * from warehouse2 where w_id = 10;"
run_sql "select * from warehouse2 where w_id < 534 and w_id > 100;"
run_sql "drop index warehouse2(w_id);"
run_sql "create index warehouse2(name);"
run_sql "select * from warehouse2 where name = 'qweruiop';"
run_sql "select * from warehouse2 where name > 'qwerghjk';"
run_sql "select * from warehouse2 where name > 'aszdefgh' and name < 'qweraaaa';"
run_sql "drop index warehouse2(name);"
run_sql "create index warehouse2(w_id,name);"
run_sql "select * from warehouse2 where w_id = 100 and name = 'qwerghjk';"
run_sql "select * from warehouse2 where w_id < 600 and name > 'bztyhnmj';"
echo "--- Test 2 Output ---"
cat index_test_db/output.txt
echo ""

echo "========== Test 3: Index Maintenance =========="
rm -f index_test_db/output.txt
run_sql "create table warehouse3 (w_id int, name char(8));"
run_sql "insert into warehouse3 values (10 , 'qweruiop');"
run_sql "insert into warehouse3 values (534, 'asdfhjkl');"
run_sql "select * from warehouse3 where w_id = 10;"
run_sql "select * from warehouse3 where w_id < 534 and w_id > 100;"
run_sql "create index warehouse3(w_id);"
run_sql "insert into warehouse3 values (500, 'lastdanc');"
run_sql "update warehouse3 set w_id = 507 where w_id = 534;"
run_sql "select * from warehouse3 where w_id = 10;"
run_sql "select * from warehouse3 where w_id < 534 and w_id > 100;"
echo "--- Test 3 Output ---"
cat index_test_db/output.txt
echo ""

# Cleanup
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null
echo "========== All Tests Done =========="
