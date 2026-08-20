package contention.abstractions;

import contention.benchmark.statistic.custom.FcStat;

public abstract class FlatCombiningStructure {

    private final FcStat fcStat;

    protected FlatCombiningStructure() {
        fcStat = new FcStat();
    }

    protected abstract void combine(FcStat stats);

    protected final void combine() {
        var from = System.nanoTime();

        combine(this.fcStat);

        fcStat.combines++;
        fcStat.combinerTimeNanos += System.nanoTime() - from;
    }

    public FcStat getStats() {
        return fcStat;
    }
}
