#pragma once

#include <QPalette>
#include <QString>

class QWidget;

namespace vkpt::platform {

QPalette StartupDarkPalette();
QString StartupDarkStyleSheet();
void ApplyStartupSurface(QWidget* widget);

QWidget* CreateStartupSplash();
void SetStartupSplashPhase(QWidget* splash, QString phase);
void ShowStartupSplashCentered(QWidget* splash, QWidget* reference);
void FinishStartupSplash(QWidget* splash, bool animated);

}  // namespace vkpt::platform
