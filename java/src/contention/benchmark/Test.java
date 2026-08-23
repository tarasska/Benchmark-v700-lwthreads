package contention.benchmark;

import contention.abstractions.*;

import java.io.*;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.Random;
//import javafx.util.Pair;

import contention.benchmark.data.sctucrure.QueueDataStructure;
import contention.benchmark.statistic.StatsCalculator;
import contention.benchmark.workload.BenchParameters;
import contention.benchmark.workload.Parameters;
import contention.benchmark.workload.thread.loops.abstractions.ThreadLoop;
import contention.benchmark.data.sctucrure.MapDataStructure;
import contention.benchmark.data.sctucrure.IntSetDataStructure;
import contention.benchmark.data.sctucrure.SortedSetDataStructure;
import contention.benchmark.json.JsonConverter;
import contention.benchmark.tools.Pair;
import contention.benchmark.statistic.BenchStatistic;
import contention.benchmark.statistic.STMStatistics;
import contention.benchmark.statistic.ThreadStatistic;
import tskazhenik.GlobalScopedValues;

import static contention.benchmark.tools.StringFormat.*;

/**
 * Synchrobench-java, a benchmark to evaluate the implementations of
 * high level abstractions including Map and Set.
 *
 * @author Vincent Gramoli
 */
public class Test {

    public static final String VERSION = "11-17-2014";


    public enum Type {
        INTSET, MAP, SORTEDSET, QUEUE;
    }

    /**
     * The array of threads executing the benchmark
     */
    private Thread[] threads;

    /**
     * The array of runnable thread codes
     */
    private ThreadLoop[] threadLoops;
    //    private ThreadLoop threadLoops;
    private double elapsedTime;
    /**
     * The observed duration of the benchmark
     */

    private ThreadStatistic curStats = new ThreadStatistic();
    public BenchStatistic[] benchStatistics;
    /**
     * The throughput
     */
    private double[] throughput = null;
    /**
     * The iteration
     */
    private int currentIteration = 0;
    /**
     * The total number of operations for all threads
     */
//    private long total = 0;

    /**
     * The total number of successful operations for all threads
     */
//    private long numAdd = 0;
//    private long numRemove = 0;
//    private long numAddAll = 0;
//    private long numRemoveAll = 0;
//    private long numSize = 0;
//    private long numContains = 0;
    /**
     * The total number of failed operations for all threads
     */
//    private long failures = 0;
    /**
     * The total number of aborts
     */
//    private long aborts = 0;
    /**
     * The instance of the benchmark
     */
    private Type benchType = null;
    private BenchParameters parameters;
    //    private Parameters testParameters;
//    private Parameters prefillParameters;
//    private Parameters warmUpParameters;
    private DataStructure<Integer> dataStructure;
    /**
     * The benchmark methods
     */
    private Method[] methods;
//    private long nodesTraversed;
//
//    public long structMods;
//    private long getCount;

    private boolean useVirtualThreads = false;
    /**
     * The thread-private PRNG
     */
    final private static ThreadLocal<Random> s_random = new ThreadLocal<Random>() {
        @Override
        protected synchronized Random initialValue() {
            return new Random();
        }
    };

    //TODO delete parameters


    /**
     * Initialize the benchmark
     *
     * @param benchName the class name of the benchmark
     * @return the instance of the initialized corresponding benchmark
     */
    @SuppressWarnings("unchecked")
    public void instanciateAbstraction(String benchName) {
        try {
            Class<CompositionalMap<Integer, Integer>> benchClass =
                    (Class<CompositionalMap<Integer, Integer>>) Class.forName(benchName);
            Constructor<CompositionalMap<Integer, Integer>> c = benchClass.getConstructor();
            methods = benchClass.getDeclaredMethods();

            if (CompositionalIntSet.class.isAssignableFrom(benchClass)) {
                dataStructure = new IntSetDataStructure((CompositionalIntSet) c.newInstance());

                benchType = Type.INTSET;
            } else if (CompositionalMap.class.isAssignableFrom(benchClass)) {
                dataStructure = new MapDataStructure<>(c.newInstance());

                benchType = Type.MAP;
            } else if (CompositionalSortedSet.class.isAssignableFrom(benchClass)) {
                dataStructure = new SortedSetDataStructure<>((CompositionalSortedSet<Integer>) c.newInstance());

                benchType = Type.SORTEDSET;
            } else if (CompositionalQueue.class.isAssignableFrom(benchClass)) {
                dataStructure = new QueueDataStructure((CompositionalQueue<Integer>) c.newInstance());

                benchType = Type.QUEUE;
            }

        } catch (Exception e) {
            System.err.println("Cannot find benchmark class: " + benchName);
            System.exit(-1);
        }
    }

    /**
     * Creates as many threads as requested
     */
    private Pair<Thread[], ThreadLoop[]> initThreads(Parameters parameters) {
        ThreadLoop[] threadLoops = parameters.getWorkload(dataStructure, methods);
        Thread[] threads = new Thread[parameters.numThreads];

        for (int threadNum = 0; threadNum < parameters.numThreads; threadNum++) {
            final int threadId = threadNum;
            Runnable threadRunnable = () -> {
                ScopedValue
                        .where(GlobalScopedValues.MAX_THREADS, parameters.numThreads)
                        .where(GlobalScopedValues.THREAD_ID, threadId)
                        .run(threadLoops[threadId]);
            };
            threads[threadNum] = useVirtualThreads
                    ? Thread.ofVirtual().unstarted(threadRunnable)
                    : Thread.ofPlatform().unstarted(threadRunnable);
        }

        return new Pair<>(threads, threadLoops);
    }


    /**
     * The instance of the benchmark
     */
    public Test(BenchParameters parameters, String dataStructureClassName) {
        this.parameters = parameters;
        instanciateAbstraction(dataStructureClassName);
        this.throughput = new double[parameters.iterations];
        this.benchStatistics = new BenchStatistic[parameters.iterations];
        this.useVirtualThreads = parameters.useVirtualThreads;
    }

    /**
     * Execute the main thread that starts and terminates the benchmark threads
     *
     * @throws InterruptedException
     */
    private void execute(Parameters parameters, Thread[] threads) throws InterruptedException {
        long startTime;

        startTime = System.currentTimeMillis();
        parameters.stopCondition.start(parameters.numThreads);
        for (Thread thread : threads)
            thread.start();

        for (Thread thread : threads) {
            if (parameters.maxAwaitTime == 0) {
                thread.join();
            } else {
                long awaitTime = Math.max(1, parameters.maxAwaitTime + startTime - System.currentTimeMillis());
//                System.out.println("Await time: " + awaitTime);
                thread.join(awaitTime);
                if (thread.isAlive()) {
//                    System.err.println("The thread " + thread.getName()
//                            + " was not terminated after the expiration of time. The max await time is "
//                            + parameters.maxAwaitTime + " millis. ");
//                            + "The thread will be interrupted. ");
//                    thread.interrupt();
                    throw new RuntimeException("The thread " + thread.getName()
                            + " was not terminated after the expiration of time. The max await time is "
                            + parameters.maxAwaitTime + " millis. ");
                }
            }
        }

        long endTime = System.currentTimeMillis();
        elapsedTime = ((double) (endTime - startTime)) / 1000.0;
    }

    public void clear() {
        dataStructure.clear();
    }

    public static Parameters parseJson(String jsonParameters) {
        return JsonConverter.fromJson(jsonParameters, Parameters.class);
    }

    private void initAndExecute(Parameters parameters) throws InterruptedException {
        Pair<Thread[], ThreadLoop[]> inits = initThreads(parameters);
        Thread[] prefillThreads = inits.first;
        threadLoops = inits.second;
        execute(parameters, prefillThreads);
    }

    public void run() throws InterruptedException {
        boolean firstIteration = true;

        for (int i = 0; i < parameters.iterations; i++) {
            if (!firstIteration) {
                // give time to the JIT
                Thread.sleep(parameters.betweenIterationsDuration);
                resetStats();
                clear();
                org.deuce.transaction.estmstats.Context.threadIdCounter.set(0);
            }


            printStage("Prefill stage");
            initAndExecute(parameters.prefill);

            Thread.sleep(parameters.afterPrefillDuration);


            printStage("WarmUp stage");
            initAndExecute(parameters.warmUp);

            if (parameters.detailedStats)
                recordPreliminaryStats();
            resetStats();
            long size = dataStructure.size();

            Thread.sleep(parameters.afterWarmUpDuration);

            printStage("Test stage");
            initAndExecute(parameters.test);

            if (dataStructure.getDataStructure() instanceof MaintenanceAlg) {
                ((MaintenanceAlg) dataStructure.getDataStructure()).stopMaintenance();
                curStats.structMods += ((MaintenanceAlg) dataStructure.getDataStructure()).getStructMods();
            }

            printBasicStats(size);
            if (parameters.detailedStats)
                printDetailedStats();

            firstIteration = false;
            currentIteration++;
        }

        if (parameters.iterations > 1) {
            printIterationStats();
        }
    }


    public static String readJsonFile(String fileName) throws IOException {
        StringBuilder jsonStringBuilder = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(new FileReader(fileName))) {
            String line;
            while ((line = reader.readLine()) != null)
                jsonStringBuilder.append(line);
        } catch (IOException e) {
            System.err.println("An error occurred while reading the file: \"" + fileName + "\".");
            throw e;
        }
        return jsonStringBuilder.toString();
    }

    public static <T> void writeJsonFile(T t, String fileName) throws IOException {
        try (BufferedWriter writer = new BufferedWriter(new FileWriter(fileName))) {
            String json = JsonConverter.toJson(t);
            writer.write(json);
        } catch (IOException e) {
            System.err.println("An error occurred while writing to the file: \"" + fileName + "\".");
            throw e;
        }
    }

    public static Parameters parseJsonFile(String fileName) throws IOException {
        return parseJson(readJsonFile(fileName));
    }

    public static void main(String[] rowArgs) throws IOException, InterruptedException {
        System.out.println("CORES: " + Runtime.getRuntime().availableProcessors());
        ParseArgument args = new ParseArgument(rowArgs);
        Test test;

        BenchParameters benchParameters = new BenchParameters();
        String resultStatisticFileName = null;
        boolean createDefaultPrefill = false;
        String dataStructureClassName = "skiplists.lockfree.NonBlockingFriendlySkipListMap";

        while (args.hasNext()) {
            switch (args.getCurrent()) {
                case "-data-structure", "-ds" -> dataStructureClassName = args.getNext();
                case "-prefill" -> benchParameters.prefill = parseJsonFile(args.getNext());
                case "-warm-up" -> benchParameters.warmUp = parseJsonFile(args.getNext());
                case "-test" -> benchParameters.test = parseJsonFile(args.getNext());
                case "-range" -> benchParameters.range = Integer.parseInt(args.getNext());
                case "-iter" -> benchParameters.iterations = Integer.parseInt(args.getNext());
                case "-create-default-prefill" -> createDefaultPrefill = true;
                case "-json-file" -> benchParameters = JsonConverter.fromJson(readJsonFile(args.getNext()));
                case "-result-file" -> resultStatisticFileName = args.getNext();
                case "-virtual-threads" -> benchParameters.useVirtualThreads = true;
                default -> System.err.println("Unexpected option: " + args.getCurrent() + ". Ignoring...");
            }
            args.next();
        }

        if (createDefaultPrefill) {
            benchParameters.createDefaultPrefill();
        }

        benchParameters.init();

        test = new Test(benchParameters, dataStructureClassName);

        printHeader();

        printStage("Benchmark parameters");
        System.out.print(indentedTitleWithData("Data Structure", dataStructureClassName));
        System.out.println(benchParameters.toStringBuilder());
//
//
//        printStage("Prefill parameters");
//        test.printParams(test.parameters.prefill);
//
//        if (benchParameters.warmUp.numThreads != 0) {
//            printStage("WarmUp parameters");
//            test.printParams(test.parameters.warmUp);
//        } else {
//            printStage("Without WarmUp");
//        }
//
//        printStage("Test parameters");
//        test.printParams(test.parameters.test);

        test.run();

        if (resultStatisticFileName != null) {
            writeJsonFile(test.benchStatistics, resultStatisticFileName);
        }
    }

    /* ---------------- Input/Output -------------- */

    /**
     * Parse the parameters on the command line
     */
    private void parseCommandLineParameters(String[] args) {

        switch (args[0]) {
            case "--help", "-h" -> {
                printUsage();
                System.exit(0);
            }
        }


    }

    /**
     * Print a 80 character line filled with the same marker character
     */
    private static void printLine() {
        System.out.println(String.valueOf('-').repeat(79));
    }

    /**
     * Print the header message on the standard output
     */
    private static void printHeader() {
        String header = "Synchrobench-java\n"
                + "A benchmark-suite to evaluate synchronization techniques";
        printLine();
        System.out.println(header);
        printLine();
        System.out.println();
    }

    private static void printStage(String stageName) {
        System.out.println(getStringStage(stageName));
    }

    /**
     * Print the benchmark usage on the standard output
     */
    private void printUsage() {
        String syntax = "Usage:\n"
                + "java synchrobench.benchmark.Test [options] [-- stm-specific options]\n\n"
                + "Options:\n"
                + "\t-v            -- print detailed statistics (default: "
//                + parameters.test.detailedStats
                + ")\n"
                + "\t-t thread-num -- set the number of threads (default: "
                + parameters.test.numThreads
                + ")\n"
//                + "\t-d duration   -- set the length of the benchmark, in milliseconds (default: "
//                + parameters.test.numMilliseconds
//                + ")\n"
//                + "\t-u updates    -- set the percentage of updates (default: "
//                + ((DefaultThreadLoopParameters) parameters.threadLoopParameters).numWrites
//                + ")\n"
//                + "\t-ue inserts   -- set the percentage of threads (default: "
//                + ((DefaultThreadLoopParameters) parameters.threadLoopParameters).numInsert
//                + ")\n"
//                + "\t-ui removes   -- set the percentage of erases (default: "
//                + ((DefaultThreadLoopParameters) parameters.threadLoopParameters).numRemove
//                + ")\n"
//                + "\t-a writeAll   -- set the percentage of composite updates (default: "
//                + ((DefaultThreadLoopParameters) parameters.threadLoopParameters).numWriteAlls
//                + ")\n"
//                + "\t-s snapshot   -- set the percentage of composite read-only operations (default: "
//                + ((DefaultThreadLoopParameters) parameters.threadLoopParameters).numSnapshots
//                + ")\n"
                + "\t-r range      -- set the element range (default: "
                + parameters.range
                + ")\n"
//                + "\t-b benchmark  -- set the benchmark (default: "
//                + parameters.test.benchClassName
//                + ")\n"
//                + "\t-i size       -- set the datastructure initial size (default: "
//                + parameters.test.size
//                + ")\n"
                + "\t-n iterations -- set the bench iterations in the same JVM (default: "
                + parameters.iterations
                + ").";
//                + "\t-W warmup     -- set the JVM warmup length, in seconds (default: "
//                + testParameters.warmUp + ").";
        System.err.println(syntax);
    }

    /**
     * Print the parameters that have been given as an input to the benchmark
     */
    private void printParams(Parameters parameters) {
        System.out.println(parameters.toStringBuilder());
    }

    /**
     * Print the statistics on the standard output
     */
    private void printBasicStats(long prefillSize) {
        BenchStatistic curBenchStats = new BenchStatistic();
        curBenchStats.prefillSize = prefillSize;
        curBenchStats.threadStatistics = new ThreadStatistic[threadLoops.length];
        for (int i = 0; i < threadLoops.length; i++) {
            curStats.add(threadLoops[i].stats);
            curBenchStats.threadStatistics[i] = threadLoops[i].stats;
        }
        curBenchStats.elapsedTime = elapsedTime;
        curBenchStats.throughput = ((double) curStats.total / elapsedTime);
        curBenchStats.commonStatistic = curStats;
        curBenchStats.effectiveUpdates = curStats.numAdd + curStats.numRemove + curStats.numAddAll + curStats.numRemoveAll;
        curBenchStats.fcStat = curStats.fcStat;
        curBenchStats.opsPerCombine = (double) curStats.fcStat.ops / Math.max(1, curStats.fcStat.combines);
        curBenchStats.nanosPerCombine = (double) curStats.fcStat.combinerTimeNanos / Math.max(1, curStats.fcStat.combines);
        curBenchStats.attemptsPerCombine = (double) curStats.fcStat.attempts / Math.max(1, curStats.fcStat.combines);
        addCustomStats(curBenchStats);

        throughput[currentIteration] = ((double) curStats.total / elapsedTime);

        printStage("Benchmark statistics");

        int finalSize = dataStructure.size();
        curBenchStats.finalSize = finalSize;

//        System.out.println("  Final size:              \t" + finalSize);
        assert finalSize == (prefillSize + curStats.numAdd - curStats.numRemove) : "Final size does not reflect the modifications.";

        //System.out.println("  Otherupdate_bin.bat size:              \t" + map.size());

        // TODO what should print special for maint data structures
        // if (bench instanceof CASSpecFriendlyTreeLockFree) {
        // System.out.println("  Balance:              \t"
        // + ((CASSpecFriendlyTreeLockFree) bench).getBalance());
        // }
        // if (bench instanceof SpecFriendlyTreeLockFree) {
        // System.out.println("  Balance:              \t"
        // + ((SpecFriendlyTreeLockFree) bench).getBalance());
        // }
        // if (bench instanceof TransFriendlyMap) {
        // System.out.println("  Balance:              \t"
        // + ((TransFriendlyMap) bench).getBalance());
        // }
        // if (bench instanceof UpdatedTransFriendlySkipList) {
        // System.out.println("  Bottom changes:              \t"
        // + ((UpdatedTransFriendlySkipList) bench)
        // .getBottomLevelRaiseCount());
        // }


        if (dataStructure.getDataStructure() instanceof MaintenanceAlg) {
            curBenchStats.numNodes = ((MaintenanceAlg) dataStructure.getDataStructure()).numNodes();
        }

        System.out.println(curBenchStats);

        benchStatistics[currentIteration] = curBenchStats;
    }

    public void addCustomStats(BenchStatistic benchStats) {
        benchStats.custom = new HashMap<>();
        // FC
        var threads = benchStats.threadStatistics.length;
        var combinerNanos = new double[threads];
        var combinerAttempts = new double[threads];
        var combines = new double[threads];
        var opsPerCombine = new double[threads];
        var nanosPerCombine = new double[threads];
        for (int i = 0; i < threads; i++) {
            combinerNanos[i] = benchStats.threadStatistics[i].fcStat.combinerTimeNanos;
            combinerAttempts[i] = benchStats.threadStatistics[i].fcStat.attempts;
            combines[i] = benchStats.threadStatistics[i].fcStat.combines;
            opsPerCombine[i] = benchStats.threadStatistics[i].fcStat.ops / Math.max(1, combines[i]);
            nanosPerCombine[i] = combinerNanos[i] / Math.max(1, combines[i]);
        }

        benchStats.custom.put("meanCombinerNanos", StatsCalculator.mean(combinerNanos));
        benchStats.custom.put("meanCombinerAttempts", StatsCalculator.mean(combinerAttempts));
        benchStats.custom.put("meanCombines", StatsCalculator.mean(combines));
        benchStats.custom.put("meanOpsPerCombine", StatsCalculator.mean(opsPerCombine));
        benchStats.custom.put("meanNanosPerCombine", StatsCalculator.mean(nanosPerCombine));

        benchStats.custom.put("stdCombinerNanos", StatsCalculator.std(combinerNanos));
        benchStats.custom.put("stdCombinerAttempts", StatsCalculator.std(combinerAttempts));
        benchStats.custom.put("stdCombines", StatsCalculator.std(combines));
        benchStats.custom.put("stdOpsPerCombine", StatsCalculator.std(opsPerCombine));
        benchStats.custom.put("stdNanosPerCombine", StatsCalculator.std(nanosPerCombine));

        benchStats.custom.put("iqrCombinerNanos", StatsCalculator.iqr(combinerNanos));
        benchStats.custom.put("iqrCombinerAttempts", StatsCalculator.iqr(combinerAttempts));
        benchStats.custom.put("iqrCombines", StatsCalculator.iqr(combines));
        benchStats.custom.put("iqrOpsPerCombine", StatsCalculator.iqr(opsPerCombine));
        benchStats.custom.put("iqrNanosPerCombine", StatsCalculator.iqr(nanosPerCombine));

        benchStats.custom.put("minCombinerNanos", StatsCalculator.min(combinerNanos));
        benchStats.custom.put("minCombinerAttempts", StatsCalculator.min(combinerAttempts));
        benchStats.custom.put("minCombines", StatsCalculator.min(combines));
        benchStats.custom.put("minOpsPerCombine", StatsCalculator.min(opsPerCombine));
        benchStats.custom.put("minNanosPerCombine", StatsCalculator.min(nanosPerCombine));

        benchStats.custom.put("maxCombinerNanos", StatsCalculator.max(combinerNanos));
        benchStats.custom.put("maxCombinerAttempts", StatsCalculator.max(combinerAttempts));
        benchStats.custom.put("maxCombines", StatsCalculator.max(combines));
        benchStats.custom.put("maxOpsPerCombine", StatsCalculator.max(opsPerCombine));
        benchStats.custom.put("maxNanosPerCombine", StatsCalculator.max(nanosPerCombine));
    }

    /**
     * Detailed Warmup TM Statistics
     */
    private int numCommits = 0;
    private int numStarts = 0;
    private int numAborts = 0;

    private int numCommitsReadOnly = 0;
    private int numCommitsElastic = 0;
    private int numCommitsUpdate = 0;

    private int numAbortsBetweenSuccessiveReads = 0;
    private int numAbortsBetweenReadAndWrite = 0;
    private int numAbortsExtendOnRead = 0;
    private int numAbortsWriteAfterRead = 0;
    private int numAbortsLockedOnWrite = 0;
    private int numAbortsLockedBeforeRead = 0;
    private int numAbortsLockedBeforeElasticRead = 0;
    private int numAbortsLockedOnRead = 0;
    private int numAbortsInvalidCommit = 0;
    private int numAbortsInvalidSnapshot = 0;

    private double readSetSizeSum = 0.0;
    private double writeSetSizeSum = 0.0;
    private int statSize = 0;
    private int txDurationSum = 0;
    private int elasticReads = 0;
    private int readsInROPrefix = 0;

    /**
     * This method is called between two runs of the benchmark within the same
     * JVM to enable its warmup
     */
    public void resetStats() {
        for (ThreadLoop threadLoop : threadLoops) {
            threadLoop.stats.reset();
        }
        this.curStats.reset();

        numCommits = 0;
        numStarts = 0;
        numAborts = 0;

        numCommitsReadOnly = 0;
        numCommitsElastic = 0;
        numCommitsUpdate = 0;

        numAbortsBetweenSuccessiveReads = 0;
        numAbortsBetweenReadAndWrite = 0;
        numAbortsExtendOnRead = 0;
        numAbortsWriteAfterRead = 0;
        numAbortsLockedOnWrite = 0;
        numAbortsLockedBeforeRead = 0;
        numAbortsLockedBeforeElasticRead = 0;
        numAbortsLockedOnRead = 0;
        numAbortsInvalidCommit = 0;
        numAbortsInvalidSnapshot = 0;

        readSetSizeSum = 0.0;
        writeSetSizeSum = 0.0;
        statSize = 0;
        txDurationSum = 0;
        elasticReads = 0;
        readsInROPrefix = 0;
    }

    public void recordPreliminaryStats() {
        numAborts = STMStatistics.getTotalAborts();
        numCommits = STMStatistics.getTotalCommits();
        numCommitsReadOnly = STMStatistics.getNumCommitsReadOnly();
        numCommitsElastic = STMStatistics.getNumCommitsElastic();
        numCommitsUpdate = STMStatistics.getNumCommitsUpdate();
        numStarts = STMStatistics.getTotalStarts();
        numAbortsBetweenSuccessiveReads = STMStatistics
                .getNumAbortsBetweenSuccessiveReads();
        numAbortsBetweenReadAndWrite = STMStatistics
                .getNumAbortsBetweenReadAndWrite();
        numAbortsExtendOnRead = STMStatistics.getNumAbortsExtendOnRead();
        numAbortsWriteAfterRead = STMStatistics.getNumAbortsWriteAfterRead();
        numAbortsLockedOnWrite = STMStatistics.getNumAbortsLockedOnWrite();
        numAbortsLockedBeforeRead = STMStatistics.getNumAbortsLockedBeforeRead();
        numAbortsLockedBeforeElasticRead = STMStatistics
                .getNumAbortsLockedBeforeElasticRead();
        numAbortsLockedOnRead = STMStatistics.getNumAbortsLockedOnRead();
        numAbortsInvalidCommit = STMStatistics.getNumAbortsInvalidCommit();
        numAbortsInvalidSnapshot = STMStatistics.getNumAbortsInvalidSnapshot();
        readSetSizeSum = STMStatistics.getSumReadSetSize();
        writeSetSizeSum = STMStatistics.getSumWriteSetSize();

        statSize = STMStatistics.getStatSize();
        txDurationSum = STMStatistics.getSumCommitingTxTime();
        elasticReads = STMStatistics.getTotalElasticReads();
        readsInROPrefix = STMStatistics.getTotalReadsInROPrefix();
    }

    /**
     * Print the detailed statistics on the standard output
     */
    private void printDetailedStats() {

        numCommits = STMStatistics.getTotalCommits() - numCommits;
        numStarts = STMStatistics.getTotalStarts() - numStarts;
        numAborts = STMStatistics.getTotalAborts() - numAborts;

        numCommitsReadOnly = STMStatistics.getNumCommitsReadOnly() - numCommitsReadOnly;
        numCommitsElastic = STMStatistics.getNumCommitsElastic() - numCommitsElastic;
        numCommitsUpdate = STMStatistics.getNumCommitsUpdate() - numCommitsUpdate;

        numAbortsBetweenSuccessiveReads =
                STMStatistics.getNumAbortsBetweenSuccessiveReads() - numAbortsBetweenSuccessiveReads;
        numAbortsBetweenReadAndWrite = STMStatistics.getNumAbortsBetweenReadAndWrite() - numAbortsBetweenReadAndWrite;
        numAbortsExtendOnRead = STMStatistics.getNumAbortsExtendOnRead() - numAbortsExtendOnRead;
        numAbortsWriteAfterRead = STMStatistics.getNumAbortsWriteAfterRead() - numAbortsWriteAfterRead;
        numAbortsLockedOnWrite = STMStatistics.getNumAbortsLockedOnWrite() - numAbortsLockedOnWrite;
        numAbortsLockedBeforeRead = STMStatistics.getNumAbortsLockedBeforeRead() - numAbortsLockedBeforeRead;
        numAbortsLockedBeforeElasticRead =
                STMStatistics.getNumAbortsLockedBeforeElasticRead() - numAbortsLockedBeforeElasticRead;
        numAbortsLockedOnRead = STMStatistics.getNumAbortsLockedOnRead() - numAbortsLockedOnRead;
        numAbortsInvalidCommit = STMStatistics.getNumAbortsInvalidCommit() - numAbortsInvalidCommit;
        numAbortsInvalidSnapshot = STMStatistics.getNumAbortsInvalidSnapshot() - numAbortsInvalidSnapshot;

        assert (numAborts == (numAbortsBetweenSuccessiveReads
                + numAbortsBetweenReadAndWrite + numAbortsExtendOnRead
                + numAbortsWriteAfterRead + numAbortsLockedOnWrite
                + numAbortsLockedBeforeRead + numAbortsLockedBeforeElasticRead
                + numAbortsLockedOnRead + numAbortsInvalidCommit + numAbortsInvalidSnapshot));

        assert (numStarts - numAborts) == numCommits;

        readSetSizeSum = STMStatistics.getSumReadSetSize() - readSetSizeSum;
        writeSetSizeSum = STMStatistics.getSumWriteSetSize() - writeSetSizeSum;
        statSize = STMStatistics.getStatSize() - statSize;
        txDurationSum = STMStatistics.getSumCommitingTxTime() - txDurationSum;

        printStage("TM statistics");

        StringBuilder stats = new StringBuilder()
                .append(indentedTitleWithData("Commits", numCommits))
                .append(indentedTitleWithDataPercent("|--regular read only (%)",
                        numCommitsReadOnly, (double) numCommitsReadOnly / (double) numCommits))
                .append(indentedTitleWithDataPercent("|--elastic (%)",
                        numCommitsElastic, (double) numCommitsElastic / (double) numCommits))
                .append(indentedTitleWithDataPercent("|--regular update (%)",
                        numCommitsUpdate, (double) numCommitsUpdate / (double) numCommits))
                .append(indentedTitleWithData("Starts", numStarts))
                .append(indentedTitleWithDataPercent("|--Aborts", numAborts, 1))
                .append(indentedTitleWithDataPercent("|--between succ. read",
                        numAbortsBetweenSuccessiveReads,
                        (double) numAbortsBetweenSuccessiveReads / (double) numAborts))
                .append(indentedTitleWithDataPercent("|--between read & write",
                        numAbortsBetweenReadAndWrite,
                        (double) numAbortsBetweenReadAndWrite / (double) numAborts))
                .append(indentedTitleWithDataPercent("|--extend upon read",
                        numAbortsExtendOnRead, (double) numAbortsExtendOnRead / (double) numAborts))
                .append(indentedTitleWithDataPercent("|--write after read",
                        numAbortsWriteAfterRead, (double) numAbortsWriteAfterRead / (double) numAborts))
                .append(indentedTitleWithDataPercent("|--locked on write",
                        numAbortsLockedOnWrite, (double) numAbortsLockedOnWrite / (double) numAborts))
                .append(indentedTitleWithDataPercent("|--locked before read",
                        numAbortsLockedBeforeRead, (double) numAbortsLockedBeforeRead / (double) numAborts))
                .append(indentedTitleWithDataPercent("|--locked before eread",
                        numAbortsLockedBeforeElasticRead,
                        (double) numAbortsLockedBeforeElasticRead / (double) numAborts))
                .append(indentedTitleWithDataPercent("|--locked on read",
                        numAbortsLockedOnRead, (double) numAbortsLockedOnRead / (double) numAborts))
                .append(indentedTitleWithDataPercent("|--invalid commit",
                        numAbortsInvalidCommit, (double) numAbortsInvalidCommit / (double) numAborts))
                .append(indentedTitleWithDataPercent("|--invalid snapshot",
                        numAbortsInvalidSnapshot, (double) numAbortsInvalidSnapshot / (double) numAborts))
                .append(indentedTitleWithData("Read set size on avg.", readSetSizeSum / statSize))
                .append(indentedTitleWithData("Write set size on avg.", writeSetSizeSum / statSize))
                .append(indentedTitleWithData("Tx time-to-commit on avg.",
                        formatDouble((double) txDurationSum / statSize) + " microsec"))
                .append(indentedTitleWithData("Number of elastic reads", elasticReads))
                .append(indentedTitleWithData("Number of reads in RO prefix", readsInROPrefix));

        System.out.println(stats);
    }

    /**
     * Print the iteration statistics on the standard output
     */
    private void printIterationStats() {
        printStage("Iteration statistics");

        int n = parameters.iterations;
        System.out.println(indentedTitleWithData("Iterations", n));
        double sum = 0;
        for (int i = 0; i < n; i++) {
            sum += ((throughput[i] / 1024) / 1024);
        }
        System.out.println(indentedTitleWithData("Total throughput (mebiops/s)", sum));
        double mean = sum / n;
        System.out.println(indentedTitleWithData("|--Mean", mean));
        double temp = 0;
        for (int i = 0; i < n; i++) {
            double diff = ((throughput[i] / 1024) / 1024) - mean;
            temp += diff * diff;
        }
        double var = temp / n;
        System.out.println(indentedTitleWithData("|--Variance", var));
        double stdevp = java.lang.Math.sqrt(var);
        System.out.println(indentedTitleWithData("|--Standard deviation pop", stdevp));
        double sterr = stdevp / java.lang.Math.sqrt(n);
        System.out.println(indentedTitleWithData("|--Standard error", sterr));
        System.out.println(indentedTitleWithData("|--Margin of error (95% CL)", (sterr * 1.96)));
    }


}
