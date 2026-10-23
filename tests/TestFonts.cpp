#include "TestFonts.h"

#include <QFontDatabase>
#include <QString>

#include <array>
#include <mutex>

namespace
{
constexpr std::array<const char*, 4> kFontStyles = {"Regular", "Bold", "Italic", "BoldItalic"};
}

namespace TestFonts
{
void Register()
{
    static std::once_flag once;
    std::call_once(once, []() {
        for (const char* pStyle : kFontStyles)
        {
            QFontDatabase::addApplicationFont(QStringLiteral(":/terminalviewer/testfonts/CascadiaMono-%1.ttf").arg(QLatin1String(pStyle)));
        }
    });
}
}
