package tskazhenik.ds.lock;

import contention.abstractions.CompositionalQueue;

import java.util.ArrayDeque;
import java.util.Deque;

public class SynchronizedStack implements CompositionalQueue<Integer> {

    private final Object lock = new Object();
    private final Deque<Integer> stack = new ArrayDeque<>();

    @Override
    public boolean push(Integer value) {
        synchronized (lock) {
            stack.push(value);
        }
        return true;
    }

    @Override
    public Integer pop() {
        synchronized (lock) {
            if (stack.isEmpty()) {
                return null;
            }

            return stack.pop();
        }
    }

    @Override
    public boolean contains(Integer value) {
        synchronized (lock) {
            return stack.contains(value);
        }
    }

    @Override
    public int size() {
        synchronized (lock) {
            return stack.size();
        }
    }

    @Override
    public void clear() {
        synchronized (lock) {
            stack.clear();
        }
    }
}
