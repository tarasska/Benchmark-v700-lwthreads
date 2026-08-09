package contention.benchmark.workload.stop.condition;

import java.util.TimerTask;
import java.util.concurrent.atomic.AtomicBoolean;

import static contention.benchmark.tools.StringFormat.indentedTitleWithData;

public class Timer implements StopCondition {
    transient private AtomicBoolean stop;

    public long workTime;

    public Timer() {}

    public Timer(long workTime) {
        this.workTime = workTime;
    }

    public void setWorkTime(long workTime) {
        this.workTime = workTime;
    }

    @Override
    public void start(int numThreads) {
        stop = new AtomicBoolean(false);
        java.util.Timer timer = new java.util.Timer(true);
        timer.schedule(new TimerTask() {
            @Override
            public void run() {
                stop.set(true);
            }
        }, workTime);
    }

    @Override
    public boolean isStopped(int id) {
        return stop.get();
    }

    @Override
    public StringBuilder toStringBuilder(int indents) {
        return new StringBuilder()
                .append(indentedTitleWithData("Type", "Timer", indents))
                .append(indentedTitleWithData("Work time", workTime, indents));
    }
}
