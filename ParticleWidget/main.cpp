#include <QApplication>
#include <QVBoxLayout>
#include "ParticleWidget.h"

int main(int argc,char** argv)
{
    QApplication app(argc,argv);

    QWidget* wind = new QWidget;
    wind->setWindowTitle("OpenGL 4.6 Particle System");
    wind->resize(1280,720);

    // 关键：如果不加 layout，ParticleWidget 默认大小会非常小
    QVBoxLayout* layout = new QVBoxLayout(wind);
    layout->setContentsMargins(0,0,0,0);

    ParticleWidget* part = new ParticleWidget();
    layout->addWidget(part);

    wind->show();
    return app.exec();
}