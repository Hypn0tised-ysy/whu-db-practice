#!/bin/bash
cd /home/hypnotised/Desktop/projects/RMDB-os-practice/db2024/rmdb
pkill -9 rmdb 2>/dev/null; sleep 2; rm -rf /tmp/obtest
./build/bin/rmdb /tmp/obtest > /dev/null 2>&1 & sleep 5
C=./rmdb_client/build/rmdb_client
rm -f /tmp/obtest/output.txt
echo "create table orders (company char(10), order_number int);"|timeout 5 $C>/dev/null 2>&1;sleep 0.3
for r in "('AAA',12)" "('ABB',13)" "('ABC',19)" "('ACA',1)"; do echo "insert into orders values $r;"|timeout 5 $C>/dev/null 2>&1;sleep 0.3; done
for q in "Q1,SELECT company, order_number FROM orders ORDER BY order_number;" \
         "Q2,SELECT company, order_number FROM orders ORDER BY company, order_number;" \
         "Q3,SELECT company, order_number FROM orders ORDER BY company DESC, order_number ASC;" \
         "Q4,SELECT company, order_number FROM orders ORDER BY order_number ASC LIMIT 2;"; do
  name=$(echo $q|cut -d, -f1); sql=$(echo $q|cut -d, -f2-)
  rm -f /tmp/obtest/output.txt
  echo "$sql"|timeout 5 $C>/dev/null 2>&1; sleep 0.5
  cp /tmp/obtest/output.txt "test/orderby_test/${name}_output.txt" 2>/dev/null
done
cat test/orderby_test/Q1_output.txt test/orderby_test/Q2_output.txt test/orderby_test/Q3_output.txt test/orderby_test/Q4_output.txt > test/orderby_test/all_output.txt
pkill -9 rmdb 2>/dev/null
echo "DONE - check test/orderby_test/"
