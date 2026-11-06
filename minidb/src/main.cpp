// Tiny interactive shell:  minidb <file> [cache_pages]
//   put <key> <value...> | get <key> | del <key> | scan <lo> <hi> | stats | check | quit
#include <cstdlib>
#include <iostream>
#include <sstream>

#include "minidb/bplustree.hpp"

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: minidb <file> [cache_pages]\n"; return 2; }
    try {
        minidb::BPlusTree::Options opts;
        if (argc > 2) opts.cache_pages = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
        minidb::BPlusTree db(argv[1], opts);
        std::cout << "minidb: " << db.size() << " entries, height " << db.height()
                  << ". Commands: put get del scan stats check quit\n";
        std::string line;
        while (std::cout << "> " && std::getline(std::cin, line)) {
            std::istringstream ss(line);
            std::string cmd;
            if (!(ss >> cmd)) continue;
            try {
                if (cmd == "quit" || cmd == "exit") break;
                if (cmd == "put") {
                    long long k; std::string v;
                    if (!(ss >> k)) { std::cout << "usage: put <key> <value>\n"; continue; }
                    std::getline(ss >> std::ws, v);
                    std::cout << (db.put(k, v) ? "inserted" : "updated") << "\n";
                } else if (cmd == "get") {
                    long long k;
                    if (!(ss >> k)) { std::cout << "usage: get <key>\n"; continue; }
                    auto v = db.get(k);
                    std::cout << (v ? *v : std::string("(not found)")) << "\n";
                } else if (cmd == "del") {
                    long long k;
                    if (!(ss >> k)) { std::cout << "usage: del <key>\n"; continue; }
                    std::cout << (db.erase(k) ? "deleted" : "(not found)") << "\n";
                } else if (cmd == "scan") {
                    long long lo, hi;
                    if (!(ss >> lo >> hi)) { std::cout << "usage: scan <lo> <hi>\n"; continue; }
                    for (auto& [k, v] : db.scan(lo, hi)) std::cout << k << " -> " << v << "\n";
                } else if (cmd == "stats") {
                    const auto& s = db.cache_stats();
                    std::cout << "entries " << db.size() << ", height " << db.height() << ", pages " << db.page_count()
                              << "\ncache: hits " << s.hits << ", misses " << s.misses << ", hit rate "
                              << static_cast<int>(s.hit_rate() * 100) << "%, evictions " << s.evictions
                              << "\ndisk: reads " << db.disk_reads() << ", writes " << db.disk_writes() << "\n";
                } else if (cmd == "check") {
                    const std::string e = db.validate();
                    std::cout << (e.empty() ? "ok: all B+ tree invariants hold" : "INVALID: " + e) << "\n";
                } else {
                    std::cout << "unknown command '" << cmd << "'\n";
                }
            } catch (const std::exception& e) {
                std::cout << "error: " << e.what() << "\n";
            }
        }
        db.flush();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
