package tskazhenik.util;

import java.util.concurrent.atomic.AtomicBoolean;

public class TTASLock {
    private final AtomicBoolean locked = new AtomicBoolean(false);

    public boolean tryLock() {
        return !locked.getAcquire() && locked.weakCompareAndSetAcquire(false, true);
    }

    public boolean isLocked() {
        return locked.getAcquire();
    }

    public void unlock() {
        locked.setRelease(false);
    }
}
