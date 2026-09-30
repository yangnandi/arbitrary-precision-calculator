import sys
import time

# CPython >= 3.11 limits int <-> str conversion to 4300 digits by default.
sys.set_int_max_str_digits(0)


def bench(name, fn, n=3):
    best = None
    for _ in range(n):
        t = time.perf_counter()
        fn()
        d = time.perf_counter() - t
        best = d if best is None else min(best, d)
    print(f"{name:<34} {best * 1000:9.1f} ms")


a = 10 ** 500000 + 7
b = 10 ** 500000 + 3
bench("mul 500k x 500k digit", lambda: a * b)


def fact(n):
    r = 1
    for i in range(2, n + 1):
        r *= i
    return r


bench("factorial(50000)", lambda: fact(50000))


def fib(n):
    def fd(k):
        if k == 0:
            return (0, 1)
        x, y = fd(k >> 1)
        c = x * (2 * y - x)
        d = x * x + y * y
        return (d, c + d) if k & 1 else (c, d)
    return fd(n)[0]


bench("fib(300000) fast-doubling", lambda: fib(300000))

c = 3 ** 500000
bench("str(3^500000) 238k digits", lambda: str(c))
print("python", __import__("sys").version.split()[0])
