#!/bin/bash
cd /home/hypnotised/Desktop/projects/RMDB-os-practice/db2024/rmdb
pkill -f "./build/bin/rmdb" 2>/dev/null
sleep 1
rm -rf /tmp/xtest
./build/bin/rmdb /tmp/xtest &
sleep 2
echo "create table warehouse (w_id int, name char(8));" | ./rmdb_client/build/rmdb_client
sleep 0.3
echo "insert into warehouse values (10 , 'qweruiop');" | ./rmdb_client/build/rmdb_client
sleep 0.3
echo "insert into warehouse values (534, 'asdfhjkl');" | ./rmdb_client/build/rmdb_client
sleep 0.3
echo "create index warehouse(w_id);" | ./rmdb_client/build/rmdb_client
sleep 0.3
echo "insert into warehouse values (10, 'uiopqwer');" | ./rmdb_client/build/rmdb_client
sleep 0.3
echo "select * from warehouse;" | ./rmdb_client/build/rmdb_client
sleep 0.3
echo "=== OUTPUT ==="
cat /tmp/xtest/output.txt
pkill -f "./build/bin/rmdb" 2>/dev/null
