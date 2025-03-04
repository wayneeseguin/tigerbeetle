# TigerBeetle Architecture

This document is a short technical overview of the internals of TigerBeetle. It starts with the
problem statement to motivate everything that follows, then gives the overview of TigerBeetle and
provides motivation for a number of specific design decisions, and finally concludes with a list of
prior art which inspired TigerBeetle.

## Problem Statement

TigerBeetle is a database for workloads that:

- are very contended and parallelize/shard poorly,
- consist mostly of writes,
- need very high throughput and moderately low latency,
- require strong consistency guarantees,
- and demand very high levels of durability.

At the moment, TigerBeetle is focused on financial transactions (transfers). Each transfer touches
two accounts, and accounts are Pareto-distributed --- a few accounts are responsible for the bulk of
the transfers. Multi-object transactions in combination with contention make traditional database
architecture inefficient --- locking the two accounts and then going over the network to the
application to compute the balances' update is slow, and, due to contention, this work can't be
efficiently distributed across separate machines.

While at the moment TigerBeetle implements only accounting logic, the underlying state machine can
be swapped for a different one. TigerBeetle does only double-entry bookkeeping, but _can_ do
anything that has the same combination of high performance and safety requirements, and high
contention.

## Overview

TigerBeetle is a distributed system. It is a cluster of six replicas. The state of each replica is
just a single file on disk, called the data file. Replicas use a consensus protocol to ensure that their
respective data files store identical data.

Specifically, TigerBeetle is a replicated state machine. The ground state of the system is an
immutable, hash-chained, append-only log of prepares. Each prepare is a batch of 8 thousands of
individual transfer objects. The primary replica:

- accepts requests from the clients,
- decides on the order in which requests shall be processed,
- converts the next request to a prepare,
- appends the prepare to the write ahead log (WAL), assigning it the next sequence number and
  adding a checksum "pointer" to the previous log entry,
- replicates prepare across backups.

When a backup receives a prepare, it writes it to the WAL section of the data file and sends a
`prepare_ok` message to the primary. Once the primary receives a quorum of `prepare_ok` messages,
the prepare is considered committed, meaning that it can no longer be removed from or reordered in
the log.

Replicas execute committed prepares in sequence number order, by applying a batch of transfers to
local state. Because all replicas start with the same (empty) state, and because the state
transition function is deterministic, the replicas arrive at the same state.

If a primary fails, the consensus algorithm proper ensures that a different replica becomes a
primary, and that the new primary correctly reconstructs the latest state of the log.

The derived state of the system is the append-only log of immutable transfers (past transfers are
stored for idempotence) and the current balances of all accounts. Physically on disk, the state is
stored as a collection (a forest) of LSM trees. Each LSM tree is a sorted collection of objects.
For example, transfer objects are stored in a tree sorted by a unique transfer's timestamp. This
tree allow efficiently finding the transfer given a timestamp. Auxiliary index trees are used to
speed up other kinds of lookups. For example, there's a tree which stores, for each transfer, a
tuple of transfer's debit account id and transfer's timestamp, sorted by account id. This tree
allows looking up all the timestamps for transfers from a specific account. Knowing the timestamp,
it is then possible to retrieve the transfer object itself.

The forest of LSM trees is implemented as an on-disk functional data structure. That is, the data is
organized as a tree of on-disk blocks (a block is 0.5 MiB in size). Blocks refer to other blocks by
their on-disk address and checksum of their content. There's a root block, called the superblock,
from which the rest of the state is reachable.

When applying prepares to local state, replica doesn't overwrite any existing blocks in the data
file, and instead writes new blocks only, maintaining the state of the superblock in memory. Once in
a while, the superblock is atomically flushed to storage, forming a new checkpoint. When a replica
crashes and restarts, it of course looses access to its previous in-memory superblock. Instead, it
reads the previous checkpoint/superblock from storage, and reconstructs the lost state by replaying
the suffix of the log of prepares after that checkpoint. Determinism guarantees that the replica
ends up in the exact same state.

TigerBeetle assumes that replica's storage can fail. If a replica writes a prepare to the WAL or a
block of an LSM trees and the corresponding `fsync` returns `0`, it still could be the case that,
when reading this data later, it will be found corrupted. Given that the system is already
replicated for high-availability, it would be wasteful to not use redundancy to repair local storage
failures, and that's exactly what TigerBeetle does.

## Design Decisions

### Intentionality in the Design

A lot of software is designed primarily via empirical feedback loop --- apply a small change to the
software, and, judging the results, revert or double down. For TigerBeetle, we are trying something
different: think from the first principles what the right solution _should_ be, and then use
"experiments" to confirm or disprove the mental model.

For example, our [TIGER_STYLE](./TIGER_STYLE.md) is an explicitly engineered engineering process.

### As Fast as a Hash Table

Here's a mental model for TigerBeetle: a good way to solve financial transactions is an in-memory
hash map that stores accounts keyed by their IDs. Processing a transfer is then two hash-map
lookups, a balance check, and two balance updates. This is the "speed-of-light" for the problem ---
any other solution wouldn't be significantly faster.

TigerBeetle improves on the in-memory hash-table across two axis:

**Persistence and High Availability:** an in-memory hash table is good until you need to reboot the
computer. Data in TigerBeetle is stored on disk, so power cycles are safe. What's more, the data is
replicated across six replicas, so, even if some of them are down, the cluster as a whole remains
available.

**Large Data Sets:** an in-memory hash table is good until your data stop fitting in RAM.
TigerBeetle allows working with datasets larger-than-memory. That being said, to keep optimal
performance, the hot path of the dataset should fit in RAM.

### Don't Waste Durability

The function of consensus algorithm is to convert durability into availability. Data placed on
durable storage is valuable, and should be utilized fully.

In particular, if an individual block on any replica bit rots, it would be extremely wasteful to not
repair this block using equivalent data from other replicas in the cluster.

### Non-Interactive Transactions

The primary paradigm for OLGP is interactive transactions. To implement a bank transfer in a
general-purpose relational database, application:

1. Opens a transaction
2. Fetches balances from the database
3. Computes balance update in the application
4. Sends updates to the database
5. Commits the transaction

Crucially, step 2 acquires a lock on the balances, which is not released until step 5. That is, a
lock is held over a network round-trip.

This approach gives good performance if most transactions are independent (e.g. are mostly reads).
For financial accounting, the opposite is true --- most transactions are writes, and they conflict
due to popular accounts. For this reason, TigerBeetle always executes a transaction directly inside
the database, avoiding moving data to code over the network.

### Single Thread

TigerBeetle is single-threaded. There are both positive and negative reasons for this design choice.

The primarily negative reason is that the underlying workload is inherently contentious: some
accounts are just way more popular than others, and transfers between hot accounts inherently
sequentialize the system. Trying to make transactions parallel not only doesn't make it faster, in
fact, the overhead of synchronization tends to dominate useful work. Formally, our claim is that
financial transaction processing is an infinite-COST problem, using the terminology from [Frank
McSherry's paper](https://www.usenix.org/system/files/conference/hotos15/hotos15-paper-mcsherry.pdf).

The positive reason for using a single thread is that CPUs are quite fast actually, and the reports
of Moore's Law demise are somewhat overstated. A single core can easily scale to 1mil TPS if:

- you keep that core busy with the useful work and move everything else off the hot path,
- you use the core efficiently and do your home work with cache line aligned data structures, memory
  prefetching, SIMD and other boring performance engineering 101.

Additionally, keeping the system single threaded greatly simplifies the programming model, making
testing much more efficient.

### Static Memory Allocation

We say that TigerBeetle doesn't use `malloc` and `free`, and that instead it does "static memory
allocation". This is a bit idiosyncratic use of the term, so let's be precise about what TigerBeetle
does and does not do first.

Unlike some embedded systems, TigerBeetle doesn't use global statics (`.bss` section) for allocation.
The memory usage of TigerBeetle depends on the CLI arguments passed at runtime.

TigerBeetle also doesn't do arena allocation. One way you can ensure that an application uses at
most, say, 4 GiB of memory is by allocating an appropriately fixed-sized arena at the start, and
using this arena for all allocations, failing with out of memory error if more than 4 GiB has to be
allocated.

In TigerBeetle, static memory allocation is not a goal, but the consequence of the fact that
everything has an explicit upper bound.

When TigerBeetle starts, it looks at the CLI arguments and then computes, for every programming
"object" in the system, how many such objects would be needed in the worst-case, and preallocates
all of the objects. After startup, no new objects are created, and, as a consequence, no dynamic
memory allocation is needed. The upper bounds are computed using a combination of static and dynamic
information.

Knowing the limits is useful, because it ensures that the system continues to function correctly
even when overloaded. For example, TigerBeetle doesn't need to have _explicit_ code for handling
backpressure --- if everything has a limit, there's nothing to grow without bound to begin with.
Backpressure arises naturally as the property of the entire system from the pairwise interactions
between components which need to honor each-other's limits.

Here's another interesting consequence of static limits. One problem in highly concurrent
applications is runaway concurrency --- spawning more concurrent tasks than the number of underlying
resources available which leads to oversubscription. A concurrent task is usually a closure
allocated somewhere on the heap and registered with an event loop, a `Box<dyn Future>`. Because
TigerBeetle _can't_ heap allocate structures at runtime, it follows that each TigerBeetle "future"
is represented by an explicit struct which is statically allocated as a field of a  component that
owns the structure. So there's always a natural limit of how many concurrent tasks can be in flight!

In summary, **static allocation is not the goal, but rather a forcing function to ensure that
everything has a limit**, and a natural consequence of the limits.

Somewhat surprisingly, our experience is that static allocation also simplifies the system greatly.
You need to spend more time up-front thinking through what data you need to store where, but, after
the initial design, the interplay of the connected limits between the components tends to just work
out.

Of course static allocation makes the system faster, and greatly reduces latency variation, but this
also is just a consequence of knowing the limits, the "physics" of the underlying system.

### No Dependencies

TigerBeetle avoids dependencies. It depends on:

- the Linux kernel API (in particular, on io_uring) to make hardware do things,
- the Zig compiler, to convert from human-readable source code to machine code,
- parts of Zig standard library, for various basic appliances like sorting algorithms or hash maps.

The usefulness of dependencies is generally inversely-proportional to the lifetime of the project.
For longer lived projects, it makes more and more sense to control more of the underlying moving
parts. TigerBeetle is explicitly engineered for the long-term, so it just makes sense to start
building all the necessary infrastructure today. This removes temptation to compromise --- static
allocation is done throughout the stack, because we wrote most of the stack. It is a pleasant
intentional coincidence that Zig's standard library APIs are compatible with static allocation.

Avoiding dependencies also acts as a forcing function for keeping the code simple and easy to
understand. Extra complexity can't sneak into the codebase hidden by a nifty API. It turns out that
most of the code in the database isn't _that_ hard.

### Zig

As follows from the [static allocation](#static-memory-allocation) section, TigerBeetle doesn't need
a garbage collector. While it is possible to do static allocation in a GC language, it makes it much
harder to guarantee that no allocation actually happens. This significantly narrows down the choice
of programming languages. Of the languages that remain, Zig makes the most sense, although Rust is a
close contender.

Both Zig and Rust provide spatial memory safety. Rust has better temporal and thread safety, but
static allocation and single-threaded execution reduce the relative importance of these benefits.
Additionally, mere memory safety would a low bar for TigerBeetle. General correctness is table
stakes, requiring a comprehensive testing strategy which leaves relatively narrow space for bugs to
escape testing, but be caught by Rust-style type system.

The primary benefit of Zig is the favorable ratio of expressivity to language complexity.
"Expressivity" here means ability to produce the desired machine code, not source-level
abstractions. Zig is a DSL for machine code, its comptime features makes it very easy to _directly_
express what you want the computer to do. This expressivity comes at the cost of missing
declaration-site interfaces, but this is less important in a zero-dependency context.

Zig provides excellent control over layout, alignment, and padding. Alignment-carrying pointer types
prevent subtle errors. Idiomatic Zig collections don't have an allocator as a parameter of
constructor, and instead explicitly pass allocator to the specific methods that require allocation,
which is a perfect fit for the memory management strategy used in TigerBeetle. Cross compilation
that works and direct (glibc-less) bindings to the kernel help keep dependency count down.

Most importantly, this all is possible using very frugal language machinery. Zig lends itself very
well to low-abstraction first-order code that does the work directly, which makes it easy to author
and debug performance-oriented code.

### Determinism

A meta principle above "static allocation" is determinism --- a property that, given the same
inputs, the software not only gives the same logical result, but also arrives at the result using
the same physical path. There's no single specific reason to demand determinism, but it tends to
consistently simplify and improve the system. In general everything in TigerBeetle is deterministic,
but here are some of the places where that determinism leads to big advantages:

- Determinism greatly simplifies the implementation of a replicated state machine, as it reduces the
  problem of synchronizing mutable state to a much simpler problem of synchronizing an immutable,
  append-only, hash-chained log.
- Determinism allows for physical repair, as opposed to logical repair. Consider the case where a
  single byte of the storage of a particular LSM tree got corrupted. If the replicas only guarantee
  logical consistency, repairing this byte might entail re-transmitting the entire tree, which is
  slow, and might fail in presence of uncorrelated faults.

  In contrast, if the replicas, on top of logical consistency, guarantee that the bytes on disk
  representing the data are _also_ the same, then, for repair, it becomes sufficient to transfer
  just a single disk block containing the problematic byte. This is the approach taken by
  TigerBeetle. It is guaranteed that replicas in the cluster converge on byte-for-byte identical LSM
  tree structure.
- Physical repair in turn massively simplifies error handing. The function that reads data from
  storage doesn't have an error condition. It _always_ receives the block with the requested
  checksum, but the block could be transparently read from a different replica.
- Physical determinism requires that LSM compaction work is scheduled deterministically, which
  ensures that compaction work is evenly spread throughout the operation, bounding the worst-case
  latencies.
- Determinism supercharges randomized testing, as any test failure can be reliably reproduced by
  sharing a seed that lead to the failure.

### Simulation Testing

TigerBeetle uses a variety of techniques to ensure that the code is correct, from good old
example-based tests to strict style guide which emphasizes simplicity and directness. The most
important technique deployed is simulation testing, as seen on <https://sim.tigerbeetle.com>.

TigerBeetle's simulator, the VOPR, can run an entire cluster on a single thread, injecting various storage
faults and infinitely speeding up time. Combined with a smart workload generator, swarm testing, and
a thousand CPU cores, VOPR makes is easy to exercise all the possible behaviors of the system.

Crucially, unlike formal proofs and model checking, the simulation testing exercises a specific
implementation. Tools like TLA are invaluable to debug an algorithm, but are of relatively little
help if you want to check if your code implements the algorithm correctly, or to verify that the
_assumptions_ behind the algorithm hold.

### Mechanical Sympathy

Mechanical Sympathy is the idea that, although a CPU is a general purpose device and can execute
anything, it is often possible to re-formulate a particular algorithm in a CPU-friendly manner,
which makes it much faster to run. Small details matter a lot for speed, and sometimes these small
details guide larger architecture.

In the context of TigerBeetle, mechanical sympathy spans all for primary colors of computation:

- Network
- Storage
- Memory
- CPU

Some examples:

Network has limited bandwidth. This means that if one node in the network requires much higher
bandwidth then the rest, the network will be underutilized. For this reason, when the primary needs
to replicate a prepare across the cluster, a ring topology is used: the primary doesn't broadcast
the prepare to every other replica, and instead sends it to just one other replica, relying on that
replica to forward the prepare farther.

Storage is typically capable of sustaining several parallel IO operations, so TigerBeetle tries to
keep it saturated. For example, when compaction needs to read a table from disk, it enqueues writes
for several of the table's blocks at a time. At the same time, care is taken to not oversubscribe
the storage --- there's a limit on the maximum number of concurrent IO operations.

Memory is not fast --- operations that miss the cache and hit the memory are dramatically slower.
TigerBeetle data structures are compact, to maximize the chance that the data is cache, and are
organized around cache lines, such that the all data for a particular operation can be found in a
single cache line, as opposed to be more spread out in memory. For example, the size of a Transfer
object is two cache lines.

CPU can both sprint and parkour, but it is so much better at sprinting! CPU is very fast at straight
line code, and can sweep several lanes at once with SIMD, but `if`s (especially unpredictable ones)
make it stagger. When processing events, where each event is either a new account or a new transfer,
TigerBeetle lifts the branching up. Events come in batches, and each batch is homogeneous ---
either all transfers, or all accounts. So the event-kind branching is moved out of the inner loop.

### Batching

One of the most effective optimization principles is the idea of amortizing the overhead --- if you
have to do a costly operation, make sure that its results "pay for" many smaller useful operation.
TigerBeetle uses batching at many different levels:

- Individual transfers are aggregated into a prepare, to amortize replication and prefetch IO
  overhead.
- Changes from 32 prepares are aggregated in memory before being written to disk together.
- Changes from 1024 prepares are aggregated before checkpoint is atomically advanced by overwriting
  the superblock.
- The LSM tree operates mostly in terms of tables and value blocks, each aggregating many individual
  records.

### LSM

TigerBeetle organizes data on disk as a collection of
[Log Structured Merge Trees](https://www.youtube.com/watch?v=hkMkBZn2mGs). In the context of
TigerBeetle, LSM has several attractive properties:

- It is particularly good at writes, and TigerBeetle's workload is write-heavy.
- LSM is organized around batches of data, just like the rest of TigerBeetle.
- LSM naturally keeps hot data near the root, while the colder data sink down to the lower level,
  which also matches Pareto-distributed workload.

Often, a database is just a single LSM tree, and individual "tables" are constructed logically, by
using key prefixes. This is not the case for TigerBeetle and instead many individual trees are used
(the LSM forest). These trees store fixed-sized values. For example in the Transfer object tree the
value is 128 bytes, for id tree the size of value is 32 bytes (`u128` id, `u64` timestamp, `u64`
padding). Each tree can then be specialized for that specific value size, which improves performance
and storage efficiency. Zig's `comptime` makes is particularly easy configure the LSM Forest through
meta programming.

### Control Plane / Data Plane Separation

Batching is a special case of the more general principle of separating control plane and data plane.
Imagine yourself as a pilot in a cabin of a powerful jet. The cabin has all sorts of dials, levers
and buttons. By pressing the buttons, you control the jet engines. Most of the work of moving the
plane is done by the engines, the data plane. The pilot doesn't do the heavy lifting, they direct
the power of the engine.

In the context of TigerBeetle, deciding which prepare to apply would be control plane, but actually
going through each individual transfer is data plane. In general, control plane is O(1) to data
plane's O(N).

It is important to clearly separate the two mods of operation. Keeping control plane `if`s outside
of data plane `for`s keeps the inner loop free of branching, improving performance. Conversely, as
the overhead of control plane is small, it can use very aggressive assertions, up to spending O(N)
time to verify O(1) operation.

### Synchronous Execution

StateMachine's `commit` function, the one that actually implements double-entry accounting, is
synchronous. It takes a slice of transfers as input, and, in a single tight CPU loop, decides, for
each transfer, whether it is applicable, computes the desired balance change if it is, and records
a result in the output slice. Crucially, the `commit` function itself doesn't do any reading from
storage. This is the key to performance.

Instead, all IO happens in the separate prefetch phase. Given a batch of transfers, it is possible
to predict which accounts need to be fetched without actually executing the transfers. What's more,
while commit execution has to happen sequentially, all prefetch IO can happen in parallel.

### Embracing Concurrency

Although TigerBeetle uses sequential execution as a simple and performant way to achieve strict
serializability semantics, it doesn't mean that _everything_ has to be sequential. For example, as
the previous section demonstrates, it is possible to simultaneously:

- fetch data from storage in parallel,
- apply double-entry accounting rules to transfers on the CPU sequentially.

This pattern generalizes: TigerBeetle embraces concurrency. Sequential execution is the exception.

- In the VSR protocol, prepares are replicated concurrently. That is, when a primary receives a
  client request, assigns it an op-number and starts a replication loop, it doesn't wait for the
  replication to finish before starting to work on the next request. _Execution_ of requests has to
  be sequential, but replication can be concurrent.

- Similarly, for a new prepare the primary concurrently:
  - writes it to its local storage,
  - and starts the replication loop.

  Because the prepare is considered committed when the primary receives a quorum of `prepare_ok`
  from a set of replicas which doesn't necessarily include the primary itself, it can the case that
  the primary concurrently executes a prepare while still writing corresponding message to the Write
  Ahead Log (WAL).

- A similar pipelining structure works in LSM compaction. Compaction is a batched two-way merge
  operation. It merges two sorted sequences of values, where values come from blocks on disk. The
  resulting sequence of values is likewise split into blocks which are written to disk. Although the
  merge needs to be sequential, it is possible to fetch several blocks from disk at the same time.
  With some more legwork, it is possible to structure compaction such that reading blocks of values
  from disk, merging values in memory, and writing the resulting blocks to disk can all happen
  concurrently.

### io_uring

TigerBeetle use io_uring exclusively for IO. It is a perfect interface for TigerBeetle, as it
combines [batching](#batching) and [concurrency](#embracing-concurrency). At the micro level, the
code in TigerBeetle isn't a good fit for coroutines or threads, the concurrency is very
fine-grained, at the level of individual syscall. For example, for pipelined compaction, the natural
way to write code is to issues two concurrent syscalls for reading the data from the corresponding
levels, and that's more or less exactly what io_uring exposes as an interface to the programmer.

io_uring is _also_ the only reasonable way to have truly asynchronous disk io on Linux, and comes
with improved throughput to boot, but these benefits are secondary to the interface being a natural
fit for the problem.

### Time

Accounting business logic needs access to wall-clock time for transfer timeouts. The state machine
can't access OS to get time directly, as that would violate determinism. Instead, TigerBeetle
primary injects a specific timestamp into the logic of state machine when it converts a request to a
prepare.

The ultimate source of time for TigerBeetle cluster is Network Time Protocol (NTP). A potential
failure mode with NTP is primary partitioned from NTP servers. To make sure that the primary's clock
is within acceptable error margin, TigerBeetle cluster aggregates timing information from a
replication quorum of replicas. TigerBeetle also guarantees that the time, observed by the state
machine, is strictly monotonic.

This high-quality time implementation is than utilized not only for baseness logic (timeouts), but
also plays a key role in the internal implementation. Every object in TigerBeetle has a globally
unique `u64` creation timestamp, which plays the role of synthetic primary key.

### Direct IO

TigerBeetle bypasses the operating system's page cache and uses Direct IO. Normally, when an
application writes to a file, the operating system only updates the in-memory cache, and the data
gets to disk later. This is a good default for the vast majority of the applications. TigerBeetle is
an exception --- it instructs the operating system to read directly from the disk, and write directly
to the disk, bypassing any caches (please refer to this
[excellent article](https://transactional.blog/how-to-learn/disk-io) for the overview of the
relevant OS APIs).

Bypassing page cache is required for correctness. While operating systems provide an `fsync` API to
flush page cache to disk, it doesn't allow handling errors reliably:
[Can Applications Recover from fsync Failures?](https://www.usenix.org/system/files/atc20-rebello.pdf)

The second reason to bypass the cache is the general principle of avoiding dependencies and assuming
less about the interface. Concretely, TigerBeetle doesn't require the OS to provide page cache,
and it doesn't require a file system by virtue of using only a single file. As a consequence,
TigerBeetle can run directly against a block device.

### Flexible Quorums

There's something odd about a TigerBeetle cluster --- we recommend using an even number of replicas,
`6`. Usually, consensus implementations use `2f + 1` nodes, `3` or `5`, to have a clear majority for
quorums. TigerBeetle uses so-called flexible quorums. For `6` replicas, the replication quorum is
only `3`. It is enough for only a half of the cluster to persist a prepare durably to disk
for it to be considered logically committed. On the other hand, for changing views (that is, rotating
the role of the primary to a different replica), at least `4` replicas are needed. Because any
subset of three replicas and any subset of four replicas intersect, the new primary is guaranteed to
know all potentially committed prepares.

The upshot here is that it's enough for `3` replicas to be online, including the primary, to keep
the cluster available. If in a cluster of `6`, three replicas crash simultaneously, there's a
50% chance that the cluster remains available. If the replicas are crashing one by one, the chance
is higher: when only `4` replicas remain, the chance that the next one to crash would be a primary
is only 25%.

### Protocol Aware Recovery

TigerBeetle assumes that the disk can fail, and that the data can be physically lost eventually even
if the original `write` and `fsync` completed successfully. If that happens, TigerBeetle
transparently repairs faulty disk sectors using identical data present on the other replicas.

Faulty storage can not be fully encapsulated by the storage interface, and requires consensus
cooperation to resolve. Here's a useful counter-example. Suppose that the primary accepts a request
from the client, converts it to a prepare by assigning it a specific op number, and starts the
replication procedure. During replication, the primary successfully appends the prepare to its local
WAL, but fails to broadcast it to other replicas. Now, the primary restarts, for whatever reason,
the cluster switches to a different primary, and, meanwhile, the prepare gets corrupted in the
original primary's WAL. As the request was prepared, the prepare should make it into the new view.
But because there's only one copy of prepare in the cluster, and it got corrupted, it is impossible
to execute this prepare.

This is the key difficulty: _potentially_ committed prepares are not necessary replicated to a full
replication quorum, and, as such, their durability is reduced. Corruption of a potentially committed
prepare requires special handling.

The solution is to use the NACK protocol: during a view change, participating replica can positively
state that it _never_ accepted a particular prepare. If at least `4` out of `6` replicas NACK the
prepare, then it can be inferred that the prepare was never replicated fully, and it can be safely
discarded, even if it is corrupted.

## References

The collection of papers behind TigerBeetle:

- [LMAX - How to Do 100K TPS at Less than 1ms Latency -
  2010](https://www.infoq.com/presentations/LMAX/) - Martin Thompson on mechanical sympathy and why
  a relational database is not the right solution.

- [The LMAX Exchange Architecture - High Throughput, Low Latency and Plain Old Java -
  2014](https://skillsmatter.com/skillscasts/5247-the-lmax-exchange-architecture-high-throughput-low-latency-and-plain-old-java)

  - Sam Adams on the high-level design of LMAX.

- [LMAX Disruptor](https://lmax-exchange.github.io/disruptor/files/Disruptor-1.0.pdf) - A high
  performance alternative to bounded queues for exchanging data between concurrent threads.

- [Evolution of Financial Exchange Architectures -
  2020](https://www.youtube.com/watch?v=qDhTjE0XmkE) - Martin Thompson looks at the evolution of
  financial exchanges and explores the state of the art today.

- [Gray Failure](https://www.microsoft.com/en-us/research/wp-content/uploads/2017/06/paper-1.pdf) -
  "The major availability breakdowns and performance anomalies we see in cloud environments tend to
  be caused by subtle underlying faults, i.e. gray failure rather than fail-stop failure."

- [The Tail at Store: A Revelation from Millions of Hours of Disk and SSD
  Deployments](https://www.usenix.org/system/files/conference/fast16/fast16-papers-hao.pdf) - "We
  find that storage performance instability is not uncommon: 0.2% of the time, a disk is more than
  2x slower than its peer drives in the same RAID group (and 0.6% for SSD). As a consequence, disk
  and SSD-based RAIDs experience at least one slow drive (i.e., storage tail) 1.5% and 2.2% of the
  time."

- [The Tail at
  Scale](https://www2.cs.duke.edu/courses/cps296.4/fall13/838-CloudPapers/dean_longtail.pdf) - "A
  simple way to curb latency variability is to issue the same request to multiple replicas and use
  the results from whichever replica responds first."

- [Viewstamped Replication Revisited](http://pmg.csail.mit.edu/papers/vr-revisited.pdf)

- [Viewstamped Replication: A New Primary Copy Method to Support Highly-Available Distributed
  Systems](http://pmg.csail.mit.edu/papers/vr.pdf)

- [Flexible Paxos: Quorum intersection revisited](https://arxiv.org/pdf/1608.06696v1)

- [Scalability! But at what COST?](https://www.usenix.org/system/files/conference/hotos15/hotos15-paper-mcsherry.pdf)

- [ZFS: The Last Word in File Systems (Jeff Bonwick and Bill
  Moore)](https://www.youtube.com/watch?v=NRoUC9P1PmA) - On disk failure and corruption, the need
  for checksums... and checksums to check the checksums, and the power of copy-on-write for
  crash-safety.

- [An Analysis of Latent Sector Errors in Disk
  Drives](https://research.cs.wisc.edu/wind/Publications/latent-sigmetrics07.pdf)

- [An Analysis of Data Corruption in the Storage
  Stack](https://www.usenix.org/legacy/events/fast08/tech/full_papers/bairavasundaram/bairavasundaram.pdf)

- [A Study of SSD Reliability in Large Scale Enterprise Storage
  Deployments](https://www.usenix.org/system/files/fast20-maneas.pdf)

- [SDC 2018 - Protocol-Aware Recovery for Consensus-Based Storage](https://www.youtube.com/watch?v=fDY6Wi0GcPs)
  ([pdf](https://www.usenix.org/conference/fast18/presentation/alagappan)) - Why replicated state
  machines need to distinguish between a crash and corruption, and why it would be disastrous to
  truncate the journal when encountering a checksum mismatch.

- [Can Applications Recover from fsync
  Failures?](https://www.usenix.org/system/files/atc20-rebello.pdf) - Why we use Direct I/O in
  TigerBeetle and why the kernel page cache is a dangerous way to recover the journal, even when
  restarting from an fsync() failure panic.

- [Coil's Mojaloop Performance Work
  2020](https://docs.mojaloop.io/legacy/discussions/Mojaloop%20Performance%202020.pdf) - By Don
  Changfoot and Joran Dirk Greef, a performance analysis of Mojaloop's central ledger that sparked
  the idea for "an accounting database" as Adrian Hope-Bailie put it. And the rest, as they say, is
  history!
