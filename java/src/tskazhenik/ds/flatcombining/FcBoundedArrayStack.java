package tskazhenik.ds.flatcombining;

import contention.abstractions.CompositionalQueue;

import java.util.Objects;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.locks.ReentrantLock;

public class FcBoundedArrayStack implements CompositionalQueue<Integer> {
    private static final int MAX_THREADS = 2048;
    private static final int FC_ATTEMPTS = 16;
    private static final int FC_THRESHOLD = 2;

    private static final int MAX_ELEMENTS = 1_000_000;

    private final ReentrantLock lock;

    private final FcRequest[] fcRequestSlots;
    private final AtomicInteger firstNotUsedSlotIdx;

    private final Integer[] stack;
    private int topIdx;

    private final ThreadLocal<FcRequest> fcRequest;

    public FcBoundedArrayStack() {
        this.lock = new ReentrantLock();
        this.fcRequestSlots = new FcRequest[MAX_THREADS];
        this.firstNotUsedSlotIdx = new AtomicInteger(0);
        this.stack = new Integer[MAX_ELEMENTS];
        this.topIdx = 0;

        for (int i = 0; i < MAX_THREADS; i++) {
            fcRequestSlots[i] = new FcRequest();
        }

        this.fcRequest= ThreadLocal.withInitial(() -> fcRequestSlots[firstNotUsedSlotIdx.getAndIncrement()]);
    }

    @Override
    public boolean push(Integer value) {
        var req = fcRequest.get();
        req.value = value;
        req.type = FCOperationType.PUSH;

        handleRequest(req);

        return req.value != null;
    }

    @Override
    public Integer pop() {
        var req = fcRequest.get();
        req.value = null;
        req.type = FCOperationType.POP;

        handleRequest(req);

        return req.value;
    }

    @Override
    public boolean contains(Integer value) {
        var req = fcRequest.get();
        req.value = value;
        req.type = FCOperationType.CONTAINS;

        handleRequest(req);

        return req.value != null;
    }

    @Override
    public int size() {
        return topIdx;
    }

    @Override
    public void clear() {
    }

    private void handleRequest(FcRequest req) {
        req.status = FCStatus.PUSHED;

        while (true) {
            if (lock.tryLock()) {
                try {
                    combine();
                } finally {
                    lock.unlock();
                }
                return;
            } else {
                while (req.status != FCStatus.FINISHED && lock.isLocked()) {
                    Thread.yield();
                }

                if (req.status == FCStatus.FINISHED) {
                    return;
                }
            }
        }
    }

    void combine() {
        var registeredThreadsCount = firstNotUsedSlotIdx.get();
        for (int t = 0; t < FC_ATTEMPTS; ++t) {
            int ops = 0;
            for (int i = 0; i < registeredThreadsCount; i++) {
                var req = fcRequestSlots[i];

                if (req.status == FCStatus.PUSHED) {
                    ++ops;

                    if (req.type == FCOperationType.PUSH) {
                        if (topIdx < MAX_ELEMENTS) {
                            stack[topIdx] = req.value;
                            topIdx++;
                        } else {
                            req.value = null;
                        }
                    } else if (req.type == FCOperationType.POP) {

                        if (topIdx == 0) {
                            req.value = null;
                        } else {
                            req.value = stack[topIdx - 1];
                            topIdx--;
                        }
                    } else if (req.type == FCOperationType.CONTAINS) {

                        boolean contains = false;
                        for (int j = topIdx - 1; j >= 0; j--) {
                            if (Objects.equals(stack[j], req.value)) {
                                contains = true;
                                break;
                            }
                        }
                        if (!contains) {
                            req.value = null;
                        }
                    }

                    req.status = FCStatus.FINISHED;
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
        volatile FCOperationType type = FCOperationType.NONE;
        volatile Integer value = 0;
        volatile FCStatus status = FCStatus.EMPTY;
    }
}
