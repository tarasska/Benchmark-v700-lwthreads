package tskazhenik.ds.flatcombining;

import contention.abstractions.CompositionalQueue;
import contention.abstractions.FlatCombiningStructure;
import contention.benchmark.statistic.custom.FcStat;
import tskazhenik.GlobalConstants;
import tskazhenik.GlobalScopedValues;
import tskazhenik.util.TTASLock;

import java.lang.invoke.VarHandle;
import java.util.ArrayDeque;

public class FcStackOptLockFence extends FlatCombiningStructure implements CompositionalQueue<Integer>  {
    private static final int FC_ATTEMPTS = 16;
    private static final int FC_THRESHOLD = 2;

    @jdk.internal.vm.annotation.Contended
    private final TTASLock lock;

    @jdk.internal.vm.annotation.Contended
    private final FcRequest[] fcRequestSlots;

    @jdk.internal.vm.annotation.Contended
    private final ArrayDeque<Integer> stack;

    public FcStackOptLockFence() {
        this.lock = new TTASLock();
        this.fcRequestSlots = new FcRequest[GlobalConstants.THREADS_LIMIT];
        this.stack = new ArrayDeque<>(1_000_000);

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
        req.published = true;
        VarHandle.releaseFence();

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
                while (req.published) {
                    iterations = (iterations + 1) % 16;
                    if (iterations == 0 && !lock.isLocked()) {
                        break;
                    }
                    Thread.yield();
                }

                if (!req.published) {
                    return;
                }
            }
        }
    }

    @Override
    protected void combine(FcStat stats) {
        var registeredThreadsCount = GlobalScopedValues.MAX_THREADS.get();
        for (int t = 0; t < FC_ATTEMPTS; ++t) {
            VarHandle.acquireFence();
            int ops = 0;
            for (int i = 0; i < registeredThreadsCount; i++) {
                var req = fcRequestSlots[i];

                if (req.published) {
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

                    req.published = false;
                }
            }
            VarHandle.releaseFence();

            stats.ops += ops;
            if (ops < FC_THRESHOLD) {
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

    @jdk.internal.vm.annotation.Contended
    private static class FcRequest {
        @jdk.internal.vm.annotation.Contended("d")
        FCOperationType type = FCOperationType.NONE;

        @jdk.internal.vm.annotation.Contended("d")
        Integer value = 0;

        @jdk.internal.vm.annotation.Contended("s")
        boolean published = false;

    }
}
