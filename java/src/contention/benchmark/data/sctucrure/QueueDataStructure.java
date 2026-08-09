package contention.benchmark.data.sctucrure;

import contention.abstractions.CompositionalQueue;
import contention.abstractions.DataStructure;

import java.util.Collection;

/**
 * Common structure for any DS with push/pop operation.
 */
public class QueueDataStructure implements DataStructure<Integer> {

    private final CompositionalQueue<Integer> queue;

    public QueueDataStructure(CompositionalQueue<Integer> queue) {
        this.queue = queue;
    }


    @Override
    public Integer insert(Integer key) {   // enqueue — key is the value
        return queue.push(key) ? null : key;
    }

    @Override
    public Integer remove(Integer key) {   // dequeue — key is ignored
        return queue.pop();
    }

    @Override
    public Integer get(Integer key) {
        return queue.contains(key) ? key : null;
    }

    @Override public boolean removeAll(Collection<Integer> c) { return false; }
    @Override public int size()  { return queue.size(); }
    @Override public void clear() { queue.clear(); }
    @Override public Object getDataStructure() { return queue; }
}
