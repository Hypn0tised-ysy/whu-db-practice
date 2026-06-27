7. 题目七：order by操作符
前置题目：题目二（查询执行）

题目种类：基础功能

推荐知识点：查询解析、查询处理、基本算子实现

题目描述：

ORDER BY 关键字用于对结果集按照一个列或者多个列进行排序，ORDER BY 关键字默认按照升序对记录进行排序，可以使用DESC关键字来指定按照降序排列，使用ASC关键字指定按照升序排列。同时，本题目要求支持LIMIT关键字，LIMIT关键字用来限制输出结果集的大小。

提示：

（1）功能题目对代码框架没有限制，学生可以修改、增添、删除数据结构及接口，也可以对框架进行重构。

（2）学生可以通过实现Sort算子来实现order by的功能。

 

测试示例：

create table orders (company char(10), order_number int);

insert into orders values('AAA',12);

insert into orders values('ABB',13);

insert into orders values('ABC',19);

insert into orders values('ACA',1);

SELECT company, order_number FROM orders ORDER BY order_number;

SELECT company, order_number FROM orders ORDER BY company, order_number;

SELECT company, order_number FROM orders ORDER BY company DESC, order_number ASC;

SELECT company, order_number FROM orders ORDER BY order_number ASC LIMIT 2;

期待输出：

| company | order_number |

| ACA | 1 |

| AAA | 12 |

| ABB | 13 |

| ABC | 19 |

| company | order_number |

| AAA | 12 |

| ABB | 13 |

| ABC | 19 |

| ACA | 1 |

| company | order_number |

| ACA | 1 |

| ABC | 19 |

| ABB | 13 |

| AAA | 12 |

| company | order_number |

| ACA | 1 |

| AAA | 12 |

 

测试输出要求：

本题目的输出要求写入数据库文件夹下的output.txt文件中，例如测试数据库名称为aggregator_test_db，则在测试时使用./bin/rmdb aggregator_test_db命令来启动服务端，对应输出应写入buid/aggregator_test_db/output.txt文件中。