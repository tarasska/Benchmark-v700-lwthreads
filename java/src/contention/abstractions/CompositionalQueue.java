package contention.abstractions;

public interface CompositionalQueue<T> {
    boolean push(T value);
    T pop();

    boolean contains(T value);

    int size();
    void clear();
}
