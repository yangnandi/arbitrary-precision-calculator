import java.math.BigInteger;

public class Bench {
    static void bench(String name, Runnable fn, int n) {
        long best = Long.MAX_VALUE;
        for (int i = 0; i < n; i++) {
            long t = System.nanoTime();
            fn.run();
            long d = System.nanoTime() - t;
            if (d < best) best = d;
        }
        System.out.printf("%-34s %9.1f ms%n", name, best / 1e6);
    }

    static BigInteger fact(int n) {
        BigInteger r = BigInteger.ONE;
        for (int i = 2; i <= n; i++) r = r.multiply(BigInteger.valueOf(i));
        return r;
    }

    // fast doubling fibonacci
    static BigInteger[] fd(int k) {
        if (k == 0) return new BigInteger[]{BigInteger.ZERO, BigInteger.ONE};
        BigInteger[] p = fd(k >> 1);
        BigInteger x = p[0], y = p[1];
        BigInteger c = x.multiply(y.shiftLeft(1).subtract(x));
        BigInteger d = x.multiply(x).add(y.multiply(y));
        return (k & 1) == 1 ? new BigInteger[]{d, c.add(d)} : new BigInteger[]{c, d};
    }

    public static void main(String[] args) {
        BigInteger a = BigInteger.TEN.pow(500000).add(BigInteger.valueOf(7));
        BigInteger b = BigInteger.TEN.pow(500000).add(BigInteger.valueOf(3));
        bench("mul 500k x 500k digit", () -> a.multiply(b), 3);
        bench("factorial(50000)", () -> fact(50000), 3);
        bench("fib(300000) fast-doubling", () -> { BigInteger ignore = fd(300000)[0]; }, 3);
        BigInteger c = BigInteger.valueOf(3).pow(500000);
        bench("str(3^500000) 238k digits", () -> c.toString(), 3);
        System.out.println("java " + System.getProperty("java.version"));
    }
}
