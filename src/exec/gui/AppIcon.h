/**
 * @file AppIcon.h
 * @brief Application icon drawn in code (EMCCD sensor package).
 */

#ifndef AndorEMCCDcontroller__gui_AppIcon_h
#define AndorEMCCDcontroller__gui_AppIcon_h

#include <QIcon>
#include <QImage>

namespace AndorEMCCDcontroller {

/** Renders the icon at @p size x @p size pixels. */
QImage RenderAppIcon(int size);

/** Icon with the usual sizes 16 ... 256. */
QIcon CreateAppIcon();

}  // namespace AndorEMCCDcontroller

#endif
