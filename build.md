# RMDB Build

Commands below are intended to be run from the repository root:

```bash
cd /home/hypnotised/Desktop/projects/RMDB-os-practice
```

## Build Server

```bash
cd db2024/rmdb
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --target rmdb --parallel 4
```

The server binary is generated at:

```bash
db2024/rmdb/build/bin/rmdb
```

## Build Client

```bash
cd db2024/rmdb
cmake -S rmdb_client -B rmdb_client/build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build rmdb_client/build --target rmdb_client --parallel 4
```

The client binary is generated at:

```bash
db2024/rmdb/rmdb_client/build/rmdb_client
```

## Clangd Compile Database

The server configure command generates:

```bash
db2024/rmdb/build/compile_commands.json
```

For clangd, create convenient symlinks:

```bash
ln -sfn db2024/rmdb/build/compile_commands.json compile_commands.json
ln -sfn build/compile_commands.json db2024/rmdb/compile_commands.json
```

If you also want clangd support for the standalone client, its compile database is:

```bash
db2024/rmdb/rmdb_client/build/compile_commands.json
```

## Run

Start the server:

```bash
cd db2024/rmdb
./build/bin/rmdb <database_name>
```

Start the client in another terminal:

```bash
cd db2024/rmdb/rmdb_client/build
./rmdb_client
```

Exit the client:

```sql
exit;
```

Stop the server with `Ctrl+C`.

## Unit Test

```bash
cd db2024/rmdb
cmake --build build --target unit_test --parallel 4
./build/bin/unit_test
```

## Regenerate Parser Files

Run these commands in `db2024/rmdb/src/parser` only when parser grammar files change:

```bash
flex --header-file=lex.yy.hpp -o lex.yy.cpp lex.l
bison --defines=yacc.tab.hpp -o yacc.tab.cpp yacc.y
```
