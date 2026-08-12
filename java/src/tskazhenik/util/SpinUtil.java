package tskazhenik.util;

public class SpinUtil {

    private static final int SPIN_THRESHOLD = 16;
    private static final int BASE_SPIN_COUNT = 128;
    private static final int MAX_SPIN_COUNT = 10000;

    public static void wait(int iteration) {
        if (iteration < SPIN_THRESHOLD) {
            spin(iteration);
        } else {
            Thread.yield();
        }
    }

    private static void spin(int iteration) {
        int spins = Math.min(BASE_SPIN_COUNT * (1 << iteration), MAX_SPIN_COUNT);
        for (int i = 0; i < spins; i++) {
            Thread.onSpinWait();
        }
    }
}
