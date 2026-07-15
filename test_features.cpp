// Проверка новых фич через реестр: undo_file (откат правок) и read_pdf.
#include "tools.hpp"
#include "config.hpp"
#include "history.hpp"
#include "memory.hpp"
#include "util.hpp"
#include <sqlite3.h>
#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>

using namespace nexus;
static int fails = 0;
static void check(const std::string& n, bool ok, const std::string& extra = "") {
    std::cout << (ok ? "PASS  " : "FAIL  ") << n;
    if (!ok && !extra.empty()) std::cout << "   (" << extra << ")";
    std::cout << "\n"; if (!ok) ++fails;
}
static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

int main() {
    Config cfg; History hist(":memory:"); hist.open();
    sqlite3* db = nullptr; sqlite3_open(":memory:", &db);
    Memory mem(db); mem.init();
    json plan = json::array(); bool approve = true;
    ToolContext ctx{cfg, hist, mem, approve, plan};
    ToolRegistry reg; reg.register_all();

    const std::string path = "/tmp/nexus_undo_test.txt";
    auto readf = [&]{ std::ifstream f(path); std::string s((std::istreambuf_iterator<char>(f)), {}); return s; };

    // undo_file: правим файл и откатываем
    { std::ofstream f(path); f << "оригинал\nвторая строка\n"; }
    reg.call("edit_file", {{"path",path},{"old_string","оригинал"},{"new_string","ИЗМЕНЕНО"}}, ctx);
    check("edit применился", has(readf(),"ИЗМЕНЕНО"));
    std::string r = reg.call("undo_file", {{"path",path}}, ctx);
    check("undo_file: откатил правку", readf() == "оригинал\nвторая строка\n", r);

    // undo_file: несколько правок, откат по одной (LIFO)
    reg.call("edit_file", {{"path",path},{"old_string","оригинал"},{"new_string","A"}}, ctx);
    reg.call("edit_file", {{"path",path},{"old_string","вторая"},{"new_string","B"}}, ctx);
    reg.call("undo_file", {}, ctx);   // без пути — откат последней (B→вторая)
    check("undo_file: LIFO без пути откатил последнюю", has(readf(),"вторая") && has(readf(),"A"), readf());

    // undo delete
    { std::ofstream f(path); f << "жив\n"; }
    reg.call("delete_file", {{"path",path}}, ctx);
    check("delete удалил", !util::file_exists(path));
    reg.call("undo_file", {{"path",path}}, ctx);
    check("undo_file: восстановил удалённый файл", util::file_exists(path) && has(readf(),"жив"));
    std::remove(path.c_str());

    // read_pdf на реальном PDF
    const Tool* pdf = reg.find("read_pdf");
    check("read_pdf зарегистрирован", pdf != nullptr);
    std::string manual = "/usr/share/doc/speex/manual.pdf";
    if (util::file_exists(manual)) {
        std::string txt = reg.call("read_pdf", {{"path",manual}}, ctx);
        check("read_pdf: извлёк текст", txt.size() > 200 && !has(txt,"ошибка"), txt.substr(0,80));
    } else {
        std::cout << "SKIP  нет тестового PDF\n";
    }

    // новые тулы на месте
    check("screenshot зарегистрирован", reg.find("screenshot") != nullptr);
    check("generate_image зарегистрирован", reg.find("generate_image") != nullptr);

    std::cout << "\n" << (fails ? std::to_string(fails)+" TEST(S) FAILED" : "ALL TESTS PASSED") << "\n";
    return fails ? 1 : 0;
}
