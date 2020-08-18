package com._4paradigm.sql.jmh;

import com._4paradigm.sql.ResultSet;
import com._4paradigm.sql.SQLRequestRow;
import com._4paradigm.sql.sdk.SdkOption;
import com._4paradigm.sql.sdk.SqlExecutor;
import com._4paradigm.sql.sdk.impl.SqlClusterExecutor;
import org.openjdk.jmh.annotations.*;
import org.openjdk.jmh.runner.Runner;
import org.openjdk.jmh.runner.RunnerException;
import org.openjdk.jmh.runner.options.Options;
import org.openjdk.jmh.runner.options.OptionsBuilder;

import java.util.ArrayList;
import java.util.concurrent.TimeUnit;

@BenchmarkMode(Mode.All)
@OutputTimeUnit(TimeUnit.MILLISECONDS)
@State(Scope.Benchmark)
@Fork(value = 1, jvmArgs = {"-Xms4G", "-Xmx4G"})
@Warmup(iterations = 1)
public class FESQLLastJoinBenchmark {
    private ArrayList<String> dataset = new ArrayList<>();
    private SqlExecutor executor;
    private SdkOption option;
    private String db = "db" + System.nanoTime();
    private String ddl = "create table t1 (col1 string, col2 timestamp, " +
            "col3 float," +
            "col4 double," +
            "col5 string," +
            "index(key=col1, ts=col2));";

    private String ddl1 = "create table t2 (col1 string, col2 timestamp, " +
            "col3 float," +
            "col4 double," +
            "col5 string," +
            "index(key=col1, ts=col2));";

    private boolean setupOk = false;
    private int recordSize = 10000;
    private String format = "insert into %s values('%s', %d," +
            "100.0, 200.0, 'hello world');";
    private long counter = 0;

    public FESQLLastJoinBenchmark() {
        SdkOption sdkOption = new SdkOption();
        sdkOption.setSessionTimeout(30000);
        sdkOption.setZkCluster(BenchmarkConfig.ZK_CLUSTER);
        sdkOption.setZkPath(BenchmarkConfig.ZK_PATH);
        this.option = sdkOption;
        try {
            executor = new SqlClusterExecutor(option);
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    @Setup
    public void setup() {
        setupOk = executor.createDB(db);
        if (!setupOk) {
            return;
        }
        setupOk = executor.executeDDL(db, ddl);
        if (!setupOk) {
            return;
        }
        setupOk = executor.executeDDL(db, ddl1);
        if (!setupOk) {
            return;
        }
        for (int i = 0; i < recordSize/1000; i++) {
            for (int j = 0; j < 1000; j++) {
                String sql = String.format(format, "t1","pk" + i, System.currentTimeMillis());
                executor.executeInsert(db, sql);
                sql = String.format(format, "t2", "pk" + i, System.currentTimeMillis() - j);
                executor.executeInsert(db, sql);
            }
        }
    }

    @Benchmark
    public void lastJoinBm() {
        String sql = "select t1.col1 as c1, t2.col2 as c2 , " +
                "sum(t1.col3) over w  from t1 last join t2 " +
                "order by t2.col2 on t1.col1 = t2.col1 and t1.col2 > t2.col2 " +
                "window w as (partition by t1.col1 order by t1.col2 ROWS Between 1000 preceding and current row);";
        SQLRequestRow row = executor.getRequestRow(db, sql);
        String pk = "pk0";
        row.Init(pk.length());
        row.AppendString(pk);
        row.AppendTimestamp(System.currentTimeMillis());
        row.AppendFloat(1.0f);
        row.AppendDouble(2.0d);
        row.AppendNULL();
        row.Build();
        executor.executeSQL(db, sql, row);
    }


    public static void main(String[] args) throws RunnerException {

       Options opt = new OptionsBuilder()
                .include(FESQLLastJoinBenchmark.class.getSimpleName())
                .forks(1)
                .build();
        new Runner(opt).run();
    }
}