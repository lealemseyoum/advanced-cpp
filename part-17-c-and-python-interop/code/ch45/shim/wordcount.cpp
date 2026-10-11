#include "wordcount.h"
#include <cctype>
#include <map>
#include <new>
#include <string>
#include <string_view>

struct wc_counter { std::map<std::string, size_t, std::less<>> words; };      // the C++ definition, invisible to C

namespace {
thread_local std::string g_last_error;
template <class F> wc_status guarded(F&& f) noexcept {          // THE boundary rule: no exception crosses extern "C"
    try { return f(); }
    catch (const std::bad_alloc&)  { g_last_error = "out of memory"; return WC_ERR_NOMEM; }
    catch (const std::exception& e) { g_last_error = e.what();       return WC_ERR_INTERNAL; }
    catch (...)                     { g_last_error = "unknown error"; return WC_ERR_INTERNAL; }
}
}

extern "C" {
wc_status wc_create(wc_counter** out) {
    if (!out) { g_last_error = "out is NULL"; return WC_ERR_ARG; }
    return guarded([&] { *out = new wc_counter; return WC_OK; });
}
void wc_destroy(wc_counter* c) { delete c; }                    // delete of nullptr is a no-op; the destructor cannot throw here
wc_status wc_add_text(wc_counter* c, const char* text, size_t len) {
    if (!c || (!text && len)) { g_last_error = "bad argument"; return WC_ERR_ARG; }
    return guarded([&] {
        std::string_view s(text, len);
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && !std::isalpha((unsigned char)s[i])) ++i;
            size_t j = i;
            while (j < s.size() && std::isalpha((unsigned char)s[j])) ++j;
            if (j > i) {
                std::string w(s.substr(i, j - i));
                for (auto& ch : w) ch = (char)std::tolower((unsigned char)ch);
                ++c->words[w];
            }
            i = j;
        }
        return WC_OK;
    });
}
wc_status wc_visit(const wc_counter* c, wc_visit_fn fn, void* user) {
    if (!c || !fn) { g_last_error = "bad argument"; return WC_ERR_ARG; }
    return guarded([&] {
        for (auto& [w, n] : c->words) if (fn(w.c_str(), n, user)) return WC_ERR_CALLBACK;     // the C callback stopped us
        return WC_OK;
    });
}
const char* wc_last_error(void) { return g_last_error.c_str(); }
const char* wc_status_string(wc_status s) {
    switch (s) { case WC_OK: return "ok"; case WC_ERR_NOMEM: return "out of memory"; case WC_ERR_ARG: return "bad argument";
                 case WC_ERR_CALLBACK: return "stopped by callback"; case WC_ERR_INTERNAL: return "internal error"; }
    return "?";
}
}
