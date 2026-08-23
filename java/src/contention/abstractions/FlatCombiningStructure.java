package contention.abstractions;

import contention.benchmark.statistic.custom.FcStat;

public abstract class FlatCombiningStructure {

    private final ThreadLocal<FcStat> fcStat = ThreadLocal.withInitial(FcStat::new);

    protected abstract void combine(FcStat stats);

    protected final void combine() {
        var from = System.nanoTime();

        var stat = this.fcStat.get();

        combine(stat);

        stat.combines++;
        stat.combinerTimeNanos += System.nanoTime() - from;
    }

    public FcStat getStats() {
        return fcStat.get();
    }
}
