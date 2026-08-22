package tskazhenik.ds.flatcombining;

import contention.abstractions.CompositionalQueue;
import contention.abstractions.FlatCombiningStructure;
import contention.benchmark.statistic.custom.FcStat;
import tskazhenik.GlobalConstants;
import tskazhenik.GlobalScopedValues;
import tskazhenik.util.TTASLock;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.util.ArrayDeque;

public class FcStackOptNotCA4THalf extends FlatCombiningStructure implements CompositionalQueue<Integer> {
    private static final int FC_ATTEMPTS = 4;

    @jdk.internal.vm.annotation.Contended
    private final TTASLock lock;

    @jdk.internal.vm.annotation.Contended
    private final FcRequest[] fcRequestSlots;

    @jdk.internal.vm.annotation.Contended
    private final ArrayDeque<Integer> stack;

    public FcStackOptNotCA4THalf() {
        this.lock = new TTASLock();
        this.fcRequestSlots = new FcRequest[GlobalConstants.THREADS_LIMIT];
        this.stack = new ArrayDeque<>(100_000);

        for (int i = 0; i < GlobalConstants.THREADS_LIMIT; i++) {
            fcRequestSlots[i] = new FcRequest();
        }
    }

    @Override
    public boolean push(Integer value) {
        var req = fcRequestSlots[GlobalScopedValues.THREAD_ID.get()];
        req.value = value;
        req.type = FCOperationType.PUSH;

        handleRequest(req);

        return req.value != null;
    }

    @Override
    public Integer pop() {
        var req = fcRequestSlots[GlobalScopedValues.THREAD_ID.get()];
        req.value = null;
        req.type = FCOperationType.POP;

        handleRequest(req);

        return req.value;
    }

    @Override
    public boolean contains(Integer value) {
        var req = fcRequestSlots[GlobalScopedValues.THREAD_ID.get()];
        req.value = value;
        req.type = FCOperationType.CONTAINS;

        handleRequest(req);

        return req.value != null;
    }

    @Override
    public int size() {
        return stack.size();
    }

    @Override
    public void clear() {
    }

    private void handleRequest(FcRequest req) {
        req.publish();

        while (true) {
            if (lock.tryLock()) {
                try {
                    combine();
                } finally {
                    lock.unlock();
                }
                return;
            } else {
                while (req.isPublished() && lock.isLocked()) {
                    Thread.yield();
                }

                if (!req.isPublished()) {
                    return;
                }
            }
        }
    }

    @Override
    protected void combine(FcStat stats) {
        var registeredThreadsCount = GlobalScopedValues.MAX_THREADS.get();
        var threshold = registeredThreadsCount / 2;
        for (int t = 0; t < FC_ATTEMPTS; ++t) {
            int ops = 0;
            for (int i = 0; i < registeredThreadsCount; i++) {
                var req = fcRequestSlots[i];

                if (req.isPublished()) {
                    ++ops;

                    if (req.type == FCOperationType.PUSH) {

                        stack.push(req.value);
                    } else if (req.type == FCOperationType.POP) {

                        if (stack.isEmpty()) {
                            req.value = null;
                        } else {
                            req.value = stack.pop();
                        }
                    } else if (req.type == FCOperationType.CONTAINS) {

                        if (!stack.contains(req.value)) {
                            req.value = null;
                        }
                    }

                    req.markFinished();
                }
            }

            stats.ops += ops;
            if (ops < threshold) {
                stats.attempts += t + 1;
                return;  // not enough pending work
            }
        }
    }

    enum FCOperationType {
        NONE,
        PUSH,
        POP,
        CONTAINS
    }

    enum FCStatus  {
        EMPTY ,
        PUSHED,
        FINISHED
    }

    private static class FcRequest {
        FCOperationType type = FCOperationType.NONE;
        Integer value = 0;
        boolean published = false;

        public void publish() {
            PUBLISHED_HANDLE.setRelease(this, true);
        }

        public void markFinished() {
            PUBLISHED_HANDLE.setRelease(this, false);
        }

        public boolean isPublished() {
            return (boolean) PUBLISHED_HANDLE.getAcquire(this);
        }

        private static final VarHandle PUBLISHED_HANDLE;

        static {
            try {
                // Find an instance field VarHandle using Lookup
                PUBLISHED_HANDLE = MethodHandles.lookup()
                        .findVarHandle(FcRequest.class, "published", boolean.class);
            } catch (ReflectiveOperationException e) {
                throw new Error(e);
            }
        }
    }
}
