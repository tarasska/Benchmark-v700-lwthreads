package tskazhenik.util;

import java.util.concurrent.atomic.AtomicBoolean;

public class TTASLock {
    private final AtomicBoolean locked = new AtomicBoolean(false);

    public void lock() {
        int iteration = 0;
        while (true) {
            while (locked.getAcquire()) {
                SpinUtil.wait(++iteration);
            }

            if (!locked.weakCompareAndSetAcquire(false, true)) {
                return;
            }
        }
    }

    public boolean tryLock() {
        return !locked.getPlain() && locked.weakCompareAndSetAcquire(false, true);
    }

    public boolean isLocked() {
        return locked.getAcquire();
    }

    public void unlock() {
        locked.setRelease(false);
    }
}
