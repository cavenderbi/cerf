#include "qt_resource_paths.h"
#include "svg_document.h"
#include "tinted_symbol_icon.h"
#include <QCoreApplication>
#include <cstdio>
#include <stdexcept>

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        const QColor color("#123456");
        Check(TintedSymbolUrl(QUrl("qrc:/icons/home.svg"), color) == TintedIconUrl("home", color),
              "CERF symbols use the tint provider");
        for (const auto& path : {"qrc:/qt-project.org/imports/QtQuick/Dialogs/quickimpl/images/home.svg",
                                 "file:///tmp/home.svg", "https://example.com/home.svg", ""}) {
            const QUrl url(QString::fromUtf8(path));
            Check(TintedSymbolUrl(url, color) == url.toString(), "non-CERF icon URLs are preserved");
        }
        const SvgDocument valid(":/icons/home.svg");
        Check(!valid.DefaultSize().isEmpty(), "valid SVG has dimensions");
        Check(!valid.Render(QSize(24, 24)).isNull(), "valid SVG renders");
        Check(valid.Render(QSize(0, 24)).isNull(), "zero-sized render is empty");
        const auto image = TintedSymbolIcon("home", QSize(24, 24), color);
        bool painted = false;
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixelColor(x, y);
            if (pixel.alpha() == 255) {
                Check(pixel == color, "symbol tint preserved");
                painted = true;
            }
        }
        Check(painted, "render contains painted pixels");
        for (const auto& path : {":/icons/missing.svg", ":/icons/invalid.svg"}) {
            const SvgDocument bad(QString::fromUtf8(path));
            Check(bad.DefaultSize().isEmpty(), "failed SVG has no dimensions");
            Check(bad.Render(QSize(24, 24)).isNull(), "failed SVG returns empty image without aborting");
        }
        Check(TintedSymbolIcon("missing", QSize(24, 24), color).isNull(), "missing tinted icon is safe");
        std::puts("PASS icon routing, rendering, tinting, missing and malformed SVGs");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
