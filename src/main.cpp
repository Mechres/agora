#include "ui/main_window.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("agora");
    QApplication::setApplicationDisplayName("Agora");
    QApplication::setOrganizationName("Mechres");

    agora::MainWindow window;
    window.show();

    return app.exec();
}
