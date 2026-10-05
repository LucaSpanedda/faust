/************************** BEGIN QTUI.h *****************************
 FAUST Architecture File
 Copyright (C) 2003-2022 GRAME, Centre National de Creation Musicale
 ---------------------------------------------------------------------
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU Lesser General Public License as published by
 the Free Software Foundation; either version 2.1 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU Lesser General Public License for more details.
 
 You should have received a copy of the GNU Lesser General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 
 EXCEPTION : As a special exception, you may create a larger work
 that contains this FAUST architecture section and distribute
 that work under terms of your choice, so long as this FAUST
 architecture section is not modified.
 ************************************************************************/

#ifndef __QTUI__
#define __QTUI__

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <set>
#include <vector>
#include <stack>

#if defined(HTTPCTRL) && defined(QRCODECTRL)

#ifdef _WIN32
# include <winsock2.h>
# undef min
# undef max
# pragma warning (disable: 4100)
#else
# include <netdb.h>
# include <arpa/inet.h>
# include <unistd.h>
#endif

#include <QtNetwork>

#endif

#include <QtGlobal>
#include <QtGui>
#if QT_VERSION >= 0x050000
#include <QtWidgets>
#endif
#if QT_VERSION >= 0x060000
#define QTSetMargins(a) setContentsMargins(a, a, a, a)
#else
#define QTSetMargins(a) setMargin(a)
#endif
#include <QApplication>
#include <QLabel>
#include <QComboBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <sstream>

#include "faust/gui/GUI.h"
#include "faust/gui/ValueConverter.h"
#include "faust/gui/SimpleParser.h"
#include "faust/gui/MetaDataUI.h"

#if defined(HTTPCTRL) && defined(QRCODECTRL)
#include "faust/gui/qrcodegen.h"
#endif

#if QT_VERSION >= 0x040300
  #define QTColorLighter	lighter
  #define QTColorDarker		darker
#else
  #define QTColorLighter	light
  #define QTColorDarker		dark
#endif

// for compatibility
#define minValue minimum
#define maxValue maximum

//==============================THEME (Luca Spanedda)================================
//
// Colors of the widgets painted by hand (knobs, bargraphs, LEDs), which the
// stylesheet cannot reach. Keep them in sync with Styles/Grey.qss (light) and
// Styles/GreyDark.qss (dark).
//
// Theme mode: 0 = follow the macOS appearance (Qt >= 6.5), 1 = light, 2 = dark.
// Initial value: FAUST_THEME=system|light|dark in the environment, otherwise the
// last choice made with the theme button (saved per application), otherwise
// FQT_THEME_DEFAULT (set by 'faust2caqt -theme ...'), otherwise 0.
//
#ifndef FQT_THEME_DEFAULT
#define FQT_THEME_DEFAULT 0
#endif

static inline int& fqtThemeMode()
{
    static int mode = -1;
    if (mode == -1) {
        const char* t = getenv("FAUST_THEME");
        std::string env = t ? t : "";
        if (env == "system") mode = 0;
        else if (env == "light") mode = 1;
        else if (env == "dark") mode = 2;
        else mode = QSettings("Faust", QCoreApplication::applicationName()).value("theme", FQT_THEME_DEFAULT).toInt();
        if (mode < 0 || mode > 2) mode = 0;
    }
    return mode;
}

static inline bool fqtDark()
{
    if (fqtThemeMode() != 0) return fqtThemeMode() == 2;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
    return false;
#endif
}

#define FQT_COLOR(light, dark)  (fqtDark() ? QColor(dark) : QColor(light))
#define FQT_ACCENT      FQT_COLOR(0x1d1d1f, 0xf2f2f4)   // controls: knob arc, slider fill
#define FQT_SIGNAL      FQT_COLOR(0x48484e, 0xc4c4ca)   // signals: linear bargraphs, LEDs
#define FQT_TRACK       FQT_COLOR(0xe6e6ea, 0x3a3a3f)   // empty part of knobs and meters
#define FQT_BORDER      FQT_COLOR(0xcfcfd4, 0x4a4a50)   // thin outline of meters and knobs
#define FQT_KNOB_TOP    FQT_COLOR(0xffffff, 0x4a4a50)   // knob face, top of the gradient
#define FQT_KNOB_BOTTOM FQT_COLOR(0xececf0, 0x323236)   // knob face, bottom of the gradient
#define FQT_TEXT        FQT_COLOR(0x1d1d1f, 0xf2f2f4)   // pointer, scale text
#define FQT_TEXT_DIM    FQT_COLOR(0x86868b, 0x8e8e93)   // dB scale marks

// Compact sizes, so that more controls fit in a window without scrolling
#define FQT_SLIDER_LENGTH   120     // was 160
#define FQT_SLIDER_THICK    24      // was 34
#define FQT_METER_LENGTH    120     // was 128 (linear) / 256 (dB)
#define FQT_METER_THICK     12      // was 16 / 18
#define FQT_KNOB_WIDTH      60      // was 64
#define FQT_KNOB_HEIGHT     92      // was 100
#define FQT_BOX_MARGIN      4       // was 5
#define FQT_BOX_SPACING     4

//==============================BEGIN QSYNTHKNOB=====================================
//
//   qsynthknob and qsynthDialVokiStyle borrowed from qsynth-0.3.3 by Rui Nuno Capela
//   This widget is based on a design by Thorsten Wilms,
//   implemented by Chris Cannam in Rosegarden,
//   adapted for QSynth by Pedro Lopez-Cabanillas,
//   improved for Qt4 by David Garcia Garzon.
//

#define DIAL_MIN       (0.25 * M_PI)
#define DIAL_MAX       (1.75 * M_PI)
#define DIAL_RANGE     (DIAL_MAX - DIAL_MIN)
#define DIAL_WRAPPING  false

class qsynthDialVokiStyle : public QCommonStyle
{
    
public:
    qsynthDialVokiStyle() {}
    virtual ~qsynthDialVokiStyle() {}
    
    virtual void drawComplexControl(ComplexControl cc, const QStyleOptionComplex* opt, QPainter* p, const QWidget* widget = NULL) const
    {
        if (cc != QStyle::CC_Dial) {
            QCommonStyle::drawComplexControl(cc, opt, p, widget);
            return;
        }
        
        const QStyleOptionSlider* dial = qstyleoption_cast<const QStyleOptionSlider*>(opt);
        if (dial == NULL) return;

#ifndef FQT_CLASSIC_KNOB
        // Flat knob: grey track arc, colored value arc, light face and dark pointer
        {
            double v = double(dial->sliderValue - dial->minimum) / double(dial->maximum - dial->minimum);
            double a = DIAL_MIN + DIAL_RANGE * v;
            int side = qMin(dial->rect.width(), dial->rect.height());
            QPointF c = QRectF(dial->rect).center();
            double arcW = qMax(3.0, side * 0.09);
            QRectF arc(c.x() - side / 2.0 + arcW, c.y() - side / 2.0 + arcW, side - 2 * arcW, side - 2 * arcW);
            bool on = (dial->state & State_Enabled);

            p->save();
            p->setRenderHint(QPainter::Antialiasing, true);

            QPen pen(FQT_TRACK, arcW, Qt::SolidLine, Qt::RoundCap);
            p->setPen(pen);
            p->drawArc(arc, 225 * 16, -270 * 16);
            pen.setColor(on ? FQT_ACCENT : FQT_BORDER);
            p->setPen(pen);
            p->drawArc(arc, 225 * 16, int(-270 * 16 * v));

            double r = arc.width() / 2.0 - arcW * 1.2;
            // soft drop shadow
            QRadialGradient shadow(c + QPointF(0, r * 0.12), r * 1.15);
            shadow.setColorAt(0.80, QColor(0, 0, 0, fqtDark() ? 90 : 40));
            shadow.setColorAt(1.00, QColor(0, 0, 0, 0));
            p->setPen(Qt::NoPen);
            p->setBrush(shadow);
            p->drawEllipse(c + QPointF(0, r * 0.12), r * 1.15, r * 1.15);
            // face
            QLinearGradient face(c.x(), c.y() - r, c.x(), c.y() + r);
            face.setColorAt(0, FQT_KNOB_TOP);
            face.setColorAt(1, FQT_KNOB_BOTTOM);
            p->setPen(QPen(FQT_BORDER, 1));
            p->setBrush(face);
            p->drawEllipse(c, r, r);

            QPointF tip(c.x() - (r - 3) * sin(a), c.y() + (r - 3) * cos(a));
            QPointF base(c.x() - r * 0.45 * sin(a), c.y() + r * 0.45 * cos(a));
            p->setPen(QPen(on ? FQT_ACCENT : FQT_TEXT_DIM, qMax(2.0, side / 24.0), Qt::SolidLine, Qt::RoundCap));
            p->drawLine(base, tip);

            if (dial->state & State_HasFocus) {
                p->setPen(QPen(FQT_ACCENT, 1));
                p->setBrush(Qt::NoBrush);
                p->drawEllipse(c, r + 1, r + 1);
            }
            p->restore();
            return;
        }
#endif
        double angle = DIAL_MIN // offset
        + (DIAL_RANGE *
           (double(dial->sliderValue - dial->minimum) /
            (double(dial->maximum - dial->minimum))));
        int degrees = int(angle * 180.0 / M_PI);
        int side = dial->rect.width() < dial->rect.height() ? dial->rect.width() : dial->rect.height();
        int xcenter = dial->rect.width() / 2;
        int ycenter = dial->rect.height() / 2;
        int notchWidth   = 1 + side / 400;
        int pointerWidth = 2 + side / 30;
        int scaleShadowWidth = 1 + side / 100;
        int knobBorderWidth = 0;
        int ns = dial->tickInterval;
        int numTicks = 1 + (dial->maximum + ns - dial->minimum) / ns;
        int indent = int(0.15 * side) + 2;
        int knobWidth = side - 2 * indent;
        int shineFocus = knobWidth / 4;
        int shineCenter = knobWidth / 5;
        int shineExtension = shineCenter * 4;
        int shadowShift = shineCenter * 2;
        int meterWidth = side - 2 * scaleShadowWidth;
        
        QPalette pal = opt->palette;
        QColor knobColor = pal.mid().color();
        QColor borderColor = knobColor.QTColorLighter();
        QColor meterColor = (dial->state & State_Enabled) ?
        QColor("orange") : pal.mid().color();
        // pal.highlight().color() : pal.mid().color();
        QColor background = pal.window().color();
        
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        
        // The bright metering bit...
        QConicalGradient meterShadow(xcenter, ycenter, -90);
        meterShadow.setColorAt(0, meterColor.QTColorDarker());
        meterShadow.setColorAt(0.5, meterColor);
        meterShadow.setColorAt(1.0, meterColor.QTColorLighter().QTColorLighter());
        p->setBrush(meterShadow);
        p->setPen(Qt::transparent);
        p->drawPie(xcenter - meterWidth / 2, ycenter - meterWidth / 2,
                   meterWidth, meterWidth, (180 + 45) * 16, -(degrees - 45) * 16);
        
        // Knob projected shadow
        QRadialGradient projectionGradient(xcenter + shineCenter, ycenter + shineCenter,
                                           shineExtension,	xcenter + shadowShift, ycenter + shadowShift);
        projectionGradient.setColorAt(0, QColor(  0, 0, 0, 100));
        projectionGradient.setColorAt(1, QColor(200, 0, 0,  10));
        QBrush shadowBrush(projectionGradient);
        p->setBrush(shadowBrush);
        p->drawEllipse(xcenter - shadowShift, ycenter - shadowShift,
                       knobWidth, knobWidth);
        
        // Knob body and face...
        QPen pen;
        pen.setColor(knobColor);
        pen.setWidth(knobBorderWidth);
        p->setPen(pen);
        
        QRadialGradient gradient(xcenter - shineCenter, ycenter - shineCenter,
                                 shineExtension,	xcenter - shineFocus, ycenter - shineFocus);
        gradient.setColorAt(0.2, knobColor.QTColorLighter().QTColorLighter());
        gradient.setColorAt(0.5, knobColor);
        gradient.setColorAt(1.0, knobColor.QTColorDarker(150));
        QBrush knobBrush(gradient);
        p->setBrush(knobBrush);
        p->drawEllipse(xcenter - knobWidth / 2, ycenter - knobWidth / 2,
                       knobWidth, knobWidth);
        
        // Tick notches...
        p->setBrush(Qt::NoBrush);
        
        if (dial->subControls & QStyle::SC_DialTickmarks) {
            pen.setColor(pal.dark().color());
            pen.setWidth(notchWidth);
            p->setPen(pen);
            double hyp = double(side - scaleShadowWidth) / 2.0;
            double len = hyp / 4;
            for (int i = 0; i < numTicks; ++i) {
                int div = numTicks;
                if (div > 1) --div;
                bool internal = (i != 0 && i != numTicks - 1);
                double angle = DIAL_MIN
                + (DIAL_MAX - DIAL_MIN) * i / div;
                double dir = (internal ? -1 : len);
                double sinAngle = sin(angle);
                double cosAngle = cos(angle);
                double x0 = xcenter - (hyp - len) * sinAngle;
                double y0 = ycenter + (hyp - len) * cosAngle;
                double x1 = xcenter - (hyp + dir) * sinAngle;
                double y1 = ycenter + (hyp + dir) * cosAngle;
                p->drawLine(QLineF(x0, y0, x1, y1));
            }
        }
        
        // Shadowing...
        
        // Knob shadow...
        if (knobBorderWidth > 0) {
            QLinearGradient inShadow(xcenter - side / 4, ycenter - side / 4,
                                     xcenter + side / 4, ycenter + side / 4);
            inShadow.setColorAt(0.0, borderColor.QTColorLighter());
            inShadow.setColorAt(1.0, borderColor.QTColorDarker());
            p->setPen(QPen(QBrush(inShadow), knobBorderWidth * 7 / 8));
            p->drawEllipse(xcenter - side / 2 + indent,
                           ycenter - side / 2 + indent,
                           side - 2 * indent, side - 2 * indent);
        }
        
        // Scale shadow...
        QLinearGradient outShadow(xcenter - side / 3, ycenter - side / 3,
                                  xcenter + side / 3, ycenter + side / 3);
        outShadow.setColorAt(0.0, background.QTColorDarker().QTColorDarker());
        outShadow.setColorAt(1.0, background.QTColorLighter().QTColorLighter());
        p->setPen(QPen(QBrush(outShadow), scaleShadowWidth));
        p->drawArc(xcenter - side / 2 + scaleShadowWidth / 2,
                   ycenter - side / 2 + scaleShadowWidth / 2,
                   side - scaleShadowWidth, side - scaleShadowWidth, -45 * 16, 270 * 16);
        
        // Pointer notch...
        double hyp = double(side) / 2.0;
        double len = hyp - indent - 1;
        
        double x = xcenter - len * sin(angle);
        double y = ycenter + len * cos(angle);
        
        QColor pointerColor = pal.dark().color();
        pen.setColor((dial->state & State_Enabled) ? pointerColor.QTColorDarker(140) : pointerColor);
        pen.setWidth(pointerWidth + 2);
        p->setPen(pen);
        p->drawLine(QLineF(xcenter, ycenter, x, y));
        pen.setColor((dial->state & State_Enabled) ? pointerColor.QTColorLighter() : pointerColor.QTColorLighter(140));
        pen.setWidth(pointerWidth);
        p->setPen(pen);
        p->drawLine(QLineF(xcenter - 1, ycenter - 1, x - 1, y - 1));
        
        // done
        p->restore();
    }
    
};

//===============================END QSYNTHKNOB======================================

//==============================BEGIN DISPLAYS===================================
//
// This section constains displays, passive QT widgets that displays values in
// different ways, in particular bargraphs
//

/**
 * An abstract widget that display a value in a range
 */
class AbstractDisplay : public QWidget
{
    
protected:
    
    FAUSTFLOAT fMin;
    FAUSTFLOAT fMax;
    FAUSTFLOAT fValue;
    
public:
    
    AbstractDisplay(FAUSTFLOAT lo, FAUSTFLOAT hi) : fMin(lo), fMax(hi), fValue(lo)
    {}
    
    /**
     * set the range of displayed values
     */
    virtual void setRange(FAUSTFLOAT lo, FAUSTFLOAT hi)
    {
        fMin = lo;
        fMax = hi;
    }
    
    /**
     * set the value to be displayed
     */
    virtual void setValue(FAUSTFLOAT v)
    {
        if (v < fMin)       v = fMin;
        else if (v > fMax)  v = fMax;
        
        if (v != fValue) {
            fValue = v;
            update();
        }
    }
};

/**
 * Displays dB values using a scale of colors
 */
class dbAbstractDisplay : public AbstractDisplay
{
    
protected:
    
    FAUSTFLOAT fScaleMin;
    FAUSTFLOAT fScaleMax;
    std::vector<int>     fLevel;
    std::vector<QBrush>  fBrush;
    
    /**
     * Create the scale of colors used to paint the bargraph in relation to the levels
     * The parameter x indicates the direction of the gradient. x=1 means an horizontal
     * gradient typically used by a vertical bargraph, and x=0 a vertical gradient.
     */
    void initLevelsColors(int x)
    {
        int alpha = 200;
        { // level until -10 dB
            QColor c(40, 160, 40, alpha);
            QLinearGradient g(0,0,x,1-x);
            g.setCoordinateMode(QGradient::ObjectBoundingMode);
            g.setColorAt(0.0, c.lighter());
            g.setColorAt(0.2, c);
            g.setColorAt(0.8, c);
            g.setColorAt(0.9, c.darker(120));
            
            fLevel.push_back(-10);
            fBrush.push_back(QBrush(g));
        }
        
        { // level until -6 dB
            QColor c(160, 220, 20, alpha);
            QLinearGradient g(0,0,x,1-x);
            g.setCoordinateMode(QGradient::ObjectBoundingMode);
            g.setColorAt(0.0, c.lighter());
            g.setColorAt(0.2, c);
            g.setColorAt(0.8, c);
            g.setColorAt(0.9, c.darker(120));
            
            fLevel.push_back(-6);
            fBrush.push_back(QBrush(g));
        }
        
        { // level until -3 dB
            QColor c(220, 220, 20, alpha);
            QLinearGradient g(0,0,x,1-x);
            g.setCoordinateMode(QGradient::ObjectBoundingMode);
            g.setColorAt(0.0, c.lighter());
            g.setColorAt(0.2, c);
            g.setColorAt(0.8, c);
            g.setColorAt(0.9, c.darker(120));
            
            fLevel.push_back(-3);
            fBrush.push_back(QBrush(g));
        }
        
        { // level until -0 dB
            QColor c(240, 160, 20, alpha);
            QLinearGradient g(0,0,x,1-x);
            g.setCoordinateMode(QGradient::ObjectBoundingMode);
            g.setColorAt(0.0, c.lighter());
            g.setColorAt(0.2, c);
            g.setColorAt(0.8, c);
            g.setColorAt(0.9, c.darker(120));
            
            fLevel.push_back(0);
            fBrush.push_back(QBrush(g));
        }
        
        { // until 10 dB (and over because last one)
            QColor c(240,  0, 20, alpha);   // ColorOver
            QLinearGradient g(0,0,x,1-x);
            g.setCoordinateMode(QGradient::ObjectBoundingMode);
            g.setColorAt(0.0, c.lighter());
            g.setColorAt(0.2, c);
            g.setColorAt(0.8, c);
            g.setColorAt(0.9, c.darker(120));
            
            fLevel.push_back(+10);
            fBrush.push_back(QBrush(g));
        }
    }
    
public:
    
    dbAbstractDisplay(FAUSTFLOAT lo, FAUSTFLOAT hi) : AbstractDisplay(lo, hi), fScaleMin(0), fScaleMax(0)
    {}
    
    /**
     * set the range of displayed values
     */
    virtual void setRange(FAUSTFLOAT lo, FAUSTFLOAT hi)
    {
        AbstractDisplay::setRange(lo, hi);
        fScaleMin = dB2Scale(fMin);
        fScaleMax = dB2Scale(fMax);
    }
};

/**
 * Small rectangular LED display which color changes with the level in dB
 */
class dbLED : public dbAbstractDisplay
{
    
protected:
    
    /**
     * Draw the LED using a color depending of its value in dB
     */
    virtual void paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.fillRect(rect(), FQT_TRACK);
        
        if (fValue <= fLevel[0]) {
            // interpolate the first color on the alpha channel
            QColor c(40, 160, 40) ;
            FAUSTFLOAT a = (fValue-fMin)/(fLevel[0]-fMin);
            c.setAlphaF(a);
            painter.fillRect(rect(), c);
            
        } else {
            // find the minimal level > value
            size_t l = fLevel.size()-1; while (fValue < fLevel[l] && l > 0) l--;
            painter.fillRect(rect(), fBrush[l]);
        }
        painter.setPen(FQT_BORDER);
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
    
public:
    
    dbLED(FAUSTFLOAT lo, FAUSTFLOAT hi) : dbAbstractDisplay(lo,hi)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        initLevelsColors(1);
    }
    
    virtual QSize sizeHint() const
    {
        return QSize(18, 10);
    }
};

/**
 * Small rectangular LED display which intensity (alpha channel) changes according to the value
 */
class LED : public AbstractDisplay
{
    
protected:
    
    QColor fColor;
    
    /**
     * Draw the LED using a transparency depending of its value
     */
    virtual void paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.fillRect(rect(), FQT_TRACK);
        // interpolate the first color on the alpha channel
        QColor c = FQT_SIGNAL;
        FAUSTFLOAT a = (fValue-fMin)/(fMax-fMin);
        c.setAlphaF(a);
        painter.fillRect(rect(), c);
        painter.setPen(FQT_BORDER);
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
    
public:
    
    LED(FAUSTFLOAT lo, FAUSTFLOAT hi) : AbstractDisplay(lo, hi)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    
    virtual QSize sizeHint() const
    {
        return QSize(18, 10);
    }
};

/**
 * A simple bargraph that detect automatically its direction
 */
class linBargraph : public AbstractDisplay
{
    
protected:
    
    
    /**
     * No scale implemented yet
     */
    void paintScale(QPainter* painter) const
    {
        painter->setPen(FQT_BORDER);
        painter->drawRect(0, 0, width() - 1, height() - 1);
    }
    
    /**
     * The length of the rectangle is proportional to the value
     */
    void paintContent(QPainter* painter) const
    {
        int w = width();
        int h = height();
        FAUSTFLOAT v = (fValue-fMin)/(fMax-fMin);
        painter->fillRect(0, 0, w, h, FQT_TRACK);
        
        if (h > w) {
            // draw vertical rectangle
            painter->fillRect(0, (1-v)*h, w, v*h, FQT_SIGNAL);
        } else {
            // draw horizontal rectangle
            painter->fillRect(0, 0, v*w, h, FQT_SIGNAL);
        }
    }
    
    virtual void paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        paintContent(&painter);
        paintScale(&painter);
    }
    
public:
    
    linBargraph(FAUSTFLOAT lo, FAUSTFLOAT hi) : AbstractDisplay(lo, hi)
    {
        // compute the brush that will be used to
        // paint the value
    }
};

/**
 * A simple vertical bargraph
 */
class linVerticalBargraph : public linBargraph
{
    
public:
    
    linVerticalBargraph(FAUSTFLOAT lo, FAUSTFLOAT hi) : linBargraph(lo, hi)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    }
    
    virtual QSize sizeHint() const
    {
        return QSize(FQT_METER_THICK, FQT_METER_LENGTH);
    }
};

/**
 * A simple horizontal bargraph
 */
class linHorizontalBargraph : public linBargraph
{
    
public:
    
    linHorizontalBargraph(FAUSTFLOAT lo, FAUSTFLOAT hi) : linBargraph(lo, hi)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }
    
    virtual QSize sizeHint() const
    {
        return QSize(FQT_METER_LENGTH, FQT_METER_THICK);
    }
};

/**
 * A dB Bargraph with a scale of colors
 */
class dbBargraph : public dbAbstractDisplay
{
    
protected:
    
    QBrush fBackColor;
    
    // These two abstract methods are implemented
    // according to the vertical or horizontal direction
    // in dbVerticalBargraph and dbHorizontalBargraph
    virtual void paintMark(QPainter* painter, FAUSTFLOAT v) const = 0;
    virtual int paintSegment(QPainter* painter, int pos, FAUSTFLOAT v, const QBrush& b) const = 0;
    
    /**
     * Draw the logarithmic scale
     */
    void paintScale(QPainter* painter) const
    {
        painter->fillRect(0,0,width(),height(), FQT_TRACK);
        painter->save();
        painter->setPen(FQT_TEXT_DIM);
        for (FAUSTFLOAT v = -10; v > fMin; v -= 10) paintMark(painter, v);
        for (FAUSTFLOAT v = -6; v < fMax; v += 3) paintMark(painter, v);
        painter->restore();
    }
    
    /**
     * Draw the content using colored segments
     */
    void paintContent(QPainter* painter) const
    {
        size_t l = fLevel.size();
        
        FAUSTFLOAT p = -1;   // fake value indicates to start from border
        size_t n = 0;
        // paint all the full segments < fValue
        for (n = 0; (n < l) && (fValue > fLevel[n]); n++) {
            p = paintSegment(painter, p, fLevel[n], fBrush[n]);
        }
        // paint the last segment
        if (n == l) n = n-1;
        p=paintSegment(painter, p, fValue, fBrush[n]);
        
        painter->setPen(FQT_BORDER);
        painter->drawRect(0,0,width()-1,height()-1);
    }
    
    virtual void paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        paintScale(&painter);
        paintContent(&painter);
    }
    
public:
    
    dbBargraph(FAUSTFLOAT lo, FAUSTFLOAT hi) : dbAbstractDisplay(lo,hi)
    {
        QFont f = this->font();
        f.setPointSize(8);
        this->setFont(f);
        fBackColor = QBrush(FQT_TRACK);
    }
};

/**
 * Vertical dB Bargraph
 */
class dbVerticalBargraph : public dbBargraph
{
    
protected:
    /**
     * Convert a dB value into a vertical position
     */
    FAUSTFLOAT dB2y(FAUSTFLOAT dB) const
    {
        FAUSTFLOAT s0 = fScaleMin;
        FAUSTFLOAT s1 = fScaleMax;
        FAUSTFLOAT sx = dB2Scale(dB);
        int h = height();
        return h - h*(s0-sx)/(s0-s1);
    }
    
    /**
     * Paint a vertical graduation mark
     */
    virtual void paintMark(QPainter* painter, FAUSTFLOAT v) const
    {
        int n = 10;
        int y = dB2y(v);
        QRect r(0,y-n,width()-1,2*n);
        if (v > 0.0) {
            painter->drawText(r, Qt::AlignRight|Qt::AlignVCenter, QString::number(v).prepend('+'));
        } else {
            painter->drawText(r, Qt::AlignRight|Qt::AlignVCenter, QString::number(v));
        }
    }
    
    /**
     * Paint a color segment
     */
    virtual int paintSegment(QPainter* painter, int pos, FAUSTFLOAT v, const QBrush& b) const
    {
        if (pos == -1) pos = height();
        FAUSTFLOAT y = dB2y(v);
        painter->fillRect(0, y, width(), pos-y+1, b);
        return y;
    }
    
public:
    
    dbVerticalBargraph(FAUSTFLOAT lo, FAUSTFLOAT hi) : dbBargraph(lo,hi)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        initLevelsColors(1);
    }
    
    virtual QSize sizeHint() const
    {
        return QSize(FQT_METER_THICK + 14, FQT_METER_LENGTH + 40);
    }
};

/**
 * Horizontal dB Bargraph
 */
class dbHorizontalBargraph : public dbBargraph
{
    
protected:
    
    /**
     * Convert a dB value into an horizontal position
     */
    FAUSTFLOAT dB2x(FAUSTFLOAT dB) const
    {
        FAUSTFLOAT s0 = fScaleMin;
        FAUSTFLOAT s1 = fScaleMax;
        FAUSTFLOAT sx = dB2Scale(dB);
        int    w = width();
        return w - w*(s1-sx)/(s1-s0);
    }
    
    /**
     * Paint an horizontal graduation mark
     */
    void paintMark(QPainter* painter, FAUSTFLOAT v) const
    {
        int n = 10;
        int x = dB2x(v);
        QRect r(x-n, 0, 2*n, height());
        painter->drawText(r, Qt::AlignHCenter|Qt::AlignVCenter, QString::number(v));
    }
    
    /**
     * Paint a horizontal color segment
     */
    int paintSegment(QPainter* painter, int pos, FAUSTFLOAT v, const QBrush& b) const
    {
        if (pos == -1) pos = 0;
        FAUSTFLOAT x = dB2x(v);
        painter->fillRect(pos, 0, x-pos, height(), b);
        return x;
    }
    
public:
    
    dbHorizontalBargraph(FAUSTFLOAT lo, FAUSTFLOAT hi) : dbBargraph(lo, hi)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        initLevelsColors(0);
    }
    
    virtual QSize sizeHint() const
    {
        return QSize(FQT_METER_LENGTH + 40, FQT_METER_THICK + 6);
    }
    
};

//===============================END DISPLAYS====================================

/******************************************************************************
 *******************************************************************************
 
 IMPLEMENTATION OF GUI ITEMS
 (QT 4.3 for FAUST)
 
 *******************************************************************************
 *******************************************************************************/


/**
 * A push button that controls/reflects the value (O/1)
 * of a zone.
 */
class uiButton : public QObject, public uiItem
{
    Q_OBJECT
    
public:
    
    QAbstractButton* fButton;
    
    uiButton(GUI* ui, FAUSTFLOAT* zone, QAbstractButton* b) : uiItem(ui, zone), fButton(b) {}
    
    virtual void reflectZone()
    {
        FAUSTFLOAT v = *fZone;
        fCache = v;
        fButton->setDown(v > 0.0);
    }
    
    public slots :
    void pressed()		{ modifyZone(1.0); }
    void released()		{ modifyZone(0.0); }
};


/**
 * A checkbox that controls/reflects the value (O/1)
 * of a zone.
 */
class uiCheckButton : public QObject, public uiItem
{
    Q_OBJECT
    
public:
    
    QCheckBox* fCheckBox;
    
    uiCheckButton(GUI* ui, FAUSTFLOAT* zone, QCheckBox* b) : uiItem(ui, zone), fCheckBox(b) {}
    
    virtual void reflectZone()
    {
        FAUSTFLOAT v = *fZone;
        fCache = v;
        fCheckBox->setCheckState((v < 0.5) ? Qt::Unchecked : Qt::Checked);
    }
    
    public slots :
    void setState(int v)		{ modifyZone(FAUSTFLOAT(v > 0)); }
};

/**
 * A slider that controls/reflects the value (min..max)
 * of a zone.
 */
class uiSlider : public QObject, public uiItem, public uiConverter
{
    Q_OBJECT
    
protected:
    
    QAbstractSlider* 	fSlider;
    FAUSTFLOAT			fCur;
    FAUSTFLOAT			fMin;
    FAUSTFLOAT			fMax;
    FAUSTFLOAT			fStep;
    
public:
    
    uiSlider(GUI* ui, FAUSTFLOAT* zone, QAbstractSlider* slider, FAUSTFLOAT cur, FAUSTFLOAT lo, FAUSTFLOAT hi, FAUSTFLOAT step, MetaDataUI::Scale scale)
    : uiItem(ui, zone), uiConverter(scale, 0, 10000, lo, hi), fSlider(slider), fCur(cur), fMin(lo), fMax(hi), fStep(step)
    {
        fSlider->setMinimum(0);
        fSlider->setMaximum(10000);
        //fSlider->setSingleStep(fStep);
        fSlider->setValue(int(0.5+fConverter->faust2ui(fCur)));
        *fZone = fCur;
    }
    
    virtual ~uiSlider()
    {}
    
    virtual void reflectZone()
    {
        FAUSTFLOAT v = *fZone;
        fCache = v;
        fSlider->setValue(int(0.5+fConverter->faust2ui(v)));
    }
    
    public slots :
    void setValue(int v)
    {
        modifyZone(fConverter->ui2faust(v));
    }
};


/**
 * A zone setter, an object that sets a zone with a predefined value
 * every time the set(bool) method is called. The boolean parameter
 * is here for compatibility with some signals and is ignored.
 */
class ZoneSetter : public QObject
{
    Q_OBJECT
    
protected:
    
    FAUSTFLOAT  fValue;
    FAUSTFLOAT* fZone;
    
public:
    explicit ZoneSetter(FAUSTFLOAT v, FAUSTFLOAT* z, QObject* parent = NULL):
    QObject(parent), fValue(v), fZone(z)
    {}
    
    public slots:
    void set(bool)
    {
        *fZone = fValue;
    }
};


/**
 * A set of mutually exclusive radio buttons vertically
 * layed out. The names and values used for the radio buttons
 * are described in the string mdescr with the following syntax
 * "{'foo':3.14; 'faa':-0.34; ... 'fii':10.5}"
 */
class uiRadioButtons : public QGroupBox, public uiItem
{
    Q_OBJECT
    
protected:
    
    std::vector<double>          fValues;
    std::vector<QRadioButton*>   fButtons;
    
public:
    
    uiRadioButtons(GUI* ui, FAUSTFLOAT* z, const char* label,
                   FAUSTFLOAT cur, FAUSTFLOAT lo, FAUSTFLOAT hi, FAUSTFLOAT /*step*/,
                   bool vertical, const char* mdescr, QWidget* parent)
    : QGroupBox(label, parent),  uiItem(ui, z)
    {
        std::vector<std::string>  names;
        std::vector<double>  values;
        
        if (parseMenuList(mdescr, names, values)) {
            
            QBoxLayout* l;
            if (vertical) {
                l = new QVBoxLayout(this);
            } else {
                l = new QHBoxLayout(this);
            }
            l->setSpacing(5);
            
            QRadioButton*   defaultbutton = NULL;
            double          mindelta = FLT_MAX;
            
            for (unsigned int i = 0; i < names.size(); i++) {
                double v = values[i];
                if ((v >= lo) && (v <= hi)) {
                    
                    // It is a valid value included in slider's range
                    QRadioButton*   b = new QRadioButton(QString(names[i].c_str()), this);
                    ZoneSetter*     s = new ZoneSetter(v,z,b);
                    fValues.push_back(v);
                    fButtons.push_back(b);
                    connect(b,SIGNAL(clicked(bool)), s, SLOT(set(bool)));
                    l->addWidget(b);
                    
                    // Check if this item is a good candidate to represent the current value
                    double delta = fabs(cur-v);
                    if (delta < mindelta) {
                        mindelta = delta;
                        defaultbutton = b;
                    }
                }
            }
            // check the best candidate to represent the current value
            if (defaultbutton) { defaultbutton->setChecked(true); }
            setLayout(l);
        } else {
            std::cerr << "parseMenuList : (" << mdescr << ") is not a menu !\n";
        }
        *fZone = cur;
    }
    
    virtual void reflectZone()
    {
        FAUSTFLOAT v = *fZone;
        fCache = v;
        
        // select closest value
        int defaultitem = -1;
        double mindelta = FLT_MAX;
        
        for (unsigned int i = 0; i < fValues.size(); i++) {
            double delta = fabs(fValues[i]-v);
            if (delta < mindelta) {
                mindelta = delta;
                defaultitem = i;
            }
        }
        if (defaultitem > -1) { fButtons[defaultitem]->setChecked(true); }
    }
};

/**
 * A popup menu. The names and values used for the menu items
 * are described in the string mdescr with the following syntax
 * "{'foo':3.14; 'faa':-0.34; ... 'fii':10.5}"
 */
class uiMenu : public QComboBox, public uiItem
{
    Q_OBJECT
    
protected:
    
    std::vector<double>  fValues;
    
public:
    
    uiMenu(GUI* ui, FAUSTFLOAT* z, const char* /*label*/,
           FAUSTFLOAT cur, FAUSTFLOAT lo, FAUSTFLOAT hi, FAUSTFLOAT /*step*/,
           const char* mdescr, QWidget* parent)
    : QComboBox(parent),  uiItem(ui, z)
    {
        std::vector<std::string>  names;
        std::vector<double>  values;
        
        if (parseMenuList(mdescr, names, values)) {
            
            int defaultitem = -1;
            double mindelta = FLT_MAX;
            
            for (unsigned int i = 0; i < names.size(); i++) {
                double v = values[i];
                if ((v >= lo) && (v <= hi)) {
                    
                    // It is a valid value : add corresponding menu item
                    addItem(QString(names[i].c_str()), v);
                    fValues.push_back(v);
                    
                    // Check if this item is a good candidate to represent the current value
                    double delta = fabs(cur-v);
                    if (delta < mindelta) {
                        mindelta = delta;
                        defaultitem = count()-1;
                    }
                }
            }
            // check the best candidate to represent the current value
            if (defaultitem > -1) { setCurrentIndex(defaultitem); }
        } else {
            std::cerr << "parseMenuList : (" << mdescr << ") is not a menu !\n";
        }
        connect(this,SIGNAL(activated(int)), this, SLOT(updateZone(int)));
        *fZone = cur;
    }
    
    virtual void reflectZone()
    {
        FAUSTFLOAT v = *fZone;
        fCache = v;
        
        // search closest value
        int defaultitem = -1;
        double mindelta = FLT_MAX;
        
        for (unsigned int i=0; i<fValues.size(); i++) {
            double delta = fabs(fValues[i]-v);
            if (delta < mindelta) {
                mindelta = delta;
                defaultitem = i;
            }
        }
        if (defaultitem > -1) { setCurrentIndex(defaultitem); }
    }
    
    public slots :
    
    void updateZone(int)
    {
        double x = itemData(currentIndex()).toDouble();
        *fZone = x;
    }
};

/**
 * A bargraph representing the value of a zone
 */
class uiBargraph : public QObject, public uiItem
{
    Q_OBJECT
    
protected:
    
    AbstractDisplay* fBar;
    
public:
    
    uiBargraph(GUI* ui, FAUSTFLOAT* zone, AbstractDisplay* bar, FAUSTFLOAT lo, FAUSTFLOAT hi)
    : uiItem(ui, zone), fBar(bar)
    {
        fBar->setRange(lo, hi);
        fBar->setValue(lo);
        *fZone = lo;
    }
    
    virtual void reflectZone()
    {
        FAUSTFLOAT v = *fZone;
        fCache = v;
        fBar->setValue(v);
    }
};

/**
 * A numerical entry that controls/reflects the value (min..max)
 * of a zone.
 */
class uiNumEntry : public QObject, public uiItem
{
    Q_OBJECT
    
protected:
    
    QDoubleSpinBox* fNumEntry;
    FAUSTFLOAT fCur;
    FAUSTFLOAT fMin;
    FAUSTFLOAT fMax;
    FAUSTFLOAT fStep;
    
public:
    
    uiNumEntry(GUI* ui, FAUSTFLOAT* zone, QDoubleSpinBox* numEntry, FAUSTFLOAT cur, FAUSTFLOAT lo, FAUSTFLOAT hi, FAUSTFLOAT step)
    : uiItem(ui, zone), fNumEntry(numEntry), fCur(cur), fMin(lo), fMax(hi), fStep(step)
    {
        int decimals = (fStep >= 1.0) ? 0 : int(0.5+log10(1.0/fStep));
        fNumEntry->setMinimum(fMin);
        fNumEntry->setMaximum(fMax);
        fNumEntry->setSingleStep(fStep);
        fNumEntry->setDecimals(decimals);
        fNumEntry->setValue(fCur);
        // See https://doc.qt.io/qt-6/qabstractspinbox.html#keyboardTracking-prop
        fNumEntry->setKeyboardTracking(false);
        *fZone = fCur;
    }
    
    virtual void reflectZone()
    {
        FAUSTFLOAT v = *fZone;
        fCache = v;
        fNumEntry->setValue(v);
    }
    
    public slots :
    void setValue(double v)
    {
        modifyZone(FAUSTFLOAT(v));
    }
};

/******************************************************************************
 *******************************************************************************
 
 IMPLEMENTATION OF THE USER INTERFACE
 (QT 4.3 for FAUST)
 
 *******************************************************************************
 *******************************************************************************/
#if defined(HTTPCTRL) && defined(QRCODECTRL)
// a simple utility to retrieve an abstract qr code
// introduced to remove the dependency to qrencode
static QImage getQRCode(const QString& url, int padding)
{
    qrcodegen_Ecc errCorLvl = qrcodegen_Ecc_HIGH; //qrcodegen_Ecc_MEDIUM qrcodegen_Ecc_LOW Error correction level
    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tempBuffer[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(url.toStdString().c_str(), tempBuffer, qrcode, errCorLvl, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
        return QImage(1, 1, QImage::Format_RGB32);
    
    int size = qrcodegen_getSize(qrcode);
    QRgb colors[2];
    colors[0] = qRgb(255, 255, 255); 	// 0 is white
    colors[1] = qRgb(0, 0, 0); 			// 1 is black
    // build the QRCode image
    QImage image(size+2*padding, size+2*padding, QImage::Format_RGB32);
    // clear the image
    for (int y=0; y<size + 2*padding; y++) {
        for (int x=0; x<size + 2*padding; x++) {
            image.setPixel(x, y, colors[0]);
        }
    }
    // copy the qrcode inside
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            image.setPixel(x+padding, y+padding, colors[qrcodegen_getModule(qrcode, x, y) & 1]);
        }
    }
    return image;
}
#endif

class QTGUI : public QWidget, public GUI, public MetaDataUI
{
    Q_OBJECT
    
protected:
    
    QTimer*                 fTimer;
    std::stack<QWidget* > 	fGroupStack;
    
    QMainWindow*            fMainWindow;
    QVBoxLayout*            fGeneralLayout;
    
    QPixmap                 fQrCode;
    QToolButton*            fThemeButton = NULL;
    
    bool isTabContext()
    {
        //return fGroupStack.empty() || ((!fGroupStack.empty()) && (dynamic_cast<QTabWidget*>(fGroupStack.top()) != 0));
        return ((!fGroupStack.empty()) && (dynamic_cast<QTabWidget*>(fGroupStack.top()) != NULL));
    }
    
    /**
     * Insert a widget into the parent widget (the top of
     * the stack group). The label is used if this group is
     * a tab.
     */
    
    void insert(const char* label, QWidget* widget)
    {
        if (!fGroupStack.empty()) {
            QWidget* mother = fGroupStack.top();
            QTabWidget*	tab = dynamic_cast<QTabWidget*>(mother);
            if (tab) {
                tab->addTab(widget, label);
            } else {
                widget->setParent(mother);
                mother->layout()->addWidget(widget);
            }
        }
    }
    
    /**
     * Analyses a full label and activates the relevant options. Returns a simplified
     * label (without options) and an amount of stack adjustement (in case additional
     * containers were pushed on the stack).
     */
    
    int checkLabelOptions(QWidget* widget, const std::string& fullLabel, std::string& simplifiedLabel)
    {
        std::map<std::string, std::string> metadata;
        extractMetadata(fullLabel, simplifiedLabel, metadata);
        
        if (metadata.count("tooltip")) {
            widget->setToolTip(metadata["tooltip"].c_str());
        }
        if (metadata["option"] == "detachable") {
            //openHandleBox(simplifiedLabel.c_str());
            return 1;
        }
        
        // no adjustement of the stack needed
        return 0;
    }
    
    /**
     * Check if a tooltip is associated to a zone and add it to the corresponding widget
     */
    void checkForTooltip(FAUSTFLOAT* zone, QWidget* widget)
    {
        if (fTooltip.count(zone)) {
            widget->setToolTip(fTooltip[zone].c_str());
        }
    }
    
    void openBox(const char* fulllabel, QLayout* layout)
    {
        std::map<std::string, std::string> metadata;
        std::string label;
        extractMetadata(fulllabel, label, metadata);
        layout->QTSetMargins(FQT_BOX_MARGIN);
        layout->setSpacing(FQT_BOX_SPACING);
        QWidget* box;
        
        label = startWith(label, "0x") ? "" : label;
        if (label.find_first_not_of(" _") == std::string::npos) label = "";
        
        if (fGroupStack.empty()) {
            if (isTabContext()) {
                box = new QWidget(this);
                box->setObjectName("tabPage");
                
            } else if (label.size() > 0) {
                QGroupBox* group = new QGroupBox(this);
                group->setTitle(label.c_str());
                group->setObjectName("topGroup");
                box = group;
                
            } else {
                // no label here we use simple widget
                layout->QTSetMargins(0);
                box = new QWidget(this);
            }
            
            box->setLayout(layout);
            fGeneralLayout->addWidget(box);
            if (fGroupTooltip != "") {
                box->setToolTip(fGroupTooltip.c_str());
                fGroupTooltip = "";
            }
        } else {
            if (isTabContext()) {
                box = new QWidget();
                box->setObjectName("tabPage");
                
            } else if (label.size()>0) {
                QGroupBox* group = new QGroupBox();
                group->setTitle(label.c_str());
                box = group;
                
            } else {
                // no label here we use simple widget
                layout->QTSetMargins(0);
                box = new QWidget;
            }
            
            box->setLayout(layout);
            if (fGroupTooltip != "") {
                box->setToolTip(fGroupTooltip.c_str());
                fGroupTooltip = "";
            }
        }
        insert(label.c_str(), box);
        fGroupStack.push(box);
    }
    
    /**
     * The box opened around a single control (slider, knob, bargraph...) is only
     * a caption: give it a name for the stylesheet and stop it from growing in
     * the direction where its content has nothing to show.
     */
    /**
     * Step used to choose the decimals of the value shown with a bargraph:
     * 5 significant digits of the range, 6 decimals for huge ranges such as
     * ma.MIN..ma.MAX (used to inspect signals with [style:numerical]).
     */
    static FAUSTFLOAT displayStep(FAUSTFLOAT min, FAUSTFLOAT max)
    {
        double range = double(max) - double(min);
        if (!(range > 0) || range > 1e9) return FAUSTFLOAT(1e-6);
        return FAUSTFLOAT(std::max(range / 1e5, 1e-6));
    }
    
    void markControlBox(bool horizontal)
    {
        QWidget* box = fGroupStack.top();
        box->setObjectName("control");
        if (horizontal) {
            box->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        } else {
            box->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
        }
    }
    
    void openTab(const char* label)
    {
        QTabWidget* group;
        
        if (fGroupStack.empty()) {
            group = new QTabWidget(this);
            fGeneralLayout->addWidget(group);
        } else {
            group = new QTabWidget();
        }
        
        insert(label, group);
        fGroupStack.push(group);
    }
    
    public slots:
    
    void update()
    {
        updateAllGuis();
    }
    
public:
    
    QTGUI(QWidget* parent) : QWidget(parent)
    {
        fGeneralLayout = new QVBoxLayout;
        setLayout(fGeneralLayout);
        QWidget::show();
        
        fMainWindow = NULL;
        fTimer = NULL;
    }
    
    QTGUI():QWidget()
    {
        fGeneralLayout = new QVBoxLayout;
        setLayout(fGeneralLayout);
        QWidget::show();
        
        fTimer = NULL;
        
        fMainWindow = new QMainWindow;
        QScrollArea *sa = new QScrollArea(fMainWindow);
        
        sa->setWidgetResizable(true);
        sa->setFrameShape(QFrame::NoFrame);
        sa->setWidget(this);
        
        fMainWindow->setCentralWidget(sa);
    }
    
    virtual ~QTGUI()
    {
        delete fGeneralLayout;
    }
    
    QString styleSheet()
    {
        QString styleSheet("");
        QFile file(fqtDark() ? ":/GreyDark.qss" : ":/Grey.qss");
        
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            styleSheet = QLatin1String(file.readAll());
            file.close();
        }
        
        return styleSheet;
    }
    
    /**
     * Analyses the widget zone metadata declarations and takes
     * appropriate actions
     */
    virtual void declare(FAUSTFLOAT* zone, const char* key, const char* value)
    {
        MetaDataUI::declare(zone, key, value);
    }
    
#if defined(HTTPCTRL) && defined(QRCODECTRL)
    
    //
    // Returns the IP address of the machine (to be qrcoded)
    //
    QString extractIPnum()
    {
        QList<QHostAddress> ipAdresses = QNetworkInterface::allAddresses();
        QList<QHostAddress>::iterator it;
        QString localhost("localhost");
        
        for (it = ipAdresses.begin(); it != ipAdresses.end(); it++) {
            if ((*it).protocol() == QAbstractSocket::IPv4Protocol && (*it) != QHostAddress::LocalHost) {
                return it->toString();
            } else if ((*it).protocol() == QAbstractSocket::IPv4Protocol && (*it) == QHostAddress::LocalHost) {
                localhost = it->toString();
            }
        }
        
        return localhost;
    }
    
    //
    // Used in HTTPD mode, display the QRCode of the URL of the application
    //
    void displayQRCode(int portnum)
    {
        QString url("http://");
        url += extractIPnum();
        url += ":";
        url += QString::number(portnum);
        displayQRCode(url, NULL);
    }
    
    void displayQRCode(const QString& url, QMainWindow* parent = NULL)
    {
        if (parent == NULL) {
            parent = new QMainWindow;
        }
        
        QWidget* centralWidget = new QWidget;
        parent->setCentralWidget(centralWidget);
        //    QTextEdit* httpdText = new QTextEdit(centralWidget);
        QTextBrowser* myBro = new QTextBrowser(centralWidget);
        
        //        //Construction of the flashcode
        //        const int padding = 5;
        //        QRcode* qrc = QRcode_encodeString(url.toLatin1().data(), 0, QR_ECLEVEL_H, QR_MODE_8, 1);
        //
        //        //   qDebug() << "QRcode width = " << qrc->width;
        //
        //        QRgb colors[2];
        //        colors[0] = qRgb(255, 255, 255); 	// 0 is white
        //        colors[1] = qRgb(0, 0, 0); 			// 1 is black
        //
        //        // build the QRCode image
        //        QImage image(qrc->width+2*padding, qrc->width+2*padding, QImage::Format_RGB32);
        //        // clear the image
        //        for (int y = 0; y < qrc->width+2*padding; y++) {
        //            for (int x = 0; x < qrc->width+2*padding; x++) {
        //                image.setPixel(x, y, colors[0]);
        //            }
        //        }
        //        // copy the qrcode inside
        //        for (int y = 0; y < qrc->width; y++) {
        //            for (int x = 0; x < qrc->width; x++) {
        //                image.setPixel(x+padding, y+padding, colors[qrc->data[y*qrc->width+x]&1]);
        //            }
        //        }
        
        const int padding = 5;
        QImage image = getQRCode (url, padding);
        //        QImage big = image.scaledToWidth(qrc->width*8);
        QImage big = image.scaledToWidth(image.width() * 8);
        QLabel* myLabel = new QLabel(centralWidget);
        
        fQrCode = QPixmap::fromImage(big);
        myLabel->setPixmap(fQrCode);
        
        //----Written Address
        
        QString sheet = QString::fromLatin1("a{ text-decoration: underline; color: white; font: Menlo; font-size: 14px }");
        //    myBro->document()->setDefaultStyleSheet(sheet);
        //    myBro->setStyleSheet("*{color: white; font: Menlo; font-size: 14px }");
        
        QString text("<br>Please connect to ");
        text += "<br><a href = " + url + ">"+ url+ "</a>";
        text += "<br>Or scan the QR code below";
        
        myBro->setOpenExternalLinks(true);
        myBro->setHtml(text);
        myBro->setAlignment(Qt::AlignCenter);
        myBro->setFixedWidth(big.width());
        //    myBro->setFixedHeight(myBro->minimumHeight());
        
        QGridLayout *mainLayout = new QGridLayout;
        mainLayout->addWidget(myBro, 0, 1);
        mainLayout->addWidget(myLabel, 1, 1);
        centralWidget->setLayout(mainLayout);
        centralWidget->show();
        centralWidget->adjustSize();
        parent->show();
    }
    
    bool toPNG(const QString& filename, QString& error)
    {
        QFile file(filename);
        if (file.open(QIODevice::WriteOnly)) {
            fQrCode.save(&file, "PNG");
            return true;
        } else {
            error = "Impossible to write file.";
            return false;
        }
    }
#endif
    
    virtual bool run()
    {
        if (!fTimer) {
            fTimer = new QTimer(this);
            QObject::connect(fTimer, SIGNAL(timeout()), this, SLOT(update()));
            fTimer->start(40);     // 25 fps: smoother bargraphs (was 100 ms)
        }
        
        if (fMainWindow) {
            // The stylesheet changes the size of every widget: apply it before
            // computing the window size (ca-qt.cpp applies it only after run())
            if (!fThemeButton) {
                addThemeButton();
                applyTheme();
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
                // follow the macOS appearance while the application is running
                QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this]() {
                    if (fqtThemeMode() == 0) applyTheme();
                });
#endif
            }
            fitWindowToContent();
            fMainWindow->show();
        }
        return true;
    }
    
    /**
     * Re-apply stylesheet and hand-painted colors after a theme change
     */
    void applyTheme()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        // also switch the native macOS window (title bar, frame) and the default palette
        static const Qt::ColorScheme schemes[] = { Qt::ColorScheme::Unknown, Qt::ColorScheme::Light, Qt::ColorScheme::Dark };
        QGuiApplication::styleHints()->setColorScheme(schemes[fqtThemeMode()]);
#endif
        qApp->setStyleSheet(styleSheet());
        if (fThemeButton) {
            static const char* names[] = { "Theme: follow macOS", "Theme: light", "Theme: dark" };
            static const char* icons[] = { "\u25D0", "\u25CB", "\u25CF" };   // half, empty, full circle
            fThemeButton->setText(QString::fromUtf8(icons[fqtThemeMode()]));
            fThemeButton->setToolTip(QString(names[fqtThemeMode()]) + "  (click to change)");
        }
        if (fMainWindow) fMainWindow->update();
    }
    
    /**
     * Small round button floating in the top right corner of the window:
     * cycles follow macOS -> light -> dark, the choice is saved per application.
     * It is not part of the layout, so it takes no space from the controls.
     */
    void addThemeButton()
    {
        fThemeButton = new QToolButton(fMainWindow);
        fThemeButton->setObjectName("themeButton");
        fThemeButton->setFixedSize(22, 22);
        fThemeButton->setCursor(Qt::PointingHandCursor);
        QObject::connect(fThemeButton, &QToolButton::clicked, this, [this]() {
            fqtThemeMode() = (fqtThemeMode() + 1) % 3;
            QSettings("Faust", QCoreApplication::applicationName()).setValue("theme", fqtThemeMode());
            applyTheme();
        });
        fMainWindow->installEventFilter(this);
        placeThemeButton();
    }
    
    void placeThemeButton()
    {
        if (fThemeButton) {
            fThemeButton->move(fMainWindow->width() - fThemeButton->width() - 6, 4);
            fThemeButton->raise();
        }
    }
    
    bool eventFilter(QObject* obj, QEvent* event) override
    {
        if (obj == fMainWindow && event->type() == QEvent::Resize) placeThemeButton();
        return QWidget::eventFilter(obj, event);
    }
    
    /**
     * Open the window as large as its content (the biggest tab page included),
     * limited to the available screen area: scrollbars appear only if the
     * interface is really bigger than the screen.
     */
    void fitWindowToContent()
    {
        ensurePolished();
        adjustSize();
        QSize content = sizeHint().expandedTo(minimumSizeHint());
        QScreen* screen = QGuiApplication::primaryScreen();
        QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1440, 900);
        // room for the title bar, and for a scrollbar if the other side does not fit
        int w = content.width() + 2;
        int h = content.height() + 2;
        int maxW = avail.width() - 20;
        int maxH = avail.height() - 40;
        if (h > maxH) w += fMainWindow->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
        if (w > maxW) h += fMainWindow->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
        fMainWindow->resize(qMin(w, maxW), qMin(h, maxH));
        fMainWindow->move(avail.x() + (avail.width() - fMainWindow->width()) / 2,
                          avail.y() + qMax(0, (avail.height() - fMainWindow->height()) / 3));
    }
    
    virtual void stop()
    {
        if (fTimer) {
            fTimer->stop();
            delete fTimer;
            fTimer = NULL;
        }
        
        GUI::stop();
    }
    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // OPEN AND CLOSE GROUPS
    //
    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    
    virtual void openHorizontalBox(const char* label)
    {
        openBox(label, new QHBoxLayout());
    }
    
    virtual void openVerticalBox(const char* label)
    {
        openBox(label, new QVBoxLayout());
    }
    
    virtual void openFrameBox(const char*)
    {}
    
    virtual void openTabBox(const char* label)
    {
        openTab(label);
    }
    
    virtual void closeBox()
    {
        QWidget* group = fGroupStack.top();
        fGroupStack.pop();
        if (fGroupStack.empty()) {
            group->show();
            group->adjustSize();
        }
    }
    
    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // ADD BUTTONS AND CHECKBOX
    //
    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    
    virtual void addButton(const char* label, FAUSTFLOAT* zone)
    {
        QAbstractButton* w = new QPushButton(label);
//        w->setAttribute(Qt::WA_MacNoClickThrough); // obsolete at least since Qt 5.11
        uiButton* c = new uiButton(this, zone, w);
        
        insert(label, w);
        QObject::connect(w, SIGNAL(pressed()), c, SLOT(pressed()));
        QObject::connect(w, SIGNAL(released()), c, SLOT(released()));
        checkForTooltip(zone, w);
        clearMetadata();
    }
    
    virtual void addToggleButton(const char*, FAUSTFLOAT*)
    {}
    
    virtual void addCheckButton(const char* label, FAUSTFLOAT* zone)
    {
        QCheckBox* w = new QCheckBox(label);
        uiCheckButton* c = new uiCheckButton(this, zone, w);
        
        insert(label, w);
        QObject::connect(w, SIGNAL(stateChanged(int)), c, SLOT(setState(int)));
        checkForTooltip(zone, w);
        clearMetadata();
    }
    
    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // ADD NUMERICAL ENTRY
    //
    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    
    virtual void addNumEntry(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step)
    {
        if (isKnob(zone)) {
            addVerticalKnob(label, zone, init, min, max, step);
            return;
        } else if (isRadio(zone)) {
            addVerticalRadioButtons(label, zone, init, min, max, step, fRadioDescription[zone].c_str());
            return;
        } else if (isMenu(zone)) {
            addMenu(label, zone, init, min, max, step, fMenuDescription[zone].c_str());
            return;
        }
        //insert(label, new QDoubleSpinBox());
        if (label && label[0]) { openVerticalBox(label); markControlBox(true); }
        QDoubleSpinBox* w = new QDoubleSpinBox();
        uiNumEntry* c = new uiNumEntry(this, zone, w, init, min, max, step);
        insert(label, w);
        std::string suffix = " " + fUnit[zone];
        w->setSuffix(suffix.c_str());
        QObject::connect(w, SIGNAL(valueChanged(double)), c, SLOT(setValue(double)));
        if (label && label[0]) closeBox();
        checkForTooltip(zone, w);
        clearMetadata();
    }
    
    // special num entry without buttons
    virtual void addNumDisplay(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step)
    {
        //insert(label, new QDoubleSpinBox());
        if (label && label[0]) openVerticalBox(label);
        QDoubleSpinBox* w = new QDoubleSpinBox();
        w->setAlignment(Qt::AlignHCenter);
        w->setObjectName("numDisplay");     // styled in Grey.qss
        w->setKeyboardTracking(false);
        uiNumEntry* c = new uiNumEntry(this, zone, w, init, min, max, step);
        insert(label, w);
        w->setButtonSymbols(QAbstractSpinBox::NoButtons);
        std::string suffix = " " + fUnit[zone];
        w->setSuffix(suffix.c_str());
        QObject::connect(w, SIGNAL(valueChanged(double)), c, SLOT(setValue(double)));
        if (label && label[0]) closeBox();
        checkForTooltip(zone, w);
        // Metadata is not cleared here, since it will be by the enclosing element calling addNumDisplay
    }
    
    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // ADD KNOBS
    //
    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    
    virtual void addVerticalKnob(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step)
    {
        openVerticalBox(label);
        markControlBox(false);
        QDial* w = new QDial(); //qsynthKnob();
        uiSlider* c = new uiSlider(this, zone, w, init, min, max, step, getScale(zone));
        insert(label, w);
        w->setStyle(new qsynthDialVokiStyle());
        w->setFocusPolicy(Qt::StrongFocus);
        w->setWrapping(DIAL_WRAPPING);
        QObject::connect(w, SIGNAL(valueChanged(int)), c, SLOT(setValue(int)));
        addNumDisplay(0, zone, init, min, max, step);
        
        // compute the size of the knob+display
        int width = int(FQT_KNOB_WIDTH * pow(2, fGuiSize[zone]));
        int height = int(FQT_KNOB_HEIGHT * pow(2, fGuiSize[zone]));
        fGroupStack.top()->setMinimumSize(width, height);
        fGroupStack.top()->setMaximumSize(width, height);
        
        closeBox();
        checkForTooltip(zone, w);
        clearMetadata();
    }
    
    virtual void addHorizontalKnob(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step)
    {
        openHorizontalBox(label);
        markControlBox(true);
        QDial* w = new QDial(); //new qsynthKnob();
        uiSlider* c = new uiSlider(this, zone, w, init, min, max, step, getScale(zone));
        insert(label, w);
        w->setStyle(new qsynthDialVokiStyle());
        w->setFocusPolicy(Qt::StrongFocus);
        w->setWrapping(DIAL_WRAPPING);
        QObject::connect(w, SIGNAL(valueChanged(int)), c, SLOT(setValue(int)));
        addNumDisplay(0, zone, init, min, max, step);
        closeBox();
        checkForTooltip(zone, w);
        clearMetadata();
    }
    
    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // ADD SLIDERS
    //
    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    
    virtual void addVerticalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step)
    {
        if (isKnob(zone)) {
            addVerticalKnob(label, zone, init, min, max, step);
            return;
        } else if (isRadio(zone)) {
            addVerticalRadioButtons(label, zone, init, min, max, step, fRadioDescription[zone].c_str());
            return;
        } else if (isMenu(zone)) {
            addMenu(label, zone, init, min, max, step, fMenuDescription[zone].c_str());
            return;
        }
        openVerticalBox(label);
        markControlBox(false);
        QSlider* w = new QSlider(Qt::Vertical);
        w->setMinimumHeight(FQT_SLIDER_LENGTH);
        w->setMinimumWidth(FQT_SLIDER_THICK);
        //w->setTickPosition(QSlider::TicksBothSides);
        uiSlider* c = new uiSlider(this, zone, w, init, min, max, step, getScale(zone));
        insert(label, w);
        QObject::connect(w, SIGNAL(valueChanged(int)), c, SLOT(setValue(int)));
        addNumDisplay(0, zone, init, min, max, step);
        closeBox();
        checkForTooltip(zone, w);
        clearMetadata();
    }
    
    virtual void addHorizontalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step)
    {
        if (isKnob(zone)) {
            addHorizontalKnob(label, zone, init, min, max, step);
            return;
        } else if (isRadio(zone)) {
            addHorizontalRadioButtons(label, zone, init, min, max, step, fRadioDescription[zone].c_str());
            return;
        } else if (isMenu(zone)) {
            addMenu(label, zone, init, min, max, step, fMenuDescription[zone].c_str());
            return;
        }
        openHorizontalBox(label);
        markControlBox(true);
        QSlider* w = new QSlider(Qt::Horizontal);
        w->setMinimumHeight(FQT_SLIDER_THICK);
        w->setMinimumWidth(FQT_SLIDER_LENGTH);
        //w->setTickPosition(QSlider::TicksBothSides);
        uiSlider* c = new uiSlider(this, zone, w, init, min, max, step, getScale(zone));
        insert(label, w);
        QObject::connect(w, SIGNAL(valueChanged(int)), c, SLOT(setValue(int)));
        addNumDisplay(0, zone, init, min, max, step);
        closeBox();
        checkForTooltip(zone, w);
        clearMetadata();
    }
    
    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // ADD RADIO-BUTTONS AND MENUS
    //
    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    
    virtual void addVerticalRadioButtons(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min,
                                         FAUSTFLOAT max, FAUSTFLOAT step, const char* mdescr)
    {
        uiRadioButtons* w = new uiRadioButtons(this, zone, label, init, min, max, step, true, mdescr, 0);
        insert(label, w);
        checkForTooltip(zone, w);
        clearMetadata();
    }
    
    virtual void addHorizontalRadioButtons(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min,
                                           FAUSTFLOAT max, FAUSTFLOAT step, const char* mdescr)
    {
        uiRadioButtons* w = new uiRadioButtons(this, zone, label, init, min, max, step, false, mdescr, 0);
        insert(label, w);
        checkForTooltip(zone, w);
        clearMetadata();
    }
    
    virtual void addMenu(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min,
                         FAUSTFLOAT max, FAUSTFLOAT step, const char* mdescr)
    {
        if (label && label[0]) { openVerticalBox(label); markControlBox(true); }
        uiMenu* w = new uiMenu(this, zone, label, init, min, max, step, mdescr, 0);
        insert(label, w);
        checkForTooltip(zone, w);
        if (label && label[0]) closeBox();
        clearMetadata();
    }
    
    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // ADD BARGRAPHS
    //
    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    
    virtual void addHorizontalBargraph(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT min, FAUSTFLOAT max)
    {
        openVerticalBox(label);
        markControlBox(true);
        if (isNumerical(zone)) {
            addNumDisplay(0, zone, min, min, max, displayStep(min, max));
        } else {
            AbstractDisplay* bargraph;
            bool db = (fUnit[zone] == "dB");
            if (isLed(zone)) {
                if (db) {
                    bargraph = new dbLED(min, max);
                } else {
                    bargraph = new LED(min, max);
                }
            } else {
                if (db) {
                    bargraph = new dbHorizontalBargraph(min, max);
                } else {
                    bargraph = new linHorizontalBargraph(min, max);
                }
            }
            
            new uiBargraph(this, zone, bargraph, min, max);
            insert(label, bargraph);
            checkForTooltip(zone, bargraph);
        }
        closeBox();
        clearMetadata();
    }
    
    virtual void addVerticalBargraph(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT min, FAUSTFLOAT max)
    {
        openVerticalBox(label);
        markControlBox(isNumerical(zone));
        if (isNumerical(zone)) {
            addNumDisplay(0, zone, min, min, max, displayStep(min, max));
        } else {
            AbstractDisplay* bargraph;
            bool db = (fUnit[zone] == "dB");
            if (isLed(zone)) {
                if (db) {
                    bargraph = new dbLED(min, max);
                } else {
                    bargraph = new LED(min, max);
                }
            } else {
                if (db) {
                    bargraph = new dbVerticalBargraph(min, max);
                } else {
                    bargraph = new linVerticalBargraph(min, max);
                }
            }
            new uiBargraph(this, zone, bargraph, min, max);
            insert(label, bargraph);
            addNumDisplay(0, zone, min, min, max, displayStep(min, max));
            checkForTooltip(zone, bargraph);
        }
        closeBox();
        clearMetadata();
    }
};

#ifdef _WIN32
# pragma warning (default: 4100)
#endif

#endif
/**************************  END  QTUI.h **************************/
