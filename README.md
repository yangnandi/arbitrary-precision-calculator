# BigCalc — arbitrary-precision calculator (no digit limit)

> **English TL;DR** — An exact calculator with **no digit limit**, implemented twice:
> **Java 25** (`BigCalc.java`, on `java.math.BigInteger`) and **C++ / GMP**
> (`cpp/BigCalcGMP.cpp`, on `mpz_class` / `mpq_class`).
> Every value is a canonical exact rational, so `1/3` stays `1/3`, `2^1000000` is exact and
> `100000!` is exact. Irrational quantities (`pi(d)`, `e(d)`, `sqrt(x,d)`, `root(x,k,d)`) are
> produced only at a caller-chosen precision and are returned as exact rationals.
> Both implementations pass the same 29-case suite **byte-for-byte** against an independent
> Python oracle; on raw big-integer arithmetic the GMP build is **3.75–10.5× faster** than the
> Java one. Run with `java BigCalc.java` or build via `cpp/build.cmd`. Details below (Chinese).

---

## C++/GMP v2 — unrestricted sizes + streaming output

The C++ implementation no longer caps anything in software. Exponents, digit
counts, root indices and factorial arguments are carried as arbitrary-precision
integers end to end, and every decimal is written out most-significant-digit
first in chunks, flushed as it is produced.

```text
$ bigcalcgmp.exe
> 2^9999999999999999999999999999999999999999999999
error: (2)^(9999999999999999999999999999999999999999999999) would need about
3010299956639811952137388947244930267681898815 decimal digits (1.249e45 bytes of
binary value, before any output buffer), but this machine currently has 23.4 GiB of
free RAM. Nothing in the calculator caps this -- it is the machine.
Use :guard off to attempt it anyway.
```

That is the point: the 46-digit exponent is **accepted**, sized exactly up front,
and the only thing that refuses it is physics. Anything that does fit now starts
scrolling immediately instead of appearing only after the whole decimal string
has been materialised:

```text
> 2^100000000
  [  5.9%  1763849 / 30103000 digits]        <- stderr progress
3684665936980458763209092390984221915069...   <- stdout, streaming
```

### What changed

| Area | Before | v2 |
| --- | --- | --- |
| Exponent of `^` | had to fit `long` ("too large to materialise") | `mpz_class`, square-and-multiply past `unsigned long` |
| `:digits N` | `int`, rejected above 10 000 000 | arbitrary precision; expansion streamed by long division |
| `root(x,k,d)`, `pi(d)`, `e(d)` | `int` digits | arbitrary precision, checked against RAM only |
| `fact(n)`, `fib(n)` | rejected past `unsigned long` | sized first, refused only when it cannot fit |
| Output | `get_str()`, whole result in memory | divide-and-conquer MSB-first emitter, `fwrite`+flush per chunk |
| Peak extra memory | one full copy of the result as ASCII (~2.4x the binary value) | one 64 Ki-digit chunk |
| Long jobs | silent | progress meter on stderr above 2 000 000 digits |
| OOM | `abort()` / Windows crash dialog | clean message and `exit 2` |

### New commands

```text
:guard on|off     RAM feasibility pre-check (default on). "off" attempts anything.
:progress on|off  streaming progress meter on stderr (default on)
:mem              free RAM, how many digits fit, current settings, ans
```

### Verification

* 29-case Python-oracle suite: **0 / 29** failures, unchanged.
* Byte-for-byte against CPython: `2^100000000` (30 103 000 digits),
  `2^1000000` (301 030), `fact(100000)` (456 574), `1/7` to 100 000 decimals.
* 599 decimal/rounding outputs (`:digits` 0…500, `:frac on|off`, 23 fractions
  including `9999/10000`, `-1/8`, `1/1024`, `1/125`, `5/9`, `-7/12`) against an
  independent half-up reference: **0 mismatches**.

---

## C++/GMP v3 — multithreading, and an honest look at the GPU

### What v2 got wrong, and what the profiler said

v3 first added threads and a CUDA kernel, and both "worked" while barely changing
anything. Measuring instead of guessing found two separate mistakes.

**1. The decimal renderer was the bottleneck, and it was serial.** v2 replaced the
original exact-decimal writer with a block long division, to avoid materialising the
scaled numerator. That costs O(scale·M(n)) instead of O(M(n)) and pinned one core:
for `pi(1000000)` it was **3.3 of the 4.2 seconds**. One multiply plus one division
restores O(M(n)), and the resulting integer goes through the parallel digit streamer
like any other:

| | before | after | |
| --- | ---: | ---: | ---: |
| `pi(1000000)` | 4 208 ms | **1 114 ms** | **3.8×** |
| `pi(200000)` | 280 ms | **171 ms** | 1.6× |

**2. Parallelising only the leaves of the conversion was nearly pointless.** The
recursive splitter recomputed `10^h` from scratch at every node — ~900 ms of serial
work for a 5.5-million-digit factorial, next to just 126 ms of parallel leaf
conversion. The splitter now computes each level's power of ten once, computes all
the levels' powers in parallel, and runs each level's divisions in parallel too.

Program-reported parallel fraction, 44 threads, before → after:

```
fact(1000000)   10.3% -> 48.0%      2^100000000    2.0% -> 44.1%
2^50000000       2.0% -> 42.3%      e(1000000)     2.3% ->  2.6%
```

### Multithreading, measured

Xeon E5-2696 v4, 22 physical / 44 logical cores:

| operation | 1 thread | 44 threads | speedup | parallel fraction |
| --- | ---: | ---: | ---: | ---: |
| `fact(10⁶)` | 1 823 ms | **752 ms** | **2.42×** | 48.0% |
| `2^10⁸` (compute + render) | 9 497 ms | **3 576 ms** | **2.66×** | 44.1% |
| `2^5·10⁷` | 4 125 ms | **1 699 ms** | **2.43×** | 42.3× |
| `e(10⁶)` | 14 771 ms | **5 845 ms** | **2.53×** | 2.6% |
| `pi(10⁶)` | 1 444 ms | **1 114 ms** | 1.30× | 7.7% |
| `pi(2·10⁵)` | 184 ms | 171 ms | 1.08× | 18.7% |

**Why `pi` stays near one core, honestly.** After the render fix, 86% of
`pi(1000000)` is four *single* GMP operations that cannot overlap with anything:
`10^1000015`, a 2 000 000-digit `mpz_sqrt`, and a 2 000 000-by-1 000 000 division.
There is no second thread to give them work. `e` scales best because its per-block
work is genuinely independent — and part of that 2.53× is algorithmic, not threading:
the parallel path evaluates `Σ hi!/j!` per block with a product tree instead of a
running product.

### GPU (CUDA) — what it can and cannot do here

An RTX 5060 Ti, driven **without the CUDA runtime**: nvcc compiles
`gpu_screen.cu` to PTX, `ptx_to_header.py` embeds it as a C string, and the
executable resolves `nvcuda.dll` at run time and JITs it through the Driver API.
Still one MinGW-built binary, no import library, no MSVC-ABI DLL, and it runs
unchanged on machines with no CUDA Toolkit.

**It cannot accelerate the arithmetic, and this is not a matter of effort.** The hot
path of every operation here is a chain of enormous multiplications and divisions.
GMP has a hand-tuned FFT for those; on a GPU that workload is latency- and
bandwidth-bound, not throughput-bound. Making `fact`/`pi`/`2^n` faster on the GPU
would need a parallel big-integer multiplier — a research-grade component.

**What it does accelerate** is the one embarrassingly parallel shape in the program:
one huge number against a very large table of small primes, `n mod p` for each,
nothing flowing between them. That backs `smallfactor(n[, limit])`:

| limit | GPU | 44-thread CPU | speedup |
| --- | ---: | ---: | ---: |
| 10⁶ | 1.24 ms | 9.17 ms | **7.4×** |
| 10⁷ | 2.34 ms | 10.15 ms | **4.3×** |
| 10⁸ | 8.43 ms | 23.09 ms | **2.7×** |

**What it does not accelerate, and I am not shipping it as if it did.** A GPU
candidate-window sieve for `nextprime` was written, verified to return identical
results, and then **rejected on measurement** — it loses to `mpz_nextprime` at every
size tried, while the kernel itself contributes under a millisecond:

| n | GPU window sieve | `mpz_nextprime` |
| --- | ---: | ---: |
| 10³⁰⁰ | 259 ms | **14 ms** |
| 10²⁰⁰⁰ | 14.7 s | **13.5 s** |
| 10⁸⁰⁰⁰ | 516 s | **402 s** |

The cost is Miller-Rabin on the survivors, not the small-prime screen, and GMP
already screens candidates well. It stays behind `:gpu nextprime on` for
experiments, off by default.

`smallfactor` cross-checked against an independent Python reference on 71 inputs
(including `2^521−1`, `2^127−1`, `2^89−1`, `10^200+357`): **0 mismatches**.

### Commands

```text
:threads N            worker threads (no arg = show, 0 = auto)
:gpu                  on|off|info|nextprime|bench [limit]
:time                 now also reports the parallel fraction and GPU kernel ms
```

`:time` output looks like this, so the split is never a mystery:

```
  [752.3 ms  | parallel regions: 15 calls, 481 tasks, 359.2 ms wall on 44
   threads = 48.0% of the run  | GPU kernels: 0.0 ms]
```

### Verification (after a clean `build.cmd`)

| Check | Result |
| --- | --- |
| 29-case Python-oracle suite | 0 / 29 |
| 599 decimal/rounding outputs vs an independent half-up reference | 0 mismatches |
| `smallfactor` vs an independent Python reference, 71 inputs | 0 mismatches |
| `2^1000000`, `2^10000000`, `fact(1000000)` vs CPython | byte-for-byte |
| compiler warnings with `-Wall -Wextra` | 0 |

---

用 **Java 25** 实现的任意精度计算器。所有数值都是基于 `BigInteger` 的**精确有理数**（分子/分母，始终约分、分母恒正），因此：

- **没有任何位数限制**，也没有任何舍入误差；
- `1/3` 就是三分之一，不是 `0.333…`；
- `2^1000000`、`100000!`、`fib(10000000)` 都是精确值；
- 无理量（π、e、开不尽方）只在你**显式指定精度**时产生，且结果仍是精确有理数。

## 为什么选 Java

先做了同口径基准测试（JDK 25 vs CPython 3.13.15，取 3 次最小值）：

| 大整数运算 | Java `BigInteger` | Python `int` | 结果 |
| --- | --- | --- | --- |
| 50 万位 × 50 万位乘法 | **88.9 ms** | 266.1 ms | Java 快 **3.0×** |
| `fib(300000)` | **6.0 ms** | 12.5 ms | Java 快 **2.1×** |
| 23.8 万位整数转字符串 | **105.8 ms** | 120.9 ms | Java 快 1.14× |
| `factorial(50000)` | 847 ms | 824 ms | 基本持平 |

决定性因素是第二条**硬性限制**：

```python
>>> str(3 ** 500000)
ValueError: Exceeds the limit (4300 digits) for integer string conversion
```

CPython 3.11+ 出于防 DoS 的考虑，默认把整数与字符串之间的转换限制在 4300 位，必须调用 `sys.set_int_max_str_digits(0)` 才能绕过。这不是"没有数位限制"，而是"默认有限制、需要手动解除"。Java 的 `BigInteger` 不存在此类人为上限。

## 为什么不是 C++

这是本文件里最值得记录的一个决策，因为它和"直觉"相反。

**先看事实：这台机器的 C++ 工具链里没有任何任意精度库。**

```
C:\mingw64\include\gmp.h                      -> 不存在
C:\mingw64\include\boost\multiprecision       -> 不存在
C:\mingw64\lib\libgmp.a                       -> 不存在
pacman / vcpkg / conan                        -> 未安装
winget search gmp                             -> 无结果
C++ 标准库（含 C++23）                          -> 没有任意精度整数类型
```

所以 C++ 路线只有两种走法，而两种都不理想。

**走法一：不用库，手写 bignum。** 这与"最高效"直接矛盾。实测（`bench/Scale.java` 与 `bench/CppScale.cpp`，均取 3 次最小值）乘法耗时的规模指数：

| 实现 | 算法 | 拟合指数 | 20 万位 | 80 万位 | 160 万位 |
| --- | --- | --- | --- | --- | --- |
| C++ 手写 schoolbook（`-O2`） | O(n²) | **2.004** | 520 ms | ~8.3 s（外推） | ~33 s（外推） |
| Java `BigInteger` | Toom-Cook 3 | **1.481** | **18.4 ms** | **135 ms** | **400 ms** |

同样是 20 万位乘法，**手写 C++ 比 Java 慢 28 倍**；到 160 万位差距扩大到约 83 倍。原因很简单：`BigInteger` 已经内置了 Karatsuba 与 Toom-Cook 3，而手写 schoolbook 是平方复杂度——**在任意精度计算里，算法选择比语言快慢重要一到两个数量级**。要在 C++ 里追平，就得自己实现 Karatsuba + Toom-Cook，而那正是内存安全问题最密集的地方。

**走法二：装 GMP。** 这才是 C++ 真正的最优解，而且**我必须承认：C++ + GMP 会赢**。GMP 的 `mpz` 在大操作数上使用 FFT/Schönhage-Strassen，乘法指数约 1.0–1.1，百万位以上通常比 `BigInteger` 快一个数量级，且它的进制转换同样是次平方复杂度。但代价是：GMP 没有官方 Windows 二进制，winget 里也没有，需要先装 MSYS2 或 vcpkg 再从源码构建。

**结论**：C++ 的速度优势来自**库**，而不是语言本身。在"用已安装的语言、且不额外引入依赖"这个前提下：

- C++ 手写 bignum → 比 Java 慢 28–83 倍；
- C++ + GMP → 最快，但需要额外安装并构建依赖；
- Java `BigInteger` → **零依赖、JDK 自带、已是 Toom-Cook 3**，是当前约束下的最优解。

> 补充一句公道话：如果拿**相同算法**对比（C++ 手写 Toom-Cook 3 vs Java），C++ 大概能快 1.5–2 倍——省掉边界检查、对象分配和 `BigInteger` 的不可变拷贝。但这点收益远小于"有没有库"带来的差别。

## C++ + GMP 版本（已实现，结论被实测推翻）

上面的推理停留在"没有 GMP"的前提。装上 GMP 之后，C++ 版本确实赢了，而且赢得比预期多——**这正是当初把 GMP 列为"真正最优解"的原因**。

### 安装与踩坑

```powershell
winget install --id MSYS2.MSYS2 -e --scope machine          # 需要管理员
C:\msys64\usr\bin\bash.exe -lc "pacman-key --init && pacman-key --populate msys2"
C:\msys64\usr\bin\bash.exe -lc "pacman -Syu --noconfirm"
C:\msys64\usr\bin\bash.exe -lc "pacman -S --noconfirm --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-gmp"
```

三个必须记录的坑：

1. **主镜像可能慢到不可用。** `repo.msys2.org` 实测吞吐 **0.01 MB/s**（46 MB 的 GCC 包要下约 76 分钟，表现就是"卡死"——`.part` 文件长时间零增长），而 USTC 中科大 **79.5 MB/s**、清华 TUNA **18.8 MB/s**。注意：MSYS2 默认的 `etc/pacman.d/mirrorlist.*` **本来就列有 USTC 和 TUNA**（在 Tier 1 段），只是排在主镜像之后。本项目把这两个国内镜像提到了最前面，`pacman -Sy` 从卡死变成 **4 秒**完成。
2. **g++ 必须能找到 `C:\msys64\mingw64\bin`。** `cc1plus.exe` 位于 `lib\gcc\...`，却依赖 `mingw64\bin` 下的 `libgmp-10.dll`、`libmpfr-6.dll`、`libmpc-3.dll`、`libisl-*.dll`。该目录不在 PATH 时子进程**静默启动失败**：`g++` 退出码 1、零输出、零报错，连 `int main(){}` 都编不过。`cpp/build.cmd` 里已固定处理。
3. **`mpq_class(num, den)` 不会自动约分。** 必须显式调用 `canonicalize()`，否则 `get_den() != 1`，整数会被误判为分数、输出与 Java 版不一致。这是对照测试抓出来的真实缺陷。

### 构建与运行

```powershell
cd bigcalc\cpp
.\build.cmd                    # 或用：C:\msys64\mingw64\bin\g++.exe -O2 -std=c++20 -static -o bigcalcgmp.exe BigCalcGMP.cpp -lgmpxx -lgmp
.\bigcalcgmp.exe               # 静态链接，3.2 MB，不依赖 MSYS2 运行库
```

### 正确性

同一套 `tests/exprs.txt` + `tests/oracle.py`，逐字节比对：

```
===== C++/GMP failures: 0 / 29 =====
```

## 运行

```powershell
cd C:\Users\minec\Documents\deepseek-harness\default-workspace\bigcalc

java BigCalc.java          # 单文件源码模式，无需编译
# 或先编译再运行（长期使用启动更快）：
javac -encoding utf8 BigCalc.java
java BigCalc
```

也可以直接运行 `run.cmd`。

## 语法

| 类别 | 内容 |
| --- | --- |
| 运算符 | `+ - * / % ^ !` 与括号；`^` 右结合，`!` 是后缀阶乘 |
| 取模 | `a % b` = `a - b*floor(a/b)`，对有理数同样成立 |
| 字面量 | 十进制、`0x` 十六进制、`0b` 二进制、`0o` 八进制、`_` 分隔符 |
| 变量 | `x = 表达式`；`ans` 是上一个结果 |
| 精确函数 | `abs floor ceil trunc round sign gcd lcm min max` |
| | `fact fib isqrt ndigits isprime nextprime pow root` |
| 指定精度 | `sqrt(x, d)`、`root(x, k, d)`、`pi(d)`、`e(d)` |
| 无理数检查 | `sqrt(2)` 会**报错**并提示改用 `sqrt(2, 50)`，不会偷偷给你浮点数 |

命令：

```
:digits N    非整数显示多少位小数（默认 30，0 表示只显示精确分数）
:frac        强制用 n/d 显示非整数（默认自动：十进制可终止时直接显示精确小数）
:base N      整数输出进制：10 / 16 / 8 / 2（也可用 :hex :dec :bin :oct）
:vars        列出变量
:time        开关计时
:help        帮助
:quit        退出
```

## 示例

```
> 2^100
1267650600228229401496703205376

> 100!
93326215443944152681699238856266700490715968264381621468592963895217599993229915608941463976156518286253697920827223758251185210916864000000000000000000000000

> 1/3 + 1/6
1/2

> sqrt(2)
error: sqrt(x) is irrational here; use sqrt(x, digits)

> pi(50)
3.14159265358979323846264338327950288419716939937511

> fib(1000)
43466557686937456435688527675040625802564660517371780402481729089536555417949051890403879840079255169295922593080322634775209689623239873322471161642996440906533187938298969649928516003704476137795166849228875

> ndigits(2^1000000)
301030

> sqrt(2, 50)
1.41421356237309504880168872420969807856967187537694
```

## 实现要点（为什么快）

| 运算 | 算法 |
| --- | --- |
| 阶乘 | **二分拆分**（乘积树），而非朴素循环 |
| 斐波那契 | **快速倍增法**（迭代版，避免递归开销） |
| 整数开方 | `BigInteger.sqrt()`（内部牛顿迭代） |
| n 次方根 | 牛顿迭代 + 精确性校验 |
| π | **Chudnovsky + 二分拆分**，每项约 14.18 位有效数字 |
| e | 1/k! 级数，`BigDecimal` 高精度累加 |
| 分数约分 | `BigInteger.gcd` |
| 输出 | 1 MB 缓冲流，百万位结果一次写出 |

## 正确性验证

`tests/oracle.py` 是一个**完全独立**的参照实现（只用 Python 的 `int` / `Fraction` 以及 π、e 的已知展开），`tests/exprs.txt` 是 29 条测试表达式，逐字节比对结果：

```
===== failures: 0 / 29 =====
```

覆盖：`100!`、`2^1000`、`fib(1000)`、`2^1000000` 的位数、大数取模、`gcd`/`lcm`、`isqrt(10^100)`、右结合 `2^3^2`、`2^3!`、有理数四则与取模、负数的奇次根 `(-8)^(1/3)`、`fact(20000)` 的位数、`isprime(2^127-1)`、`nextprime(10^30)`、`pi(50)`、`e(50)`、`sqrt(2,50)`、进制字面量混算等。

复跑：

```powershell
cd bigcalc
@(':frac on', ':digits 0') + (Get-Content tests\exprs.txt) | java BigCalc > tests\actual.txt
python tests\oracle.py > tests\expected.txt
Compare-Object (Get-Content tests\expected.txt) ((Get-Content tests\actual.txt)[2..99])
```

## 性能实测

`ndigits(...)` 口径（包含把结果转成十进制字符串的开销）：

| 运算 | 结果位数 | BigCalc (Java) | Python 3.13 |
| --- | --- | --- | --- |
| `fact(100000)` | 456,574 | 572 ms | 517 ms |
| `fact(300000)` | 1,512,852 | 2,393 ms | 2,557 ms |
| `fib(1000000)` | 208,988 | 130 ms | 189 ms |
| `fib(10000000)` | 2,089,877 | 3,842 ms | 5,020 ms |
| `3^1000000` | 477,122 | 321 ms | 377 ms |
| `7^1000000` | 845,099 | 790 ms | 796 ms |

π(2000) + e(2000) + π(10000) 合计 **680 ms**（含 JVM 启动）。

> 说明：纯大数乘法的差距是 3 倍（见上表），但把结果转成十进制字符串这一步在两边都是接近平方复杂度的瓶颈，所以端到端口径的差距被拉平到 1.1–1.5 倍。要真正拉开差距，应避免对超长结果做十进制转换。

### Java vs C++/GMP 正面基准

同一台机器、同一套表达式，计时取自两个程序各自的 `:time`（不含进程启动）。

**算法口径对齐的纯算术**（阶乘都用二分拆分、fib 都用快速倍增；末尾 `% 1000000007` 只为把输出变小，不改变计算量）：

| 运算 | Java | C++/GMP | 加速 |
| --- | --- | --- | --- |
| `fact(100000)` | 270.6 ms | **52.6 ms** | 5.14× |
| `fact(300000)` | 633.9 ms | **169.1 ms** | 3.75× |
| `fib(1000000)` | 58.6 ms | **8.9 ms** | 6.58× |
| `fib(10000000)` | 970.0 ms | **92.4 ms** | 10.50× |
| `3^1000000` | 31.9 ms | **4.6 ms** | 6.93× |
| `7^1000000` | 72.6 ms | **8.7 ms** | 8.34× |

**高精度常数**：

| 运算 | Java | C++/GMP | 加速 |
| --- | --- | --- | --- |
| `pi(2000)` | 72.0 ms | **2.9 ms** | 24.83× |
| `pi(10000)` | 260.8 ms | **53.9 ms** | 4.84× |
| `pi(50000)` | 5,388.7 ms | **1,004.5 ms** | 5.36× |
| `e(2000)` | 173.4 ms | **2.5 ms** | 69.36× |
| `e(10000)` | 2,886.9 ms | **53.8 ms** | 53.66× |

> π 两边都是 Chudnovsky + 二分拆分，属同算法对比。e 的差距更大，是因为 Java 版用 `BigDecimal` 级数累加，而 C++ 版改用精确整数 `A/N!`（其中 `A = Σ N!/k!`），实现本身也更占优。

**十进制位数统计（`ndigits`）**：

| 运算 | Java | C++/GMP | 加速 |
| --- | --- | --- | --- |
| `ndigits(fact(300000))` | 1,973.1 ms | **168.2 ms** | 11.73× |
| `ndigits(fib(10000000))` | 3,156.4 ms | **85.2 ms** | 37.05× |
| `ndigits(7^1000000)` | 594.3 ms | **13.3 ms** | 44.68× |

> ⚠️ 这一组**不是同算法对比**：Java 走 `toString()`（真做进制转换），C++ 用 GMP 的 `mpz_sizeinbase()`（不生成十进制串）。它体现的是"GMP 提供了更省事的原语"，不能当作纯语言速度差。

**结论修正**：此前"同算法下 C++ 只快 1.5–2 倍"的估计过于保守。实测：

- 有 GMP 时，纯大数算术快 **3.75–10.5 倍**，高精度常数快 **5–69 倍**；
- 没有 GMP、只能手写 schoolbook 时，反而慢 **28–83 倍**（见上一节）。

差距的核心依然是：**算法与库的选择，在量级上压过语言本身。**


## 已知限制

- **超越函数未实现**：`ln`/`exp`/`sin`/`cos`/`tan` 会明确报错。这是刻意的设计选择——本计算器的前提是"精确"，引入浮点会破坏这个前提。目前只提供 `pi(d)`、`e(d)` 以及开方三种可显式指定精度的无理量。
- `sqrt(x,d)` / `root(x,k,d)` 的结果是**截断**（floor）而非四舍五入。
- 阶乘/幂的规模最终受内存与时间限制（例如 `10^9!` 不现实），但代码里没有任何人为位数上限。
- 每行一条表达式，不支持多行或脚本文件。

## 文件

```
bigcalc/
├── BigCalc.java         Java 实现（约 700 行，单文件，无第三方依赖）
├── BigCalc.class        编译产物（可直接 java BigCalc）
├── run.cmd              便捷启动
├── README.md            本文档
├── cpp/
│   ├── BigCalcGMP.cpp   C++/GMP 实现（与 Java 版功能对等）
│   ├── bigcalcgmp.exe   静态链接产物（3.2 MB，不依赖 MSYS2 运行库）
│   └── build.cmd        构建脚本（内含 MSYS2 PATH 处理）
├── bench/
│   ├── Bench.java       Java 大整数基准
│   ├── bench.py         Python 同口径基准
│   ├── Scale.java       Java 乘法规模指数拟合
│   └── CppScale.cpp     C++ schoolbook 规模指数拟合
└── tests/
    ├── exprs.txt        29 条测试表达式
    ├── oracle.py        独立参照实现
    ├── actual.txt       Java 实际输出
    ├── actual_cpp.txt   C++/GMP 实际输出
    ├── expected.txt     参照输出
    ├── perf_arith.txt   算术基准输入
    ├── perf_convert.txt 进制统计基准输入
    └── perf_pi.txt      π/e 基准输入
```
