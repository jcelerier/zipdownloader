#include "zipdownloader.hpp"
#include <QCoreApplication>
#include <QDebug>

int main(int argc, char** argv)
{
    QCoreApplication app{argc, argv};

    zdl::download_and_extract(
                QUrl("https://github.com/richgel999/miniz/archive/master.zip"),
                "/tmp/some_folder",
                [] (const auto& files) {
                   qDebug("Download & extraction successful !");
                   for(const auto& f : files)
                       qDebug() << f;
                   qApp->exit(0);
                },
                [] {
                   qDebug("Failure...");
                   qApp->exit(1);
                }
    );

    app.exec();
}
