// Проверка новой логики через НАСТОЯЩИЙ код агента (линкуемся с его объектниками):
//   - edit_file: уникальность old_string, replace_all, отчёт о строке
//   - Memory::search: реальная выборка релевантных фактов
//   - is_dangerous_command: детектор опасных команд (копия — функция file-local static)
#include "tools.hpp"
#include "memory.hpp"
#include "config.hpp"
#include "history.hpp"
#include "util.hpp"
#include <sqlite3.h>
#include <iostream>
#include <fstream>
#include <string>

using namespace nexus;
static int fails = 0;
static void check(const std::string& name, bool ok, const std::string& extra = "") {
    std::cout << (ok ? "PASS  " : "FAIL  ") << name;
    if (!ok && !extra.empty()) std::cout << "   (" << extra << ")";
    std::cout << "\n";
    if (!ok) ++fails;
}

// ── копия детектора из agent.cpp (там он static, не линкуется) ──
static bool is_dangerous_command(const std::string& raw) {
    std::string c = util::to_lower(raw);
    std::string n; bool sp = false;
    for (char ch : c) {
        if (ch == ' ' || ch == '\t') { if (!n.empty()) sp = true; }
        else { if (sp) n += ' '; sp = false; n += ch; }
    }
    auto has = [&](const char* sub) { return n.find(sub) != std::string::npos; };
    return
        has("rm -rf") || has("rm -fr") || has("rm -r -f") || has("rm -f -r") ||
        has("mkfs") || has("dd if=") || has("dd of=") ||
        has(":(){:|:&};:") || has(":(){ :|:& };:") ||
        has("> /dev/sd") || has("of=/dev/sd") || has("of=/dev/nvme") ||
        has("chmod -r 777 /") || has("chown -r") ||
        has("drop table") || has("drop database") || has("truncate table") ||
        has("push --force") || has("push -f") || has("--force-with-lease") ||
        has("rsync --delete") || has("git reset --hard") || has("git clean -f") ||
        has("shutdown") || has("reboot") || has("halt") || has("init 0") ||
        has("> /dev/null; rm") || has("format ") || has("del /f") || has("rd /s");
}

int main() {
    // ── 1. Детектор опасных команд ──
    check("danger: rm -rf",        is_dangerous_command("rm -rf /home/x/build"));
    check("danger: rm   -rf (spaces)", is_dangerous_command("rm    -rf  build"));
    check("danger: force push",    is_dangerous_command("git push --force origin main"));
    check("danger: drop table",    is_dangerous_command("sqlite3 db 'DROP TABLE users'"));
    check("danger: dd of=/dev/sda",is_dangerous_command("dd if=/dev/zero of=/dev/sda"));
    check("danger: reboot",        is_dangerous_command("sudo reboot"));
    check("safe: ls -la",         !is_dangerous_command("ls -la"));
    check("safe: git push",       !is_dangerous_command("git push origin main"));
    check("safe: rm file.txt",    !is_dangerous_command("rm file.txt"));
    check("safe: grep -rf",       !is_dangerous_command("grep -rf pattern ."));

    // ── общий sqlite для History+Memory (in-memory shared cache) ──
    sqlite3* db = nullptr;
    sqlite3_open(":memory:", &db);

    // ── 2. Memory::search (настоящий код) ──
    Memory mem(db);
    mem.init();
    mem.add("Пользователь Арсен использует Arch Linux с KDE Plasma 6");
    mem.add("Проект nexus-chat — мессенджер на C++ с E2E-шифрованием");
    mem.add("Любимый язык питон для скриптов автоматизации");
    mem.add("Деплой идёт на VPS через systemd и Caddy с Let's Encrypt");
    auto hits = mem.search("на чём написан мессенджер nexus", 3);
    bool found_chat = false;
    for (auto& h : hits) if (h.text.find("nexus-chat") != std::string::npos) found_chat = true;
    check("memory: находит релевантный факт", found_chat,
          hits.empty() ? "нет результатов" : hits[0].text);
    check("memory: не пусто при осмысленном запросе", !hits.empty());
    // честный лексический запрос: делит слова с нужным фактом (Arch/Linux/Plasma).
    // семантику без общих слов (синонимы, словоформы) bag-of-words/FTS не ловит —
    // это осознанное ограничение, для него нужны эмбеддинги
    auto hits2 = mem.search("система Arch Linux Plasma", 2);
    bool found_os = false;
    for (auto& h : hits2) if (h.text.find("Arch Linux") != std::string::npos) found_os = true;
    check("memory: ранжирует по релевантности (ОС наверх)", found_os,
          hits2.empty() ? "нет" : hits2[0].text);

    // ── 3. edit_file через настоящую ToolRegistry ──
    History hist(":memory:");
    hist.open();
    Config cfg;
    bool approve = true;
    ToolContext ctx{cfg, hist, mem, approve};
    ToolRegistry reg;
    reg.register_all();
    const Tool* edit = reg.find("edit_file");
    check("edit_file зарегистрирован", edit != nullptr);

    std::string path = "/tmp/nexus_edit_test.txt";
    auto writef = [&](const std::string& c){ std::ofstream f(path); f << c; };
    auto readf  = [&]{ std::ifstream f(path); std::string s((std::istreambuf_iterator<char>(f)), {}); return s; };

    // 3a. уникальная замена + номер строки
    writef("line one\nTARGET here\nline three\n");
    std::string r = reg.call("edit_file", {{"path",path},{"old_string","TARGET here"},{"new_string","REPLACED"}}, ctx);
    check("edit: уникальная замена выполнена", readf() == "line one\nREPLACED\nline three\n", r);
    check("edit: сообщает номер строки (2)", r.find("строка 2") != std::string::npos, r);

    // 3b. неоднозначное совпадение → отказ, файл не тронут
    writef("dup\ndup\nkeep\n");
    r = reg.call("edit_file", {{"path",path},{"old_string","dup"},{"new_string","X"}}, ctx);
    check("edit: неоднозначность → ошибка", r.find("встречается 2 раз") != std::string::npos, r);
    check("edit: при отказе файл НЕ изменён", readf() == "dup\ndup\nkeep\n");

    // 3c. replace_all снимает неоднозначность
    r = reg.call("edit_file", {{"path",path},{"old_string","dup"},{"new_string","X"},{"replace_all",true}}, ctx);
    check("edit: replace_all заменил оба", readf() == "X\nX\nkeep\n", r);
    check("edit: replace_all сообщает 2 вхождения", r.find("2 вхожден") != std::string::npos, r);

    // 3d. old_string не найден
    r = reg.call("edit_file", {{"path",path},{"old_string","NOPE"},{"new_string","Y"}}, ctx);
    check("edit: ненайденный old_string → ошибка", r.find("не найден") != std::string::npos, r);

    // 3e. old==new → отказ
    r = reg.call("edit_file", {{"path",path},{"old_string","keep"},{"new_string","keep"}}, ctx);
    check("edit: old==new → ошибка", r.find("совпадают") != std::string::npos, r);

    std::remove(path.c_str());
    sqlite3_close(db);

    std::cout << "\n" << (fails ? std::to_string(fails) + " TEST(S) FAILED" : "ALL TESTS PASSED") << "\n";
    return fails ? 1 : 0;
}
