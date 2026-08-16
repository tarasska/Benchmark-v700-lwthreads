package tskazhenik.ds.flatcombining;

import contention.abstractions.CompositionalQueue;
import tskazhenik.GlobalScopedValues;
import tskazhenik.util.SpinUtil;
import tskazhenik.util.TTASLock;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.util.ArrayDeque;

public class FcStackOptLockBackoff implements CompositionalQueue<Integer>  {
    private static final int THREADS_LIMIT = 2048;
    private static final int FC_ATTEMPTS = 16;
    private static final int FC_THRESHOLD = 2;

    @jdk.internal.vm.annotation.Contended
    private final TTASLock lock;

    @jdk.internal.vm.annotation.Contended
    private final FcRequest[] fcRequestSlots;

    @jdk.internal.vm.annotation.Contended
    private final ArrayDeque<Integer> stack;

    public FcStackOptLockBackoff() {
        this.lock = new TTASLock();
        this.fcRequestSlots = new FcRequest[THREADS_LIMIT];
        this.stack = new ArrayDeque<>(100_000);

        for (int i = 0; i < THREADS_LIMIT; i++) {
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
                int iterations = 0;
                while (req.isPublished()) {
                    iterations = (iterations + 1) % 16;
                    if (iterations == 0) {
                        if (!lock.isLocked()) {
                            break;
                        } else {
                            Thread.yield();
                        }
                    } else {
                        for (int i = 0; i < Math.min(1 << iterations, 1024); i++) {
                            Thread.onSpinWait();
                        }
                    }
                }

                if (!req.isPublished()) {
                    return;
                }
            }
        }
    }

    void combine() {
        var registeredThreadsCount = GlobalScopedValues.MAX_THREADS.get();
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

            if (ops < FC_THRESHOLD) {
                break;  // not enough pending work
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

    @jdk.internal.vm.annotation.Contended
    private static class FcRequest {
        @jdk.internal.vm.annotation.Contended
        FCOperationType type = FCOperationType.NONE;

        @jdk.internal.vm.annotation.Contended
        Integer value = 0;

        @jdk.internal.vm.annotation.Contended
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
