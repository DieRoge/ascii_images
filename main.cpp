#ifdef _WIN32
#include <windows.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION

#include "ascii_renderer.hpp"
#include <iostream>
#include <string>

void print_help(const char* prog_name) {
    std::cout << "Использование: " << prog_name << " -i <input.jpg> [опции]\n\n"
              << "Опции:\n"
              << "  -i, --input <path>       Путь к исходному изображению (обязательно)\n"
              << "  -o, --output <path>      Путь сохранения PNG (по умолчанию: wallpaper_ascii.png)\n"
              << "  -f, --font <path>        Путь к .ttf шрифту (по умолчанию: font.ttf)\n"
              << "  -p, --preset <name>      Пресет символов: minimal, standard, blocks, hybrid, braille, matrix, binary\n"
              << "  -s, --size <int>         Высота символа в пикселях (по умолчанию: 18)\n"
              << "  -g, --gamma <float>      Гамма-коррекция (по умолчанию: 1.0, для темных обоев попробуй 1.3)\n"
              << "  -c, --contrast <float>   Контрастность (по умолчанию: 1.15)\n"
              << "  -t, --threshold <float>  Порог фильтра Собеля (по умолчанию: 0.28, меньше = больше границ)\n"
              << "  --no-edges               Отключить контурные символы Собеля\n"
              << "  --mono                   Монохромный режим (зеленый терминал)\n"
              << "  --invert                 Инвертировать яркость\n"
              << "  -h, --help               Показать эту справку\n";
}

int main(int argc, char* argv[]) {
    #ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    #endif

    if (argc < 2) {
        print_help(argv[0]);
        return 1;
    }

    Config cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next_val = [&]() -> std::string {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "[Ошибка] Опция " << arg << " требует аргумент.\n";
            std::exit(1);
        };

        if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            return 0;
        } else if (arg == "-i" || arg == "--input") {
            cfg.input_path = next_val();
        } else if (arg == "-o" || arg == "--output") {
            cfg.output_path = next_val();
        } else if (arg == "-f" || arg == "--font") {
            cfg.font_path = next_val();
        } else if (arg == "-p" || arg == "--preset") {
            cfg.preset_name = next_val();
        } else if (arg == "-s" || arg == "--size") {
            cfg.cell_h = std::max(6, std::stoi(next_val()));
        } else if (arg == "-g" || arg == "--gamma") {
            cfg.gamma = std::stod(next_val());
        } else if (arg == "-c" || arg == "--contrast") {
            cfg.contrast = std::stod(next_val());
        } else if (arg == "-t" || arg == "--threshold") {
            cfg.edge_threshold = std::stod(next_val());
        } else if (arg == "--no-edges") {
            cfg.detect_edges = false;
        } else if (arg == "--mono") {
            cfg.keep_color = false;
        } else if (arg == "--invert") {
            cfg.invert = true;
        } else {
            std::cerr << "[Ошибка] Неизвестный параметр: " << arg << "\n";
            return 1;
        }
    }

    if (cfg.input_path.empty()) {
        std::cerr << "[Ошибка] Укажи входной файл через -i <path>\n";
        return 1;
    }

    AsciiRenderer renderer(cfg);
    if (!renderer.init_font()) {
        return 1;
    }
    if (!renderer.process_image()) {
        return 1;
    }

    return 0;
}