package tskazhenik.ds.lockfree;

import contention.abstractions.CompositionalQueue;
import tskazhenik.util.SpinUtil;

import java.util.Objects;
import java.util.concurrent.atomic.AtomicReference;

public class TreiberStack implements CompositionalQueue<Integer> {

    private final AtomicReference<Node> stack = new AtomicReference<>();

    @Override
    public boolean push(Integer value) {
        var newNode = new Node(value);
        int iteration = 0;
        while (true) {
            var top = stack.get();
            if (top != null) {
                newNode.next = top.next;
            }
            if (stack.compareAndSet(top, newNode)) {
                return true;
            }

            SpinUtil.wait(++iteration);
        }
    }

    @Override
    public Integer pop() {
        int iteration = 0;
        while (true) {
            var top = stack.get();
            if (top == null) {
                return null;
            }
            if (stack.compareAndSet(top, top.next)) {
                return top.key;
            }

            SpinUtil.wait(++iteration);
        }
    }

    @Override
    public boolean contains(Integer value) {
        var current = stack.get();
        while (current != null) {
            if (Objects.equals(current.key, value)) {
                return true;
            }
            current = current.next;
        }
        return false;
    }

    @Override
    public int size() {
        return 0;
    }

    @Override
    public void clear() {
        // TODO?
    }

    private static class Node {

        private final Integer key;
        private Node next;

        private Node(Integer key) {
            this.key = key;
            this.next = null;
        }

    }
}
