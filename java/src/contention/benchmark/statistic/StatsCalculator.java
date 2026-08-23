package contention.benchmark.statistic;

import java.util.Arrays;

public class StatsCalculator {

    public static double mean(double[] data) {
        double sum = 0.0;
        for (double v : data) sum += v;
        return sum / data.length;
    }

    public static double std(double[] data) {
        double m = mean(data);
        double sumSq = 0.0;
        for (double v : data) sumSq += (v - m) * (v - m);
        return Math.sqrt(sumSq / data.length);
    }

    public static double iqr(double[] data) {
        if (data.length == 1) {
            return 0.0;
        }
        double[] sorted = data.clone();
        Arrays.sort(sorted);
        int n = sorted.length;
        double q1, q3;

        if (n % 2 == 0) {
            q1 = median(Arrays.copyOfRange(sorted, 0, n / 2));
            q3 = median(Arrays.copyOfRange(sorted, n / 2, n));
        } else {
            q1 = median(Arrays.copyOfRange(sorted, 0, n / 2));
            q3 = median(Arrays.copyOfRange(sorted, n / 2 + 1, n));
        }
        return q3 - q1;
    }

    public static double min(double[] data) {
        return Arrays.stream(data).min().orElse(0);
    }

    public static double max(double[] data) {
        return Arrays.stream(data).max().orElse(0);
    }

    private static double median(double[] arr) {
        int m = arr.length;
        if (m == 0) {
            return 0;
        }
        if (m == 1) {
            return arr[0];
        }
        if (m % 2 == 0) {
            return (arr[m / 2 - 1] + arr[m / 2]) / 2.0;
        } else {
            return arr[m / 2];
        }
    }

}