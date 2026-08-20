package contention.benchmark.statistic.custom;

public class FcStat {
    public long combines;
    public long attempts;
    public long ops;
    public long combinerTimeNanos;

    public void add(FcStat other) {
        this.combines += other.combines;
        this.attempts += other.attempts;
        this.ops += other.ops;
        this.combinerTimeNanos += other.combinerTimeNanos;
    }
}
