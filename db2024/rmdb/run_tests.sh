#!/bin/bash
cd /home/hypnotised/Desktop/projects/RMDB-os-practice/db2024/rmdb
pkill -f "./build/bin/rmdb" 2>/dev/null
sleep 1
rm -rf /tmp/idxall
./build/bin/rmdb /tmp/idxall &
sleep 2
C=./rmdb_client/build/rmdb_client

echo "=== Test 1 ==="
rm -f /tmp/idxall/output.txt
echo "create table warehouse (id int, name char(8));" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "create index warehouse (id);" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "show index from warehouse;" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "create index warehouse (id,name);" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "show index from warehouse;" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "drop index warehouse (id);" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "drop index warehouse (id,name);" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "show index from warehouse;" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "T1:"
cat /tmp/idxall/output.txt

echo "=== Test 2 ==="
rm -f /tmp/idxall/output.txt
echo "create table w2 (w_id int, name char(8));" | timeout 3 $C >/dev/null 2>&1; sleep 0.2
for v in "(10 , 'qweruiop')" "(534, 'asdfhjkl')" "(100,'qwerghjk')" "(500,'bgtyhnmj')"; do
  echo "insert into w2 values $v;" | timeout 3 $C >/dev/null 2>&1; sleep 0.2
done
echo "create index w2(w_id);" | timeout 3 $C >/dev/null 2>&1; sleep 0.4
echo "select * from w2 where w_id = 10;" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "select * from w2 where w_id < 534 and w_id > 100;" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "drop index w2(w_id);" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "create index w2(name);" | timeout 3 $C >/dev/null 2>&1; sleep 0.4
echo "select * from w2 where name = 'qweruiop';" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "select * from w2 where name > 'qwerghjk';" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "select * from w2 where name > 'aszdefgh' and name < 'qweraaaa';" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "drop index w2(name);" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "create index w2(w_id,name);" | timeout 3 $C >/dev/null 2>&1; sleep 0.4
echo "select * from w2 where w_id = 100 and name = 'qwerghjk';" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "select * from w2 where w_id < 600 and name > 'bztyhnmj';" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "T2:"
cat /tmp/idxall/output.txt

echo "=== Test 3 ==="
rm -f /tmp/idxall/output.txt
echo "create table w3 (w_id int, name char(8));" | timeout 3 $C >/dev/null 2>&1; sleep 0.2
echo "insert into w3 values (10 , 'qweruiop');" | timeout 3 $C >/dev/null 2>&1; sleep 0.2
echo "insert into w3 values (534, 'asdfhjkl');" | timeout 3 $C >/dev/null 2>&1; sleep 0.2
echo "select * from w3 where w_id = 10;" | timeout 3 $C >/dev/null 2>&1; sleep 0.2
echo "select * from w3 where w_id < 534 and w_id > 100;" | timeout 3 $C >/dev/null 2>&1; sleep 0.2
echo "create index w3(w_id);" | timeout 3 $C >/dev/null 2>&1; sleep 0.4
echo "insert into w3 values (500, 'lastdanc');" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "update w3 set w_id = 507 where w_id = 534;" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "select * from w3 where w_id = 10;" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "select * from w3 where w_id < 534 and w_id > 100;" | timeout 3 $C >/dev/null 2>&1; sleep 0.3
echo "T3:"
cat /tmp/idxall/output.txt

pkill -f "./build/bin/rmdb" 2>/dev/null
echo "=== DONE ==="
