#include "cerfman/svg_icon_provider.h"
#include <QApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QTimer>
#include <cstdio>
#include <memory>
#include <QtQml/qqmlextensionplugin.h>

Q_IMPORT_QML_PLUGIN(CerfStylePlugin)

int main(int argc, char** argv) {
    QQuickStyle::setStyle(QStringLiteral("CerfStyle"));
    QQuickStyle::setFallbackStyle(QStringLiteral("Material"));
    QApplication app(argc, argv);
    QQmlEngine engine;
    engine.addImageProvider(QStringLiteral("icon"), new SvgIconProvider);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import QtQuick.Controls
        import QtQuick.Dialogs
        ApplicationWindow {
            visible: true
            width: 640; height: 480
            FileDialog {
                id: picker
                objectName: "picker"
                options: FileDialog.DontUseNativeDialog
                currentFolder: "file:///tmp"
            }
            Component.onCompleted: picker.open()
        }
    )", QUrl());
    std::unique_ptr<QObject> window(component.create());
    if (!window) {
        std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
        return 1;
    }
    QTimer::singleShot(500, &app, &QCoreApplication::quit);
    app.exec();
    QObject* picker = window->findChild<QObject*>(QStringLiteral("picker"));
    if (!picker || !picker->property("visible").toBool()) {
        std::fprintf(stderr, "FAIL: file picker did not open\n");
        return 1;
    }
    std::puts("PASS Qt Quick file picker opens with CERF style");
}
