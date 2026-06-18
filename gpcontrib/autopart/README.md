## Install

```
make install USE_PGXS=1 PG_CONFIG=<path to pg_config>
```

## Test

```sql
create extension autopart;

create table t_p (i int, i2 int not null, t text) distributed by (i) partition by range (i2);

SELECT add_to_autopart_config('t_p',
                             'i2',
                             '1000');

```

```sql
session1> BEGIN;
session1> insert into t_p select g,g,g from generate_series(1,100) g;
INSERT 0 100


session2> BEGIN;
session2> insert into t_p select g,g,1 from generate_series(2000,2900) g;
INSERT 0 901

session1> COMMIT;

session2> COMMIT;

session1> \d+ t_p
                              Partitioned table "public.t_p"
 Column |  Type   | Collation | Nullable | Default | Storage  | Stats target | Description 
--------+---------+-----------+----------+---------+----------+--------------+-------------
 i      | integer |           |          |         | plain    |              | 
 i2     | integer |           | not null |         | plain    |              | 
 t      | text    |           |          |         | extended |              | 
Partition key: RANGE (i2)
Partitions: t_p_0_1000 FOR VALUES FROM (0) TO (1000),
            t_p_2000_3000 FOR VALUES FROM (2000) TO (3000)
Distributed by: (i)
Access method: heap

session1>  select count() from t_p;
 count 
-------
  1001
```


```sql
adb=# BEGIN;
adb=#  create table t_p3 (i int, i2 int not null, t text) distributed by (i) partition by range (i2);

adb=# SELECT add_to_autopart_config('t_p3',
                             'i2',
                             '1000');
 add_to_autopart_config 
------------------------
 t
(1 row)

adb=# insert into t_p3 select g,g,g from generate_series(1,100) g;
NOTICE:  relation "t_p3_0_1000" already exists, skipping  (seg0 127.0.1.1:7002 pid=203714)
NOTICE:  relation "t_p3_0_1000" already exists, skipping  (seg1 127.0.1.1:7003 pid=203715)
NOTICE:  relation "t_p3_0_1000" already exists, skipping  (seg2 127.0.1.1:7004 pid=203716)
INSERT 0 100
adb=# commit;
COMMIT
adb=# \d+ t_p3;
                              Partitioned table "public.t_p3"
 Column |  Type   | Collation | Nullable | Default | Storage  | Stats target | Description 
--------+---------+-----------+----------+---------+----------+--------------+-------------
 i      | integer |           |          |         | plain    |              | 
 i2     | integer |           | not null |         | plain    |              | 
 t      | text    |           |          |         | extended |              | 
Partition key: RANGE (i2)
Partitions: t_p3_0_1000 FOR VALUES FROM (0) TO (1000)


```