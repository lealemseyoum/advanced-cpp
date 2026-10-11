#include "arena.hpp"
#include <chrono>
#include <cstdio>
#include <list>
#include <memory_resource>
#include <string>
#include <vector>
template <class F> double best_ms(F f) { double b = 1e9; for (int r = 0; r < 7; ++r) { auto t0 = std::chrono::steady_clock::now(); f(); auto t1 = std::chrono::steady_clock::now(); b = std::min(b, std::chrono::duration<double, std::milli>(t1 - t0).count()); } return b; }
struct Node { long a, b; Node* next; };

int main() {
    volatile long sink = 0;
    constexpr int N = 1'000'000;
    // 1. build and free a 1M-node linked list, 5 times (alloc + free heavy)
    auto list_new = [&] { for (int r = 0; r < 5; ++r) { Node* h = nullptr; for (int i = 0; i < N; ++i) h = new Node{i, 0, h}; long s = 0; for (Node* p = h; p;) { s += p->a; Node* n = p->next; delete p; p = n; } sink = s; } };
    auto list_pool = [&] { edu::Pool<Node> pool(4096); for (int r = 0; r < 5; ++r) { Node* h = nullptr; for (int i = 0; i < N; ++i) h = pool.create(Node{i, 0, h}); long s = 0; for (Node* p = h; p;) { s += p->a; Node* n = p->next; pool.destroy(p); p = n; } sink = s; } };
    auto list_arena = [&] { edu::Arena a(1 << 20); for (int r = 0; r < 5; ++r) { Node* h = nullptr; for (int i = 0; i < N; ++i) h = new (a.allocate(sizeof(Node), alignof(Node))) Node{i, 0, h}; long s = 0; for (Node* p = h; p; p = p->next) s += p->a; sink = s; a.reset(); } };
    std::printf("1M-node list x5       new/delete %7.1f ms   Pool %7.1f ms   Arena(+reset) %7.1f ms\n", best_ms(list_new), best_ms(list_pool), best_ms(list_arena));

    // 2. std::pmr::list<int>: default resource vs monotonic vs our PoolResource (push 1M, clear, repeat 5)
    auto pmr_run = [&](std::pmr::memory_resource* mr) { std::pmr::list<int> l(mr); for (int r = 0; r < 5; ++r) { for (int i = 0; i < N; ++i) l.push_back(i); sink = l.back(); l.clear(); } };
    auto r_def  = [&] { pmr_run(std::pmr::new_delete_resource()); };
    auto r_pool = [&] { std::pmr::unsynchronized_pool_resource pr; pmr_run(&pr); };
    auto r_ours = [&] { edu::PoolResource pr; pmr_run(&pr); };
    std::printf("pmr::list<int> push/clear x5   new_delete %7.1f ms   std unsync_pool %7.1f ms   edu::PoolResource %7.1f ms\n", best_ms(r_def), best_ms(r_pool), best_ms(r_ours));

    // 3. short-lived many-strings request: arena vs default
    auto req_def = [&] { for (int r = 0; r < 20000; ++r) { std::vector<std::string> v; for (int i = 0; i < 50; ++i) v.push_back("a header value that exceeds sso " + std::to_string(i)); sink = v.size(); } };
    auto req_arena = [&] { edu::Arena a(64 * 1024); for (int r = 0; r < 20000; ++r) { { std::pmr::vector<std::pmr::string> v(&a); for (int i = 0; i < 50; ++i) v.emplace_back("a header value that exceeds sso " + std::to_string(i)); sink = v.size(); } a.reset(); } };
    std::printf("20k 'requests' of 50 strings   std::allocator %7.1f ms   Arena + reset %7.1f ms\n", best_ms(req_def), best_ms(req_arena));
}
