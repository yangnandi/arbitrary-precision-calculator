import java.math.BigInteger;

/**
 * Measures how BigInteger multiplication cost scales with operand size.
 * The fitted exponent identifies the algorithm:
 *   2.00  -> schoolbook (naive O(n^2))
 *   ~1.58 -> Karatsuba
 *   ~1.465 -> Toom-Cook 3
 *   ~1.0  -> FFT (Schonhage-Strassen / GMP)
 */
public class Scale {
    public static void main(String[] args) {
        int[] sizes = {200000, 400000, 800000, 1600000};
        BigInteger[] nums = new BigInteger[sizes.length];
        for (int i = 0; i < sizes.length; i++) {
            // exactly `sizes[i]` decimal digits
            nums[i] = BigInteger.TEN.pow(sizes[i] - 1).add(BigInteger.valueOf(123456789L));
        }

        double[] times = new double[sizes.length];
        for (int i = 0; i < sizes.length; i++) {
            long best = Long.MAX_VALUE;
            for (int r = 0; r < 3; r++) {
                long t = System.nanoTime();
                BigInteger p = nums[i].multiply(nums[i]);
                long d = System.nanoTime() - t;
                if (p.signum() == 0) System.out.print(""); // keep it live
                if (d < best) best = d;
            }
            times[i] = best / 1e6;
            System.out.printf("%9d digits  %10.1f ms%n", sizes[i], times[i]);
        }

        System.out.println();
        System.out.println("fitted exponent between consecutive sizes:");
        for (int i = 1; i < sizes.length; i++) {
            double e = Math.log(times[i] / times[i - 1]) / Math.log((double) sizes[i] / sizes[i - 1]);
            System.out.printf("  %d -> %d : %.3f%n", sizes[i - 1], sizes[i], e);
        }
        double overall = Math.log(times[sizes.length - 1] / times[0])
                / Math.log((double) sizes[sizes.length - 1] / sizes[0]);
        System.out.printf("  overall %.3f%n", overall);
    }
}
