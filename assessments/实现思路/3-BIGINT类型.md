# 任务3：BIGINT类型

## 提交记录
`903f069` - task3-4: impl bigInt and date time types

## 问题分析
数据库只支持4字节有符号INT。需要实现8字节有符号BIGINT，存储范围与MySQL一致：-9,223,372,036,854,775,808 ~ 9,223,372,036,854,775,807。

关键挑战：
1. 整数字面量解析需要64位（原`atoi`只支持32位）
2. 记录存储需要8字节（原INT 4字节）
3. 类型系统需要新增BIGINT类型
4. 溢出检测

## 修改文件

| 文件 | 改动内容 |
|------|---------|
| `src/defs.h` | 新增`TYPE_BIGINT`枚举值，`coltype2str`增加BIGINT映射 |
| `src/parser/ast.h` | `IntLit::val`改为`int64_t`，`SemValue::sv_int`改为`int64_t`，新增`SV_TYPE_BIGINT` |
| `src/parser/ast_printer.h` | 新增`SV_TYPE_BIGINT → "BIGINT"`类型名映射 |
| `src/parser/lex.l` | 新增`BIGINT`关键字，VALUE_INT解析改用`strtoll`（64位），溢出检测（errno=ERANGE→标记overflow） |
| `src/parser/yacc.y` | 新增`BIGINT`类型规则：`sizeof(int64_t)`即8字节 |
| `src/common/common.h` | Value联合体改用`int64_t bigint_val`，新增`set_bigint()`，`init_raw()`支持8字节写入 |
| `src/optimizer/planner.h` | `SV_TYPE_BIGINT → TYPE_BIGINT`映射 |
| `src/analyze/analyze.cpp` | `convert_sv_value`中溢出检测+自动选BIGINT，类型兼容性（INT↔BIGINT↔FLOAT） |
| `src/execution/execution_manager.cpp` | BIGINT显示：`*(int64_t*)rec_buf` |
| `src/execution/executor_seq_scan.h` | eval_condition/eval_binary新增TYPE_BIGINT分支 |
| `src/execution/executor_nestedloop_join.h` | compare_values新增TYPE_BIGINT分支 |
| `src/execution/executor_insert.h` | INT→BIGINT隐式转换支持 |
| `src/index/ix_index_handle.h` | ix_compare新增TYPE_BIGINT分支 |

## 实现思路

### 1. 64位整数解析
- lexer中`atoi`→`strtoll`：支持64位整数解析
- 溢出检测：`errno==ERANGE`时设置`overflow=true`标志
- `convert_sv_value`中检查overflow并抛异常

### 2. 类型系统扩展
- `ColType`新增`TYPE_BIGINT`
- `SvType`新增`SV_TYPE_BIGINT`
- Value联合体：`int64_t bigint_val`替代原`int int_val`

### 3. 存储与比较
- `init_raw()`: TYPE_BIGINT分支写入`*(int64_t*)raw->data = bigint_val`
- 所有比较函数（eval_condition, compare_values, ix_compare）新增TYPE_BIGINT分支
- 类型兼容：INSERT允许INT→BIGINT自动提升，条件比较允许INT↔BIGINT

### 4. 溢出保护
- BIGINT列允许值范围：INT64_MIN ~ INT64_MAX
- `strtoll`溢出时返回INT64_MAX/MIN并标记overflow
- `convert_sv_value`检查overflow抛异常→写failure到output.txt
