#!/bin/bash
set -e
cd /home/hypnotised/Desktop/projects/RMDB-os-practice/db2024/rmdb
pkill -9 rmdb 2>/dev/null || true; sleep 2
rm -rf /tmp/concurrency_db
./build/bin/rmdb /tmp/concurrency_db > /dev/null 2>&1 &
SERVER_PID=$!
sleep 5

C=./rmdb_client/build/rmdb_client
O=./test/concurrency_test

echo "=== Setup data ==="
cat << 'SETUP' | timeout 10 $C > /dev/null 2>&1
create table concurrency_test (id int, name char(8), score float);
insert into concurrency_test values (1, 'xiaohong', 90.0);
insert into concurrency_test values (2, 'xiaoming', 95.0);
insert into concurrency_test values (3, 'zhanghua', 88.5);
SETUP
sleep 1

echo "=== Test 1: Dirty Read Prevention ==="
# Transaction 1: begin, update row 2, hold lock
# Transaction 2: begin, try to read row 2 → should block/abort
# Run txn1 in background, then txn2, then check results

# Start txn1 in background: begin + update (holds X lock)
( echo "begin;"; sleep 0.3; echo "update concurrency_test set score = 100.0 where id = 2;"; sleep 5; echo "abort;"; ) | timeout 10 $C > /tmp/txn1_out.txt 2>&1 &
T1PID=$!
sleep 1  # Wait for txn1 to acquire lock

# Start txn2: begin + select (tries S lock → conflict → abort)
( echo "begin;"; sleep 0.2; echo "select * from concurrency_test where id = 2;"; echo "commit;"; ) | timeout 5 $C > /tmp/txn2_out.txt 2>&1
T2RC=$?
wait $T1PID 2>/dev/null

echo "Txn1 output:"; cat /tmp/txn1_out.txt 2>/dev/null | tail -5
echo "Txn2 output:"; cat /tmp/txn2_out.txt 2>/dev/null | tail -5

# Verify: after txn1 abort, row 2 should still have score=95.0
rm -f /tmp/concurrency_db/output.txt
echo "select * from concurrency_test where id = 2;" | timeout 5 $C > /dev/null 2>&1
sleep 0.5
cp /tmp/concurrency_db/output.txt "$O/Q1_output.txt" 2>/dev/null
echo "Q1 Final state:"; cat "$O/Q1_output.txt" 2>/dev/null || echo "EMPTY"

echo ""
echo "=== Test 2: Basic Lock ==="
# Single transaction: begin, update, commit → verify persistence
rm -f /tmp/concurrency_db/output.txt
cat << 'T2' | timeout 10 $C > /dev/null 2>&1
begin;
update concurrency_test set score = 99.0 where id = 1;
commit;
select * from concurrency_test where id = 1;
T2
sleep 0.5
cp /tmp/concurrency_db/output.txt "$O/Q2_output.txt" 2>/dev/null
echo "Q2:"; cat "$O/Q2_output.txt" 2>/dev/null || echo "EMPTY"

kill $SERVER_PID 2>/dev/null
echo "DONE"
