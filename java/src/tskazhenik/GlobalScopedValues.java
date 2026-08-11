package tskazhenik;

public class GlobalScopedValues {
    /**
     * Max threadId for benchmark run
     */
    public static final ScopedValue<Integer> MAX_THREADS = ScopedValue.newInstance();
    /**
     * Must be in [0; MAX_THREADS)
     */
    public static final ScopedValue<Integer> THREAD_ID = ScopedValue.newInstance();
}
