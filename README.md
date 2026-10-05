# arc.cpp 交付说明书（未验证·解释器可玩版）

**文档编号：** ARC-CPP-HANDOVER-001-FULL  
**项目代号：** Arc 语言 v4.2 单文件解释器  
**文件：** `arc.cpp`  
**状态：** 未编译、未验证、可执行性未判定，但默认解释执行，可玩性较高  
**交付方：** VENT
**接手方：** 兄弟你  
**日期：** ____年__月__日  

---

## 一、前置性、非担保性声明

兄弟：

兹就 `arc.cpp` 之交付作如下前置性、非担保性声明：

该代码文本虽已撰写告竣，然迄今未经任何编译验证，其可构建性、可链接性与运行期语义均处于未判定状态。项目以十一批次之离散结构组织，各批次验证悉数缺位，且该分批方案系由人工智能辅助生成，故其结构自洽性与语义完备性不具先验保证。

是故，当前版本能否实际运行，无法作出确定性承诺；其可执行性近乎观测前之叠加态。你接手后，可循“编译—报错—修正”之迭代路径渐进推进，至于终至何处，悉以实际进展为界。

源码位于 `arc.cpp`；其中注释由 VSCode 所集成之 AI 生成，仅具参考性，不构成事实或语义担保，不宜尽信。我的编译环境为 MinGW-w64 GCC 16.2.0，Target 为 `x86_64-w64-mingw32`，线程模型 posix，SEH，UCRT。理论层面存在运行之可能，但非承诺。

**补充声明：** 该文件不仅是一个未验证的 NASM PoC；它首先内建了一个解释器后端，且据代码结构覆盖较完整，默认即解释执行。因此，它可被视为一门“以解释器为主要执行路径的解释型语言”。这是当前版本最实际、最可玩的优点。NASM 后端是附加风险项，解释器才是主玩法。

---

## 二、文件与环境

- **源码文件：** `arc.cpp`
- **语言标准：** 建议 C++17 或以上  
  代码使用 `std::variant`、`std::optional`、`if constexpr` 等特性。
- **编译环境：** MinGW-w64 GCC 16.2.0  
  `Target: x86_64-w64-mingw32`，`Thread model: posix`，`SEH`，`UCRT`
- **建议编译命令（未验证）：**

```bash
g++ -std=c++17 -O0 -g -Wall -Wextra arc.cpp -o arc.exe
```

若需静态链接运行库：

```bash
g++ -std=c++17 -O0 -g -static-libstdc++ -static-libgcc arc.cpp -o arc.exe
```

只做语法检查：

```bash
g++ -std=c++17 -fsyntax-only arc.cpp
```

- **默认后端：** 解释器 `--emit=interp`
- **可选后端：** `--emit=nasm`，输出 NASM 汇编
- **目标平台：** `--target=win-x86_64` 或 `--target=linux-x86_64`
- **命令行选项：**
  - `-v, --verbose`
  - `--tokens`
  - `--no-pp`
  - `--ast`
  - `--emit=interp|nasm`
  - `--target=win-x86_64|linux-x86_64`
  - `-o <path>`
  - `-h, --help`

---

## 三、代码结构总览

文件头自称 P1–P11，实际尾部另附 P12 后端骨架。整体均处于未验证状态。

| 批次 | 内容 | 状态 |
|---|---|---|
| P1 | 头文件、错误类型、类型系统、Token、关键字表 | 未验证 |
| P2 | 词法分析器 Lexer | 未验证 |
| P3 | 预处理器 Preprocessor | 未验证 |
| P4 | 抽象语法树 AST | 未验证 |
| P5 | 语法解析器：骨架、顶层、类型、声明 | 未验证 |
| P6 | 语法解析器：语句、模式 | 未验证 |
| P7 | 语法解析器：表达式、cte/sext 最小执行 | 未验证 |
| P8 | 运行时 Value / Env / Object / Array / Map / Opt / Rlt | 未验证 |
| P9 | 解释器：表达式求值 | 未验证 |
| P10 | 解释器：语句执行 + 协程状态机 | 未验证 |
| P11 | `Interpreter::run()` + `main` 驱动 | 未验证 |
| P12 | Target / Backend 抽象 + NASM PoC | 未验证，尤甚 |

---

## 四、代码简介

### P1：错误、类型、Token

- `ArcError`：带行列信息的运行时错误。
- `Type`：Arc 类型系统，包括：
  `void / nil / int / flt / chr / bol / str / cod / arr / map / ptr / ownptr / opt / sig / rng / slc / tup / tck / rlt / fun / named`。
- 提供 `size()`、`align()`、`toString()`、`make*` 工厂。
- `TT`：Token 类型枚举。
- `keywordTable()`：Arc 关键字到 Token 的映射。
- `tokenStartsType()`：判断某 Token 是否可作为类型开头。

### P2：Lexer

- 缩进敏感词法分析。
- 生成 `Newline / Indent / Dedent`。
- 禁止 tab 缩进，只允许空格。
- 支持 `{* ... *}` 块注释，`;` 行注释；`for` 头中 `;` 有特殊处理。
- 数字：十进制、十六进制、二进制、浮点、指数。
- 字符、字符串、转义。
- 操作符最长匹配。
- `asm` 后可捕获原始 `{ ... }` 块。

### P3：Preprocessor

- 预处理指令：
  `#if / #ifdef / #ifndef / #els / #end / #inc / #def / #onc / #err / #wrn / #assert`。
- 宏值类型：`int64_t` 或 `std::string`。
- 内置 PP 表达式求值器，支持 `defined()`、逻辑、比较、算术。
- 路径规范化、循环 include 检测、`#onc` 只包含一次。
- 系统头路径硬编码 `/usr/local/include`、`/usr/include`，Windows 下未必适用。

### P4：AST

- `Expr` 多态表达式节点。
- `Stmt` 多态语句节点。
- `Pattern` 模式节点。
- 声明节点：
  `Function / RecordDecl / EnumDecl / UnionDecl / SigDecl / TraitDecl / TypDef / ClassDecl / ModuleDecl / SextStmt / TopLevel / Program`。
- `Program` 保存函数、编译期函数、记录、枚举、联合、类、特质、模块、类型别名、常量、use 别名、全局变量、入口等注册表。

### P5：Parser 上

- 顶层分发：`fun / cte fun / ent / cor / rec / enm / uni / sig / tra / typ / cls / fnl cls / mod / sext / use / pkg / lod / upd / exp / ext / equ / var / attr`。
- 类型解析：基础类型、指针、拥有指针、数组、map、opt、sig、rng、slc、tup、tck、rlt、函数类型、泛型实例化。
- 函数、入口、协程、记录、枚举、联合、签名、特质、类型别名、类、模块。
- 类继承验证、构造函数重载参数数唯一、访问块、嵌套类、友元、抽象/虚/终态方法。
- `sext` 元编程骨架。
- `use / pkg / lod / upd / exp / ext / equ / 全局变量 / attr`。

### P6：Parser 中

- `parseSuite()`：缩进块解析。
- 语句解析：
  `var / reg var / if / whl / for / swt / ret / prn / put / epu / epr / get / new / del / asm / unw / inc / dec / mov / set / swp / brk / cnt / jmp / label / atr / attr / 表达式语句 / 赋值`。
- `for` 支持范围、for-each、C 风格。
- `swt` 支持 `cas`、`def`、模式、guard。
- 模式解析：通配、绑定、字面量、枚举、记录、元组、别名。

### P7：Parser 下 + cte/sext

- 表达式优先级：
  管道、`||`/`or`、`&&`、相等、关系、加减、乘除、幂、一元、后缀、primary。
- 表达式种类：
  字面量、标识符、二元、一元、命名指令、调用、方法调用、`cal`、索引、切片、成员、元组索引、`.has`、`.val`、`as`、`ok`、`er`、`try`、`unw`、`or`、数组、元组、记录、类、枚举、范围、`sizeof`、`sel`、`blk`、`lam`、`fmt`、管道、`rse`、`cte`。
- `tryEvalConst()`：编译期常量求值。
- `evalCteCall()`：编译期函数最小执行。
- `execSextBlock()` / `execSextStmt()`：sext 最小执行。

### P8：运行时

- `Value`：`std::variant`，可容纳：
  `monostate / int64_t / double / bool / char / string / Array / Object / Map / Closure / Rlt / Range / Coroutine / Opt / Slice / Ptr`。
- `Env`：作用域链，支持 `moved / consts / owned / noRelease`。
- `Array`、`Object`、`MapValue`、`Closure`、`RltValue`、`RangeValue`、`OptValue`、`SliceValue`、`PtrValue`、`Coroutine`。
- 默认值 `defaultOfType()`。
- 字符串化、相等比较、truthy、MapKey 转换。
- 对象字段用 `std::map<std::string, Value>`。

### P9：解释器表达式求值

- 字面量、变量、二元、一元。
- 命名指令：`add/sub/mul/div/mod/pow/neg/abs/min/max/shl/shr/lsr/and/or/xor/not/equ/neq/grt/lss/geq/leq/sel`。
- 调用、方法调用、`cal`。
- 索引、切片、成员、元组索引。
- `.has`、`.val`。
- `as` 转换，返回 `opt`。
- `ok / er / try / unw / or`。
- 数组、元组、记录、类、枚举、范围。
- `sizeof`、`sel`、`blk`、`lam`、`fmt`、管道、`rse`。
- 类方法查找、trait 运算符重载雏形。

### P10：解释器语句执行 + 协程

- 语句执行：
  `var / assign / mov / set / inc / dec / swp / expr / if / while / for-range / for-each / for-c / switch / break / continue / jmp / label / ret / print / get / new / del / atr / attr / asm / unw / block`。
- 模式匹配 `matchPattern()`。
- `destroyOwnedInScope()`：作用域退出时调用 `fin`。
- `callFunction()`：默认参数、返回值、`try` 传播。
- 协程：
  - `startCoroutine()` 创建协程。
  - `resumeCoroutine()` 状态机。
  - `CoFrame` 栈保存块、while、for-range、for-c 状态。
  - `atr` 挂起，`rse` 恢复。
  - 协程内支持部分语句，不支持 `jmp/label`、for-each 等。

### P11：run + main

- `Interpreter::run()`：
  - 建立全局 `Env`。
  - 为模块建立模块 `Env`。
  - 注册函数闭包、编译期函数、模块成员、use 别名。
  - 初始化全局变量、常量。
  - 调用入口 `ent`。
  - 返回值转为进程退出码。
- `main()`：
  - 解析命令行。
  - 读文件。
  - 预处理。
  - 词法分析。
  - 语法分析。
  - 可选打印 token / AST 摘要。
  - 选择后端：解释器或 NASM。
  - 调用后端 `compile()`。

### P12：Target / Backend / NASM PoC

- `Target` 抽象：
  - `LinuxX64Target`
  - `WinX64Target`
- 调用约定信息：
  - 整数参数寄存器。
  - 浮点参数寄存器。
  - 返回类型分类。
  - callee-saved 寄存器。
  - 栈对齐、shadow space。
  - 汇编器、链接器参数。
- `Backend` 抽象：
  - `InterpreterBackend`：直接调用解释器。
  - `NasmBackend`：生成 NASM 汇编。
- NASM 后端：
  - Linux：`_start` + syscall。
  - Windows：`mainCRTStartup` + `printf/scanf/exit`。
  - 支持部分表达式、语句、打印、字符串缓冲、整数/浮点输出、读取。
  - 仅为 PoC，覆盖范围有限，未验证。

---

## 五、补充条款：解释执行能力与游玩指南

### A.1 为什么算优点

此前声明侧重“未编译、未验证、NASM 后端风险高”。现补充如下：

`arc.cpp` 内建 `InterpreterBackend`，默认 `--emit=interp`。你不需要 NASM、ld、lld-link、调用约定、栈对齐、shadow space 这些东西，就可以直接跑 `.arc` 源码。解释器覆盖了表达式求值、语句执行、作用域、函数、闭包、数组、map、字符串、记录、枚举、类、opt/rlt、协程、预处理、模块/use、sext/cte 最小执行等路径。虽然仍未经任何编译验证，但它是当前最完整的执行路径。换句话说：NASM 后端是附加风险项，解释器才是主玩法。

| 项目 | 解释器后端 | NASM 后端 |
|---|---|---|
| 是否需要汇编器 | 否 | 是 |
| 是否需要链接器 | 否 | 是 |
| 是否默认 | 是 | 否 |
| 可玩性 | 高 | 低，PoC |
| 覆盖范围 | 较完整 | 有限 |
| 适合用途 | 玩语言、测语法、跑例子 | 看汇编生成骨架 |
| 风险 | 未验证 | 未验证且更甚 |

所以，接手后建议先玩解释器，最后再碰 NASM。

### A.2 运行方式

写一个 `test.arc`：

```arc
ent main:
    prn "hello arc"
```

然后：

```bash
arc.exe test.arc
```

默认就是解释执行。也可显式指定：

```bash
arc.exe --emit=interp test.arc
```

常用调试选项：

```bash
arc.exe --tokens test.arc      ; 只跑词法，打印 token
arc.exe --ast test.arc         ; 解析后打印程序结构摘要
arc.exe -v test.arc            ; 显示各阶段进度
arc.exe --no-pp test.arc       ; 跳过预处理
```

注意：`--emit=nasm` 只输出 NASM 汇编文本，不会自动汇编链接。解释器会忽略 `-o` 和 `--target` 的实际后端影响，但 `main` 仍会校验 target 名称。

### A.3 最小可玩样例

```arc
ent main:
    prn "hello arc"
```

运行：

```bash
arc.exe test.arc
```

理论输出：

```text
hello arc
```

但请记住：未验证。第一次运行可能直接成功，也可能坍缩成一堆报错。

### A.4 稍完整样例

```arc
#def N 3

fun add(a: int, b: int): int:
    ret a + b

ent main:
    prn "hello arc"

    var x: int
    x = 41
    prn add(x, 1)

    var a: arr<int, 3>
    a = [1, 2, 3]
    for i in 0..<3:
        prn a[i]

    var s: str
    s = "x="
    prn fmt "{s}{x}"
```

运行：

```bash
arc.exe test.arc
```

理论输出：

```text
hello arc
42
1
2
3
x=41
```

注意：缩进必须用空格，不能用 tab。Lexer 明确禁止 tab 缩进。

### A.5 常用语法速览

| 功能 | 写法 |
|---|---|
| 入口 | `ent main:` |
| 函数 | `fun f(a: int): int:` 换行缩进 `ret ...` |
| 单表达式函数 | `fun f(a: int) => a + 1` |
| 变量 | `var x: int` |
| 赋值 | `x = 1` |
| 打印换行 | `prn "text"` |
| 打印不换行 | `put "text"` |
| 标准错误 | `epr "err"` / `epu "err"` |
| 条件 | `if cond:` / `els:` |
| 循环 | `whl cond:` |
| 范围 for | `for i in 0..<10:` |
| for-each | `for x in arr:` |
| 分支 | `swt x:` / `cas 1:` / `def:` |
| 数组 | `var a: arr<int, 3>`，`a = [1,2,3]`，`a[0]` |
| map | `var m: map<str, int>`，`m["k"] = 1` |
| 字符串 | `var s: str`，`s = "hi"` |
| 记录 | `rec P:` 字段；`rec P, .x = 1` |
| 枚举 | `enm E:` `cas A`；`enm E.A` |
| opt/rlt | `ok x`，`er e`，`try r`，`r or fallback`，`.has`，`.val` |
| 协程 | `cor g(): int:`，`atr x`，`rse h` |
| 管道 | `x |> f(y)` |
| 格式化 | `fmt "x={x}"` |
| 预处理 | `#def`，`#if`，`#inc` |
| 行注释 | `; comment` |
| 块注释 | `{* comment *}` |

### A.6 解释器相关代码位置

| 批次 | 内容 |
|---|---|
| P8 | 运行时 `Value / Env / Object / Array / Map / Opt / Rlt` |
| P9 | 解释器表达式求值 `evalExpr` 等 |
| P10 | 解释器语句执行 + 协程状态机 |
| P11 | `Interpreter::run()` + `main` 驱动 |
| P12 | `InterpreterBackend`，默认解释执行 |

也就是说，解释器不是附属品，而是主干。NASM 后端反而更像后来挂上的实验分支。

### A.7 推荐玩法路线

1. 编译 `arc.cpp`，先只求 `--tokens` 能跑。
2. 写 `ent main:` + `prn "hello"`，跑解释器。
3. 玩变量、算术、字符串、`prn` / `put`。
4. 玩 `if` / `whl` / `for` / `swt`。
5. 玩函数、递归、默认参数。
6. 玩数组、map、字符串拼接、`fmt`。
7. 玩记录、枚举、类。
8. 玩 `ok` / `er` / `try` / `unw` / `or`。
9. 玩协程 `cor` / `atr` / `rse`。
10. 玩预处理、模块、`use`、`sext`、`cte`。
11. 最后再碰 `--emit=nasm`。

---

## 六、已知风险与未验证项

1. **整体未编译。**  
   语法错误、链接错误、运行期错误均未排除。

2. **AI 注释不可全信。**  
   注释可能描述的是意图，而非实际行为。

3. **NASM 后端风险最高。**  
   栈帧计算、局部变量统计、for-each/switch 临时槽、浮点调用、字符串生命周期、数组参数、调用约定等均未验证。

4. **解释器与 NASM 后端语义可能不一致。**  
   两套执行路径未必等价。

5. **高级特性多为骨架或最小实现。**  
   类、泛型、特质、sext、cte、模块导出、use 别名等未充分验证。

6. **预处理器平台差异。**  
   系统头路径硬编码为 Unix 风格，Windows 下可能不可用。

7. **协程状态机未验证。**  
   `CoFrame` 栈、yield/resume、break/continue 处理均未测试。

8. **命令行与后端耦合。**  
   `--emit=nasm` 只输出汇编，不自动汇编链接。

9. **重复包含 `<variant>`。**  
   无害，但显示代码整理痕迹。

10. **解释器虽较完整，但类型检查很弱。**  
    很多地方靠运行期值决定，类型错误可能延迟到运行期才炸。

---

## 七、接手建议

1. **先做语法检查：**

```bash
g++ -std=c++17 -fsyntax-only arc.cpp
```

2. **再编译，不优化，带警告：**

```bash
g++ -std=c++17 -O0 -g -Wall -Wextra arc.cpp -o arc.exe
```

3. **从最小程序开始：**

```arc
ent main:
    prn "hello"
```

4. **先跑解释器后端：**

```bash
arc.exe --tokens test.arc
arc.exe --ast test.arc
arc.exe test.arc
```

5. **再测预处理、模块、类、协程、sext、cte。**

6. **NASM 后端放最后。**  
   先确认解释器语义，再对齐汇编后端。

7. **建议路线：**  
   编译通过 → 词法通过 → 语法通过 → 最小解释执行 → 逐步扩展特性 → NASM PoC。

---

## 八、最小冒烟测试样例

```arc
ent main:
    prn "hello"
```

运行：

```bash
arc.exe test.arc
```

预期：理论上输出 `hello` 并换行。  
但请注意：这只是理论预期，未经验证。

---

## 九、结语

以上。可循“编译—报错—修正”之迭代路径渐进推进。其可执行性近乎观测前之叠加态；坍缩于你第一次成功构建，或第一次报错。

`arc.cpp` 当前最实际的玩法，不是拿 NASM 去硬编，而是把它当解释型语言直接跑。先编译，再 hello，再逐步扩展。源码在 `arc.cpp`。默认解释执行。注释由 VSCode AI 生成，别全信。编译环境为 MinGW-w64 GCC 16.2.0，理论层面存在运行之可能。

先玩 `ent main:`。  
祝编译顺利。
