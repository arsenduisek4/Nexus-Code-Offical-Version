// Проверка модуля зрения через НАСТОЯЩИЙ код: детект картинок, base64, размеры, mime.
#include "vision.hpp"
#include "config.hpp"
#include "util.hpp"
#include <iostream>
#include <string>

using namespace nexus;
static int fails = 0;
static void check(const std::string& n, bool ok, const std::string& extra = "") {
    std::cout << (ok ? "PASS  " : "FAIL  ") << n;
    if (!ok && !extra.empty()) std::cout << "   (" << extra << ")";
    std::cout << "\n"; if (!ok) ++fails;
}
static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

int main(int argc, char** argv) {
    std::string pic = argc > 1 ? std::string(argv[1]) + "/pic.png" : "pic.png";

    Config gem; gem.model = "gemini-3.1-flash-lite";
    gem.base_url = "https://generativelanguage.googleapis.com/v1beta/openai";
    Config ds;  ds.model = "deepseek-chat"; ds.base_url = "https://api.deepseek.com";
    check("supports: Gemini → да", vision::supports(gem));
    check("supports: DeepSeek → нет", !vision::supports(ds));

    check("has_image_refs: путь к png", vision::has_image_refs("что на " + pic + " ?"));
    check("has_image_refs: обычный текст → нет", !vision::has_image_refs("просто почини сборку"));
    check("has_image_refs: url картинки", vision::has_image_refs("глянь https://x.com/a.jpg вот"));
    check("has_image_refs: несуществующий файл → нет", !vision::has_image_refs("нет /tmp/zzz_nope.png тут"));

    json parts = json::array();
    std::vector<std::string> notes;
    bool got = vision::collect("опиши " + pic + " кратко", parts, notes);
    check("collect: нашёл картинку", got && parts.size() == 1, "n=" + std::to_string(parts.size()));
    if (!parts.empty()) {
        std::string url = parts[0]["image_url"].value("url", "");
        check("collect: data:image/png;base64", has(url, "data:image/png;base64,"), url.substr(0, 40));
        check("collect: base64 непустой и валидной длины", url.size() > 200 && (url.size() % 4 != 1));
    }
    check("collect: заметка с размером 240×160", !notes.empty() && has(notes[0], "240×160"),
          notes.empty() ? "" : notes[0]);

    // url остаётся ссылкой, не кодируется
    json p2 = json::array(); std::vector<std::string> n2;
    vision::collect("см https://example.com/photo.jpeg", p2, n2);
    check("collect: url отдаётся как ссылка", !p2.empty() &&
          p2[0]["image_url"].value("url", "") == "https://example.com/photo.jpeg");

    std::cout << "\n" << (fails ? std::to_string(fails) + " TEST(S) FAILED" : "ALL TESTS PASSED") << "\n";
    return fails ? 1 : 0;
}
