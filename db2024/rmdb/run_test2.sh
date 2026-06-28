#!/bin/bash
cd /home/hypnotised/Desktop/projects/RMDB-os-practice/db2024/rmdb
pkill -f "./build/bin/rmdb" 2>/dev/null
sleep 1
rm -rf /tmp/idx_test2
./build/bin/rmdb /tmp/idx_test2 &
sleep 2
C=./rmdb_client/build/rmdb_client

rm -f /tmp/idx_test2/output.txt

echo "create table warehouse (w_id int, name char(8));" | timeout 3 $C >/dev/null 2>&1
sleep 0.3
for v in "(10 , 'qweruiop')" "(534, 'asdfhjkl')" "(100,'qwerghjk')" "(500,'bgtyhnmj')"; do
  echo "insert into warehouse values $v;" | timeout 3 $C >/dev/null 2>&1
  sleep 0.2
done
echo "create index warehouse(w_id);" | timeout 3 $C >/dev/null 2>&1
sleep 0.4
echo "select * from warehouse where w_id = 10;" | timeout 3 $C >/dev/null 2>&1
sleep 0.3
echo "select * from warehouse where w_id < 534 and w_id > 100;" | timeout 3 $C >/dev/null 2>&1
sleep 0.3
echo "drop index warehouse(w_id);" | timeout 3 $C >/dev/null 2>&1
sleep 0.3
echo "create index warehouse(name);" | timeout 3 $C >/dev/null 2>&1
sleep 0.4
echo "select * from warehouse where name = 'qweruiop';" | timeout 3 $C >/dev/null 2>&1
sleep 0.3
echo "select * from warehouse where name > 'qwerghjk';" | timeout 3 $C >/dev/null 2>&1
sleep 0.3
echo "select * from warehouse where name > 'aszdefgh' and name < 'qweraaaa';" | timeout 3 $C >/dev/null 2>&1
sleep 0.3
echo "drop index warehouse(name);" | timeout 3 $C >/dev/null 2>&1
sleep 0.3
echo "create index warehouse(w_id,name);" | timeout 3 $C >/dev/null 2>&1
sleep 0.4
echo "select * from warehouse where w_id = 100 and name = 'qwerghjk';" | timeout 3 $C >/dev/null 2>&1
sleep 0.3
echo "select * from warehouse where w_id < 600 and name > 'bztyhnmj';" | timeout 3 $C >/dev/null 2>&1
sleep 0.3

echo "=== Test 2 Output ==="
cat /tmp/idx_test2/output.txt

pkill -f "./build/bin/rmdb" 2>/dev/null
