#pragma once

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <cstdint>

#include "lib/stb_image.h"
#include "lib/stb_image_write.h"
#include "lib/stb_truetype.h"

struct RGB {
    uint8_t r, g, b;
};

struct Config {
    std::string input_path;
    std::string output_path = "wallpaper_ascii.png";
    std::string font_path = "font.ttf";
    std::string preset_name = "standard";

    int cell_h = 18;               // Высота символа в пикселях
    double gamma = 1.0;            // Гамма-коррекция (>1.0 затемняет тени, <1.0 высветляет)
    double contrast = 1.15;        // Контрастность (1.0 — без изменений)
    double color_boost = 1.25;     // Усиление яркости цвета символов

    bool detect_edges = true;      // Включить детектор границ Собеля
    double edge_threshold = 0.28;  // Порог срабатывания границы [0.05 .. 1.0]

    bool keep_color = true;        // true = цвет исходника, false = монохром
    bool invert = false;           // Инверсия яркости
    RGB bg_color = {12, 12, 16};   // Цвет фона обоев
    RGB mono_color = {0, 230, 118};// Цвет символов в монохромном режиме
};

// Градиентные алфавиты в UTF-32 (от самого темного к самому светлому)
inline const std::unordered_map<std::string, std::u32string> PRESETS = {
    {"minimal",  U" .:-=+*#%@"},
    {"standard", U" .'`^\",:;Il!i><~+_-?][}{1)(|/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$"},
    {"blocks",   U" ░▒▓█"},
    {"hybrid",   U" .:-=+*#%@░▒▓█"},
    {"braille",  U" ⠁⠃⠇⠏⠟⠿⣿"},
    {"matrix",   U" .:+-*=%012345789Z#@&"},
    {"binary",   U" 01"}
};

class AsciiRenderer {
public:
    explicit AsciiRenderer(const Config& cfg) : cfg_(cfg) {
        auto it = PRESETS.find(cfg_.preset_name);
        if (it != PRESETS.end()) {
            alphabet_ = it->second;
        } else {
            std::cerr << "[!] Пресет '" << cfg_.preset_name << "' не найден, используется 'standard'.\n";
            alphabet_ = PRESETS.at("standard");
        }
    }

    bool init_font() {
        std::ifstream file(cfg_.font_path, std::ios::binary | std::ios::ate);
        if (!file) {
            std::cerr << "[Ошибка] Не удалось открыть файл шрифта: " << cfg_.font_path << "\n";
            return false;
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        font_buffer_.resize(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(font_buffer_.data()), size)) {
            return false;
        }

        if (!stbtt_InitFont(&font_, font_buffer_.data(), stbtt_GetFontOffsetForIndex(font_buffer_.data(), 0))) {
            std::cerr << "[Ошибка] Не удалось инициализировать TTF шрифт.\n";
            return false;
        }

        float scale = stbtt_ScaleForPixelHeight(&font_, static_cast<float>(cfg_.cell_h));

        int ascent = 0, descent = 0, line_gap = 0;
        stbtt_GetFontVMetrics(&font_, &ascent, &descent, &line_gap);
        int baseline = static_cast<int>(std::round(ascent * scale));

        // Вычисляем ширину ячейки по эталонному символу моноширинного шрифта
        int advance_w = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&font_, 'M', &advance_w, &lsb);
        cell_w_ = std::max(4, static_cast<int>(std::round(advance_w * scale)));

        // Собираем все символы, которые нужно закэшировать (алфавит + символы границ)
        std::u32string all_chars = alphabet_ + U"|-/\\ ";
        int missing_glyphs = 0;

        for (char32_t cp : all_chars) {
            if (glyph_cache_.contains(cp)) continue;

            if (cp != U' ' && stbtt_FindGlyphIndex(&font_, static_cast<int>(cp)) == 0) {
                ++missing_glyphs;
            }

            glyph_cache_[cp] = rasterize_glyph(cp, scale, baseline);
        }

        if (missing_glyphs > 0) {
            std::cerr << "[Внимание] Шрифт не содержит " << missing_glyphs
                      << " глиф(ов) из выбранного пресета. Используй шрифт с полной поддержкой Unicode (например, DejaVuSansMono или JetBrainsMono).\n";
        }

        return true;
    }

    bool process_image() {
        int img_w = 0, img_h = 0, channels = 0;
        uint8_t* img = stbi_load(cfg_.input_path.c_str(), &img_w, &img_h, &channels, 0);
        if (!img) {
            std::cerr << "[Ошибка] Не удалось загрузить картинку: " << cfg_.input_path << "\n";
            return false;
        }

        std::cout << "[*] Исходник: " << img_w << "x" << img_h << " (" << channels << " канала)\n";
        std::cout << "[*] Размер ячейки символа: " << cell_w_ << "x" << cfg_.cell_h << " px\n";

        // 1. Предрасчёт карты яркости [0..1] для каждого пикселя
        std::vector<float> lum_map(static_cast<size_t>(img_w) * img_h, 0.0f);
        for (int y = 0; y < img_h; ++y) {
            for (int x = 0; x < img_w; ++x) {
                int idx = (y * img_w + x) * channels;
                float r = img[idx] / 255.0f;
                float g = (channels >= 3 ? img[idx + 1] : img[idx]) / 255.0f;
                float b = (channels >= 3 ? img[idx + 2] : img[idx]) / 255.0f;
                lum_map[y * img_w + x] = 0.299f * r + 0.587f * g + 0.114f * b;
            }
        }

        // 2. Предрасчёт градиентов Собеля (Gx, Gy)
        std::vector<float> gx_map, gy_map;
        if (cfg_.detect_edges) {
            compute_sobel(lum_map, img_w, img_h, gx_map, gy_map);
        }

        // 3. Инициализация выходного холста и заливка фоном
        std::vector<uint8_t> out_img(static_cast<size_t>(img_w) * img_h * 3);
        for (size_t i = 0; i < out_img.size(); i += 3) {
            out_img[i]     = cfg_.bg_color.r;
            out_img[i + 1] = cfg_.bg_color.g;
            out_img[i + 2] = cfg_.bg_color.b;
        }

        // 4. Проход по сетке тайлов и рендеринг символов
        for (int start_y = 0; start_y < img_h; start_y += cfg_.cell_h) {
            for (int start_x = 0; start_x < img_w; start_x += cell_w_) {
                auto [codepoint, color] = analyze_cell(img, lum_map, gx_map, gy_map, img_w, img_h, channels, start_x, start_y);
                draw_glyph(out_img, img_w, img_h, start_x, start_y, codepoint, color);
            }
        }

        stbi_image_free(img);

        // 5. Сохранение в PNG
        if (!stbi_write_png(cfg_.output_path.c_str(), img_w, img_h, 3, out_img.data(), img_w * 3)) {
            std::cerr << "[Ошибка] Не удалось сохранить результат в " << cfg_.output_path << "\n";
            return false;
        }

        std::cout << "[+] Готово! Обои сохранены в: " << cfg_.output_path << "\n";
        return true;
    }

private:
    Config cfg_;
    std::u32string alphabet_;
    int cell_w_ = 8;

    std::vector<unsigned char> font_buffer_;
    stbtt_fontinfo font_{};
    std::unordered_map<char32_t, std::vector<uint8_t>> glyph_cache_;

    // Растеризация одного символа в буфер cell_w_ x cell_h
    std::vector<uint8_t> rasterize_glyph(char32_t cp, float scale, int baseline) {
        std::vector<uint8_t> cell_bmp(static_cast<size_t>(cell_w_) * cfg_.cell_h, 0);
        if (cp == U' ') return cell_bmp;

        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(&font_, static_cast<int>(cp), scale, scale, &x0, &y0, &x1, &y1);
        int gw = x1 - x0;
        int gh = y1 - y0;

        if (gw <= 0 || gh <= 0) return cell_bmp;

        std::vector<uint8_t> raw_bmp(static_cast<size_t>(gw) * gh, 0);
        stbtt_MakeCodepointBitmap(&font_, raw_bmp.data(), gw, gh, gw, scale, scale, static_cast<int>(cp));

        // Центрируем глиф по горизонтали внутри ячейки
        int offset_x = std::max(0, (cell_w_ - gw) / 2);

        for (int gy = 0; gy < gh; ++gy) {
            int dst_y = baseline + y0 + gy;
            if (dst_y < 0 || dst_y >= cfg_.cell_h) continue;

            for (int gx = 0; gx < gw; ++gx) {
                int dst_x = offset_x + gx;
                if (dst_x < 0 || dst_x >= cell_w_) continue;
                cell_bmp[dst_y * cell_w_ + dst_x] = raw_bmp[gy * gw + gx];
            }
        }
        return cell_bmp;
    }

    // Оператор Собеля 3x3
    static void compute_sobel(const std::vector<float>& lum, int w, int h,
                              std::vector<float>& gx_out, std::vector<float>& gy_out) {
        gx_out.assign(static_cast<size_t>(w) * h, 0.0f);
        gy_out.assign(static_cast<size_t>(w) * h, 0.0f);

        for (int y = 1; y < h - 1; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                float tl = lum[(y - 1) * w + (x - 1)];
                float tc = lum[(y - 1) * w + x];
                float tr = lum[(y - 1) * w + (x + 1)];
                float ml = lum[y * w + (x - 1)];
                float mr = lum[y * w + (x + 1)];
                float bl = lum[(y + 1) * w + (x - 1)];
                float bc = lum[(y + 1) * w + x];
                float br = lum[(y + 1) * w + (x + 1)];

                gx_out[y * w + x] = (tr + 2.0f * mr + br) - (tl + 2.0f * ml + bl);
                gy_out[y * w + x] = (bl + 2.0f * bc + br) - (tl + 2.0f * tc + tr);
            }
        }
    }

    // Коррекция контраста и гаммы для значения в [0..1]
    double adjust_luminance(double val) const {
        if (cfg_.invert) {
            val = 1.0 - val;
        }
        // Контраст относительно средней точки 0.5
        val = (val - 0.5) * cfg_.contrast + 0.5;
        val = std::clamp(val, 0.0, 1.0);
        // Степенная гамма-коррекция
        val = std::pow(val, cfg_.gamma);
        return std::clamp(val, 0.0, 1.0);
    }

    // Выбор контурного символа по углу нормали градиента
    static char32_t pick_edge_char(double avg_gx, double avg_gy) {
        constexpr double PI = 3.14159265358979323846;
        // Угол нормали градиента в градусах [0, 180)
        double angle = std::atan2(avg_gy, avg_gx) * 180.0 / PI;
        if (angle < 0.0) angle += 180.0;

        // Градиент перпендикулярен самой линии границы:
        // Горизонтальный градиент (0° или 180°) -> вертикальная граница '|'
        if (angle < 22.5 || angle >= 157.5) return U'|';
        // Диагональ одного знака (22.5°..67.5°) -> граница '/'
        if (angle < 67.5) return U'/';
        // Вертикальный градиент (67.5°..112.5°) -> горизонтальная граница '-'
        if (angle < 112.5) return U'-';
        // Диагональ разных знаков (112.5°..157.5°) -> граница '\'
        return U'\\';
    }

    std::pair<char32_t, RGB> analyze_cell(const uint8_t* img,
                                          const std::vector<float>& lum_map,
                                          const std::vector<float>& gx_map,
                                          const std::vector<float>& gy_map,
                                          int img_w, int img_h, int channels,
                                          int start_x, int start_y) const {
        uint64_t sum_r = 0, sum_g = 0, sum_b = 0;
        double sum_lum = 0.0;
        double sum_gx = 0.0, sum_gy = 0.0;
        int count = 0;

        int end_y = std::min(start_y + cfg_.cell_h, img_h);
        int end_x = std::min(start_x + cell_w_, img_w);

        for (int y = start_y; y < end_y; ++y) {
            for (int x = start_x; x < end_x; ++x) {
                int p_idx = y * img_w + x;
                int c_idx = p_idx * channels;

                sum_r += img[c_idx];
                sum_g += (channels >= 3 ? img[c_idx + 1] : img[c_idx]);
                sum_b += (channels >= 3 ? img[c_idx + 2] : img[c_idx]);
                sum_lum += lum_map[p_idx];

                if (cfg_.detect_edges) {
                    sum_gx += gx_map[p_idx];
                    sum_gy += gy_map[p_idx];
                }
                ++count;
            }
        }

        if (count == 0) return {U' ', cfg_.bg_color};

        double avg_lum = adjust_luminance(sum_lum / count);

        // Расчёт выходного цвета символа
        RGB out_color = cfg_.mono_color;
        if (cfg_.keep_color) {
            auto boost = [this](double c) -> uint8_t {
                double n = (c / 255.0) * cfg_.color_boost;
                return static_cast<uint8_t>(std::clamp(n * 255.0, 0.0, 255.0));
            };
            out_color = {
                boost(static_cast<double>(sum_r) / count),
                boost(static_cast<double>(sum_g) / count),
                boost(static_cast<double>(sum_b) / count)
            };
        }

        // Проверка на сильную границу (Sobel)
        if (cfg_.detect_edges) {
            double avg_gx = sum_gx / count;
            double avg_gy = sum_gy / count;
            double mag = std::hypot(avg_gx, avg_gy);

            if (mag >= cfg_.edge_threshold) {
                return {pick_edge_char(avg_gx, avg_gy), out_color};
            }
        }

        // Обычный выбор символа по яркости
        size_t idx = static_cast<size_t>( std::round(avg_lum * (alphabet_.size() - 1)) );
        idx = std::min(idx, alphabet_.size() - 1);
        return {alphabet_[idx], out_color};
    }

    // Отрисовка глифа из кэша с альфа-блендингом
    void draw_glyph(std::vector<uint8_t>& out_img, int img_w, int img_h,
                    int start_x, int start_y, char32_t cp, RGB fg) const {
        auto it = glyph_cache_.find(cp);
        if (it == glyph_cache_.end()) return;
        const auto& mask = it->second;

        int end_y = std::min(start_y + cfg_.cell_h, img_h);
        int end_x = std::min(start_x + cell_w_, img_w);

        for (int y = start_y; y < end_y; ++y) {
            int gy = y - start_y;
            for (int x = start_x; x < end_x; ++x) {
                int gx = x - start_x;
                uint8_t alpha = mask[gy * cell_w_ + gx];
                if (alpha == 0) continue;

                float a = alpha / 255.0f;
                size_t out_idx = (static_cast<size_t>(y) * img_w + x) * 3;

                out_img[out_idx]     = static_cast<uint8_t>(fg.r * a + cfg_.bg_color.r * (1.0f - a));
                out_img[out_idx + 1] = static_cast<uint8_t>(fg.g * a + cfg_.bg_color.g * (1.0f - a));
                out_img[out_idx + 2] = static_cast<uint8_t>(fg.b * a + cfg_.bg_color.b * (1.0f - a));
            }
        }
    }
};