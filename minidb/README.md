# minidb

A small **disk-based storage engine** in C++17: a page-oriented **B+ tree** (int64 key ->
short string value) on top of an **LRU buffer pool** and a **pager** that reads and writes
fixed 4 KiB pages. It is the C++ continuation of a pointer-based B+ tree: same data
structure, but now nodes live in a file and only a bounded number of them are in memory.

```
$ ./minidb demo.db 64
> put 5 hello
inserted
> scan 0 10
5 -> hello
> stats
entries 1, height 1, pages 2
cache: hits 11, misses 0, hit rate 100%, evictions 0
disk: reads 0, writes 2
> check
ok: all B+ tree invariants hold
```

## Layers

| Layer | File | Responsibility |
|---|---|---|
| Pager | `pager.cpp` | file as an array of 4 KiB pages: allocate, read, write, sync |
| Buffer pool | `buffer_pool.cpp` | write-back LRU cache (list + hash map, O(1) hit/evict), dirty-page write-back, hit/miss/eviction statistics |
| B+ tree | `bplustree.cpp` | search, insert with node splits (root splits grow the tree), overwrite, lazy delete, range scan over the leaf chain, structural `validate()` |
| Shell | `main.cpp` | `put / get / del / scan / stats / check` |

On-disk format (all integers little-endian, so files are portable):

- **Page 0 (header):** magic `MINIDB01`, page size, root page id, node capacities, entry count.
- **Leaf page:** `nkeys`, next-leaf id, then `nkeys` x { key i64, length u8, value[56] }.
- **Internal page:** `nkeys`, then keys, then `nkeys + 1` child page ids.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/minidb_bench

# sanitizers
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMINIDB_SANITIZE=address,undefined
```

## How correctness is checked

- `BPlusTree::validate()` walks the whole tree and checks: keys strictly increasing in every
  node, every key inside its subtree's separator bounds, all leaves at the same depth, node fill
  limits, the leaf chain is globally sorted, and the number of entries matches the header.
- **Differential tests** against `std::map`: 5,000 random inserts and a 30,000-operation random
  insert/erase mix with deliberately tiny nodes (3-5 keys) so the tree is tall and splits happen
  constantly; range scans are compared against `std::map::lower_bound/upper_bound`.
- A 3-page buffer pool on a much taller tree (constant eviction) must still give correct results.
- Close/reopen persistence: the file's own node capacities override the options passed in, and
  everything (including data written after the reopen) is still there on the next open.
- Corrupt, foreign, or truncated files are rejected; extreme keys (INT64_MIN/MAX) and the 56-byte
  value limit are tested. Everything runs under AddressSanitizer + UBSan.

## Benchmark (one machine, Release; 100,000 random inserts, lookups, 2,000 range scans)

| Cache (pages) | Insert hit rate | Lookup hit rate | Disk reads | Lookups/s |
|---|---|---|---|---|
| 4 | 41% | 53% | 363,781 | ~138k |
| 32 | 67% | 73% | 208,216 | ~139k |
| 256 | 77% | 78% | 157,486 | ~157k |
| 4096 (whole tree fits) | 100% | 100% | 0 | ~197k |

Tree: height 3, 2,397 pages. "Disk reads" counts page reads issued to the file; the operating
system's own page cache hides most of their latency, so wall-clock gains are smaller than the
I/O reduction. Numbers are machine dependent: run `minidb_bench` yourself.

## Limitations (honest list)

- **No crash safety:** there is no write-ahead log, so a crash in the middle of a split can leave
  the file inconsistent. Pages are only guaranteed on disk after `flush()` / destruction.
- **Lazy deletes:** removing a key never merges or rebalances nodes and never frees pages, so a
  file does not shrink after deletions (the tree stays correct; `validate()` allows underfull and
  empty leaves).
- Single-threaded; one writer, no transactions, no concurrency control.
- Fixed-size entries: int64 keys and values up to 56 bytes. Pages are copied in and out of the
  pool rather than pinned, which is simple and correct but not zero-copy.
- No SQL layer.

## Ideas for extending it

A write-ahead log with recovery (the natural next step), page merging/redistribution on delete
and a free-page list, pinned pages instead of copies, variable-length records with a slotted
page layout, latch coupling for concurrent readers, a tiny query layer on top.
