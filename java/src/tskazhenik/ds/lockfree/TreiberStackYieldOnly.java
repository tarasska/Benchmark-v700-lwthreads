package tskazhenik.ds.lockfree;

import contention.abstractions.CompositionalQueue;

import java.util.Objects;
import java.util.concurrent.atomic.AtomicReference;

public class TreiberStackYieldOnly implements CompositionalQueue<Integer> {

    private final AtomicReference<Node> stack = new AtomicReference<>();

    @Override
    public boolean push(Integer value) {
        var newNode = new Node(value);
        while (true) {
            var top = stack.get();
            newNode.next = top;
            if (stack.compareAndSet(top, newNode)) {
                return true;
            }

            Thread.yield();
        }
    }

    @Override
    public Integer pop() {
        while (true) {
            var top = stack.get();
            if (top == null) {
                return null;
            }
            if (stack.compareAndSet(top, top.next)) {
                return top.key;
            }

            Thread.yield();
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
