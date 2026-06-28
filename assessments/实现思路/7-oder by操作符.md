# 任务7：ORDER BY操作符 + LIMIT

## 提交记录
`9054c9e` - task7: impl order by

## 问题分析
框架已有SortExecutor桩代码和单列ORDER BY语法，但：
1. SortExecutor为空实现（返回nullptr）
2. 不支持多列ORDER BY
3. 不支持混合ASC/DESC
4. 不支持LIMIT子句

## 修改文件

| 文件 | 改动内容 |
|------|---------|
| `src/execution/execution_sort.h` | 完整实现SortExecutor：多列排序，ASC/DESC每列独立控制 |
| `src/parser/ast.h` | OrderBy改为vector（多列支持），SelectStmt新增`has_limit`/`limit_val` |
| `src/parser/yacc.y` | 多列`order_list`语法（`col ASC, col2 DESC`），`opt_limit_clause`，epsilon设nullptr防崩溃 |
| `src/parser/lex.l` | 新增`LIMIT`关键字 |
| `src/optimizer/plan.h` | SortPlan改为vector<TabCol>，DMLPlan新增`limit_val` |
| `src/optimizer/planner.cpp` | `generate_sort_plan`支持多列 |
| `src/portal.h` | SortExecutor接收vector参数，limit_val传递 |
| `src/execution/execution_manager.cpp/h` | select_from新增limit_val参数，循环中break检查 |

## 实现思路

### 1. SortExecutor
- **收集阶段**：`beginTuple()`遍历child执行器，用`prev_->Next()`收集所有记录到`sorted_data_` vector< vector<char> >
- **排序阶段**：`std::sort`+自定义比较器，按`sort_cols_`顺序逐列比较，DESC时反转结果
- **返回阶段**：`Next()`按序返回排序后的记录

### 2. 比较器（`cmpcol`）
支持INT/BIGINT/FLOAT/STRING/DATETIME五种类型，STRING去尾零后比较。

### 3. 多列ORDER BY
- OrderBy结构改为`vector<shared_ptr<Col>> cols`+`vector<OrderByDir> orderby_dirs`
- 解析：`order_list: col opt_asc_desc | order_list ',' col opt_asc_desc`
- 每列独立指定ASC/DESC

### 4. LIMIT支持
- 解析：`opt_limit_clause: LIMIT VALUE_INT` → 设置`has_limit=true, limit_val=N`
- 传播链：AST→Query→DMLPlan→PortalStmt→select_from
- `select_from`循环中：`if (limit_val > 0 && num_rec >= limit_val) break`

### 5. 关键Bug修复
- yacc epsilon规则：原`{ /* ignore */ }`未初始化`$$`→shared_ptr为垃圾值→随机触发has_sort=true→Segment Fault
- 修复：`{ $$ = nullptr; }`→明确设null→has_sort=false→跳过排序
