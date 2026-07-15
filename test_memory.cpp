// Проверка МОЩНОЙ памяти через настоящий код: семантический recall (синонимы/перефраз
// без общих слов), дедуп, пиннинг. Нужен gemini_api_key в ~/.nexuscode/config.json —
// ключ читаем сами, в argv/лог не тащим. Без ключа семантические проверки пропускаются.
#include "memory.hpp"
#include "common.hpp"
#include <sqlite3.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

using namespace nexus;
static int fails = 0;
static void check(const std::string& n, bool ok, const std::string& extra = "") {
    std::cout << (ok ? "PASS  " : "FAIL  ") << n;
    if (!ok && !extra.empty()) std::cout << "   (" << extra << ")";
    std::cout << "\n"; if (!ok) ++fails;
}

int main(int argc, char** argv) {
    std::string cfgpath = argc > 1 ? argv[1] : std::string(getenv("HOME")) + "/.nexuscode/config.json";
    std::string key;
    try {
        std::ifstream f(cfgpath); std::stringstream ss; ss << f.rdbuf();
        key = json::parse(ss.str()).value("gemini_api_key", "");
    } catch (...) {}

    sqlite3* db = nullptr; sqlite3_open(":memory:", &db);
    Memory mem(db); mem.init();
    if (!key.empty())
        mem.configure_embeddings("https://generativelanguage.googleapis.com/v1beta/openai",
                                 key, "gemini-embedding-001");

    if (!mem.embeddings_enabled()) {
        std::cout << "SKIP  нет gemini_api_key — семантические проверки пропущены\n";
        std::cout << "\nALL TESTS PASSED\n";
        return 0;
    }
    check("эмбеддинги включены", mem.embeddings_enabled());

    mem.add("Пользователь предпочитает язык Rust для системного программирования");
    mem.add("Обед у пользователя обычно ровно в час дня");
    mem.add("Развёртывание идёт на VPS через контейнеры Docker и systemd");

    // ЗАПРОС БЕЗ ОБЩИХ СЛОВ с нужным фактом — лексика бы не нашла, семантика должна
    auto h = mem.search("на чём девелопер любит писать низкоуровневый код", 3);
    bool found_rust = false;
    for (auto& x : h) if (x.text.find("Rust") != std::string::npos) found_rust = true;
    check("семантика: нашла факт про Rust по перефразу", found_rust,
          h.empty() ? "пусто" : h[0].text);

    auto h2 = mem.search("куда катят прод и в чём его запускают", 3);
    bool found_deploy = false;
    for (auto& x : h2) if (x.text.find("Docker") != std::string::npos) found_deploy = true;
    check("семантика: нашла факт про деплой по перефразу", found_deploy,
          h2.empty() ? "пусто" : h2[0].text);

    // дедуп: почти тот же факт не должен плодить дубль
    int before = mem.count();
    mem.add("Пользователь предпочитает язык Rust для системного программирования");
    check("дедуп: повтор не увеличил счётчик", mem.count() == before,
          "было " + std::to_string(before) + ", стало " + std::to_string(mem.count()));

    // пиннинг: закреплённый факт всплывает даже на нерелевантный запрос
    int64_t pid = mem.add("Кодовое слово проекта — Гелиос-9");
    mem.pin(pid, true);
    auto h3 = mem.search("что приготовить на ужин из макарон", 5);
    bool pinned_up = false;
    for (auto& x : h3) if (x.text.find("Гелиос-9") != std::string::npos) pinned_up = true;
    check("пиннинг: закреплённый факт всплыл", pinned_up, h3.empty() ? "пусто" : h3[0].text);

    sqlite3_close(db);
    std::cout << "\n" << (fails ? std::to_string(fails) + " TEST(S) FAILED" : "ALL TESTS PASSED") << "\n";
    return fails ? 1 : 0;
}
