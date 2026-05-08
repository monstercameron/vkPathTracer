#pragma once

#include <QPalette>
#include <QString>
#include <QStringList>

class QWidget;

namespace vkpt::platform {

QPalette StartupDarkPalette();
QString StartupDarkStyleSheet();
void ApplyStartupSurface(QWidget* widget);

QWidget* CreateStartupSplash();
QWidget* CreateLoadingSplash(QString title, QString subtitle, QString footer, QStringList messages);
void SetLoadingSplashPhase(QWidget* splash, QString phase);
void ShowLoadingSplashCentered(QWidget* splash, QWidget* reference);
void FinishLoadingSplash(QWidget* splash, bool animated);
void SetStartupSplashPhase(QWidget* splash, QString phase);
void ShowStartupSplashCentered(QWidget* splash, QWidget* reference);
void FinishStartupSplash(QWidget* splash, bool animated);

}  // namespace vkpt::platform
