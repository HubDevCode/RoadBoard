// Colour palette & metrics (dark skin).
#pragma once

#include <QColor>
#include <QFont>

namespace road::theme {

inline const QColor bg{0x1b, 0x1f, 0x26};
inline const QColor bgDeep{0x12, 0x15, 0x1a};
inline const QColor panel{0x27, 0x2d, 0x36};
inline const QColor panelHi{0x31, 0x38, 0x43};
inline const QColor border{0x3c, 0x45, 0x52};
inline const QColor text{0xc9, 0xd2, 0xdc};
inline const QColor textDim{0x7d, 0x88, 0x96};
inline const QColor title{0xf5, 0xc5, 0x18};       // road yellow
inline const QColor accent{0x3f, 0xc1, 0xff};      // cyan
inline const QColor device{0x8f, 0xd3, 0xff};
inline const QColor btnOff{0x38, 0x40, 0x4b};
inline const QColor btnText{0xa9, 0xb3, 0xbe};
inline const QColor busA{0x2f, 0xbf, 0x71};       // hardware outputs A1..A5
inline const QColor hwInColor{0x3f, 0xc1, 0xff};   // hardware inputs 1..5
inline const QColor virtInColor{0xf5, 0xc5, 0x18}; // virtual inputs V1..V3
inline const QColor busB{0x3d, 0x8b, 0xfd};
inline const QColor mute{0xe5, 0x48, 0x4d};
inline const QColor solo{0xf5, 0xc5, 0x18};
inline const QColor mono{0x9b, 0x7b, 0xff};
inline const QColor orange{0xf0, 0x8c, 0x00};
inline const QColor meterGreen{0x34, 0xd0, 0x58};
inline const QColor meterYellow{0xff, 0xd3, 0x3d};
inline const QColor meterRed{0xff, 0x4d, 0x4d};
inline const QColor limitYellow{0xff, 0xd3, 0x3d};
inline const QColor limitGreen{0x4c, 0xe0, 0x7a};

inline QFont mkFont(int px, bool bold = false) {
    QFont f;
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

} // namespace road::theme
