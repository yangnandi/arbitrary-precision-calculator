"""Independent oracle for BigCalc's test expressions.

Uses only Python's exact int / Fraction plus well-known decimal expansions
of pi and e, so it shares no code with the Java implementation.
"""

import math
import sys
from decimal import Decimal, ROUND_HALF_EVEN, getcontext
from fractions import Fraction

sys.set_int_max_str_digits(0)
getcontext().prec = 120

PI = ("3.14159265358979323846264338327950288419716939937510"
      "582097494459230781640628620899862803482534211706798214808651328230664709384460955058223172535940812848111745028410270193852110555964462294895493038196")
E = ("2.71828182845904523536028747135266249775724709369995"
     "957496696762772407663035354759457138217852516642742746639193200305992181741359662904357290033429526059563073813232862794349076323382988075319525101901")


def fib(n):
    def fd(k):
        if k == 0:
            return 0, 1
        a, b = fd(k >> 1)
        c = a * (2 * b - a)
        d = a * a + b * b
        return (d, c + d) if k & 1 else (c, d)
    return fd(n)[0]


def is_prime(n):
    if n < 2:
        return False
    for p in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % p == 0:
            return n == p
    d, s = n - 1, 0
    while d % 2 == 0:
        d //= 2
        s += 1
    for a in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(s - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


def next_prime(n):
    if n < 2:
        return 2
    c = n + 1
    while not is_prime(c):
        c += 1
    return c


def scaled_constant(text, digits):
    """Round a decimal constant to `digits` places, half-even, return Fraction."""
    return Fraction(Decimal(text).quantize(Decimal(1).scaleb(-digits), rounding=ROUND_HALF_EVEN))


def fmt(v):
    if isinstance(v, Fraction):
        return str(v.numerator) if v.denominator == 1 else f"{v.numerator}/{v.denominator}"
    return str(v)


F = Fraction
cases = [
    math.factorial(100),                     # 100!
    2 ** 1000,
    fib(1000),
    len(str(2 ** 1000000)),
    (2 ** 1000) % 97,
    math.gcd(123456789, 987654321),
    math.lcm(123456789, 987654321),
    math.isqrt(10 ** 100),
    2 ** (3 ** 2),
    2 ** math.factorial(3),
    (1 + 2) * (3 + 4),
    F(1, 3) + F(1, 6),
    F(10 ** 50, 3),
    F(2),                                    # 8^(1/3)
    F(2),                                    # root(64,6)
    F(-2),                                   # (-8)^(1/3)
    math.factorial(100),
    len(str(math.factorial(20000))),
    1 if is_prime(2 ** 127 - 1) else 0,
    1 if is_prime(2 ** 128 + 1) else 0,
    next_prime(10 ** 30),
    scaled_constant(PI, 50),
    scaled_constant(E, 50),
    F(math.isqrt(2 * 10 ** 100), 10 ** 50),  # sqrt(2, 50), truncated
    0b1010 + 0x10 + 0o17,
    F(5, 7) - F(1, 3) * ((F(5, 7) / F(1, 3)) // 1),
    1000000007 % 1000000009,
    F(1, 2 ** 10),
    F(10 ** 100, 7 ** 20),
]

for case in cases:
    print(fmt(case))
