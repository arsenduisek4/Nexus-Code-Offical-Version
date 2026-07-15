// Проверка MCP-клиента через НАСТОЯЩИЙ код: поднимаем mock-MCP-сервер (python) из конфига,
// регистрируем его тулы в реестр и реально дёргаем их (без модели). Оффлайн, детерминированно.
#include "mcp.hpp"
#include "tools.hpp"
#include "config.hpp"
#include "history.hpp"
#include "memory.hpp"
#include "util.hpp"
#include <sqlite3.h>
#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>

using namespace nexus;
static int fails = 0;
static void check(const std::string& name, bool ok, const std::string& extra = "") {
    std::cout << (ok ? "PASS  " : "FAIL  ") << name;
    if (!ok && !extra.empty()) std::cout << "   (" << extra << ")";
    std::cout << "\n";
    if (!ok) ++fails;
}
static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

int main(int argc, char** argv) {
    std::string sp = argc > 1 ? argv[1] : ".";   // каталог scratchpad с mock_mcp.py
    std::string cfgpath = sp + "/mcp_test_config.json";
    { std::ofstream f(cfgpath);
      f << "{\"mcpServers\":{\"mock\":{\"command\":\"python3\",\"args\":[\"" << sp << "/mock_mcp.py\"]}}}"; }

    Config cfg;
    History hist(":memory:"); hist.open();
    sqlite3* db = nullptr; sqlite3_open(":memory:", &db);
    Memory mem(db); mem.init();
    json plan = json::array();
    bool approve = true;
    ToolContext ctx{cfg, hist, mem, approve, plan};

    ToolRegistry reg; reg.register_all();
    size_t before = reg.size();

    MCPManager mcp;
    int up = mcp.load(cfgpath, reg);
    check("MCP: поднят 1 сервер", up == 1, "up=" + std::to_string(up));
    check("MCP: зарегистрированы 2 тула", mcp.tool_count() == 2, "n=" + std::to_string(mcp.tool_count()));
    check("MCP: реестр вырос на 2", reg.size() == before + 2);

    const Tool* echo = reg.find("mcp__mock__echo");
    check("MCP: тул mcp__mock__echo есть", echo != nullptr);
    check("MCP: echo помечен read-only (немутирующий)", echo && !echo->mutating);
    const Tool* add = reg.find("mcp__mock__add");
    check("MCP: add без readOnlyHint → мутирующий", add && add->mutating);

    // схема должна быть очищена: без $schema/additionalProperties
    if (echo) {
        std::string sch = echo->parameters.dump();
        check("MCP: схема очищена ($schema убран)", !has(sch, "$schema"), sch);
        check("MCP: схема очищена (additionalProperties убран)", !has(sch, "additionalProperties"), sch);
        check("MCP: схема сохранила properties.text", has(sch, "text"), sch);
    }

    // реальный вызов через сервер
    std::string r = reg.call("mcp__mock__echo", {{"text", "привет"}}, ctx);
    check("MCP: echo вернул результат от сервера", has(r, "ECHO: привет"), r);
    r = reg.call("mcp__mock__add", {{"a", 2}, {"b", 40}}, ctx);
    check("MCP: add посчитал на сервере", has(r, "SUM=42"), r);

    // повторный вызов (проверяем, что пайп жив и id-матчинг не сломался)
    r = reg.call("mcp__mock__echo", {{"text", "два"}}, ctx);
    check("MCP: повторный вызов работает", has(r, "ECHO: два"), r);

    auto sum = mcp.summary();
    check("MCP: summary непустой", !sum.empty() && has(sum[0], "mock"), sum.empty() ? "" : sum[0]);

    std::remove(cfgpath.c_str());
    std::cout << "\n" << (fails ? std::to_string(fails) + " TEST(S) FAILED" : "ALL TESTS PASSED") << "\n";
    return fails ? 1 : 0;
}
