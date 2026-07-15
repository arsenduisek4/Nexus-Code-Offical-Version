// Проверка Tier-0/Tier-1 улучшений через НАСТОЯЩИЙ код агента (линкуемся с src/*):
//   - read_file: нумерация строк <N>→text, offset/limit
//   - edit_file: точная замена, фаззи-фоллбэк по отступам, replace_all, отказы
//   - multi_edit: атомарная пачка правок (всё или ничего)
//   - repo_map: карта сигнатур по языкам, фокус-ранжирование
#include "tools.hpp"
#include "memory.hpp"
#include "config.hpp"
#include "history.hpp"
#include "util.hpp"
#include <sqlite3.h>
#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>

using namespace nexus;
static int fails = 0;
static void check(const std::string& name, bool ok, const std::string& extra = "") {
    std::cout << (ok ? "PASS  " : "FAIL  ") << name;
    if (!ok && !extra.empty()) std::cout << "   (" << extra << ")";
    std::cout << "\n";
    if (!ok) ++fails;
}
static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

int main() {
    Config cfg;
    History hist(":memory:"); hist.open();
    sqlite3* db = nullptr; sqlite3_open(":memory:", &db);
    Memory mem(db); mem.init();
    ToolRegistry reg; reg.register_all();
    json plan = json::array();
    bool approve = true;
    ToolContext ctx{cfg, hist, mem, approve, plan};

    const std::string path = "/tmp/nexus_t0.txt";
    auto writef = [&](const std::string& c){ std::ofstream f(path); f << c; };
    auto readf  = [&]{ std::ifstream f(path); std::string s((std::istreambuf_iterator<char>(f)), {}); return s; };

    // ── read_file: нумерация ──
    writef("alpha\nbeta\ngamma\n");
    std::string r = reg.call("read_file", {{"path",path}}, ctx);
    check("read_file: нумерует строки", has(r,"1→alpha") && has(r,"2→beta") && has(r,"3→gamma"), r);
    r = reg.call("read_file", {{"path",path},{"offset",2},{"limit",1}}, ctx);
    check("read_file: offset/limit", has(r,"2→beta") && !has(r,"alpha") && !has(r,"gamma"), r);

    // ── edit_file: точная уникальная замена ──
    writef("line one\nTARGET here\nline three\n");
    r = reg.call("edit_file", {{"path",path},{"old_string","TARGET here"},{"new_string","REPLACED"}}, ctx);
    check("edit: точная замена", readf()=="line one\nREPLACED\nline three\n", r);
    check("edit: сообщает строку 2", has(r,"строка 2"), r);

    // ── edit_file: фаззи-фоллбэк по отступам ──
    writef("void f() {\n        int x = 1;\n}\n");   // 8 пробелов отступа в файле
    r = reg.call("edit_file", {{"path",path},{"old_string","    int x = 1;"},{"new_string","    int x = 42;"}}, ctx);
    check("edit: фаззи по отступам сработал", has(readf(),"int x = 42;"), r);

    // ── edit_file: неоднозначность → отказ, файл цел ──
    writef("dup\ndup\nkeep\n");
    r = reg.call("edit_file", {{"path",path},{"old_string","dup"},{"new_string","X"}}, ctx);
    check("edit: неоднозначность → ошибка", has(r,"встречается 2 раз"), r);
    check("edit: файл не тронут при отказе", readf()=="dup\ndup\nkeep\n");
    r = reg.call("edit_file", {{"path",path},{"old_string","dup"},{"new_string","X"},{"replace_all",true}}, ctx);
    check("edit: replace_all заменил оба", readf()=="X\nX\nkeep\n", r);

    // ── edit_file: не найдено ──
    r = reg.call("edit_file", {{"path",path},{"old_string","ZZZ"},{"new_string","Y"}}, ctx);
    check("edit: не найдено → ошибка", has(r,"не найден"), r);

    // ── multi_edit: атомарная пачка ──
    writef("aaa\nbbb\nccc\n");
    json edits = json::array({
        {{"old_string","aaa"},{"new_string","111"}},
        {{"old_string","ccc"},{"new_string","333"}},
    });
    r = reg.call("multi_edit", {{"path",path},{"edits",edits}}, ctx);
    check("multi_edit: обе правки", readf()=="111\nbbb\n333\n", r);

    // multi_edit: если одна правка ломается — НИЧЕГО не пишем
    writef("aaa\nbbb\nccc\n");
    json edits2 = json::array({
        {{"old_string","aaa"},{"new_string","111"}},
        {{"old_string","NOPE"},{"new_string","x"}},   // не найдётся
    });
    r = reg.call("multi_edit", {{"path",path},{"edits",edits2}}, ctx);
    check("multi_edit: атомарность (откат при ошибке)", readf()=="aaa\nbbb\nccc\n", r);
    check("multi_edit: сообщает номер сломанной правки", has(r,"#2"), r);

    std::remove(path.c_str());

    // ── repo_map: карта текущего проекта ──
    r = reg.call("repo_map", {{"path","."}}, ctx);
    check("repo_map: находит src-файлы", has(r,"src/agent.cpp") || has(r,"agent.cpp"), r.substr(0,200));
    check("repo_map: тянет сигнатуры функций", has(r,"run_turn") || has(r,"system_prompt"), "");
    std::string rf = reg.call("repo_map", {{"path","."},{"focus","memory search cosine"}}, ctx);
    check("repo_map: фокус-ранжирование помечает ★", has(rf,"★"), rf.substr(0,120));

    // ── repo_map: однострочная C-функция `int main(){...}` (регресс терминатора) ──
    {
        std::string d = "/tmp/nexus_rm_test";
        std::system(("mkdir -p " + d).c_str());
        { std::ofstream f(d + "/one.cpp"); f << "#include <cstdio>\nint main(){printf(\"hi\");}\n"; }
        std::string rr = reg.call("repo_map", {{"path", d}}, ctx);
        check("repo_map: ловит однострочную C-функцию", has(rr,"one.cpp") && has(rr,"int main"), rr.substr(0,200));
        std::system(("rm -rf " + d).c_str());
    }

    // ── spawn_agent: регистрация и read-only guard ──
    const Tool* sa = reg.find("spawn_agent");
    check("spawn_agent зарегистрирован", sa != nullptr);
    check("spawn_agent немутирующий (параллелится)", sa && !sa->mutating);
    // ctx.delegate пуст (мы его не ставили) → tool должен вернуть «недоступно», а не падать
    r = reg.call("spawn_agent", {{"task","изучи X"}}, ctx);
    check("spawn_agent без delegate → безопасный отказ", has(r,"недоступн"), r);

    const Tool* me = reg.find("multi_edit");
    check("multi_edit зарегистрирован и мутирующий", me && me->mutating);

    std::cout << "\n" << (fails ? std::to_string(fails)+" TEST(S) FAILED" : "ALL TESTS PASSED") << "\n";
    return fails ? 1 : 0;
}
