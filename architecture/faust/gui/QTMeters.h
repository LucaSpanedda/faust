/************************** BEGIN QTMeters.h **************************
 Analysis strip for the Faust QT applications (Luca Spanedda), modelled on
 MiniMeters: waveform flowing into the oscilloscope, levels/loudness,
 stereometer and spectrum (or spectrogram), above the controls of the QTGUI
 window, and a status bar below them (CPU per core, DSP load, block size).

 Everything shows two signals, L and R. A routing matrix (round button in the
 top left corner of the strip) decides which inputs and outputs of the DSP
 go to L and to R; several channels on the same side are averaged.

 - MeterDSP wraps the DSP and copies its input and output buffers into
   lock-free ring buffers (audio thread: plain memory copies only).
 - The modules read the rings from the GUI thread, do all the analysis
   (levels, K-weighted loudness, FFT, correlation) and paint with the theme
   colors of QTUI.h (FQT_* macros), in light and dark mode.
 - The modules are drawn by the GPU (QOpenGLWidget, 4x multisampling) at
   60 fps; -DFQT_METERS_NO_GL draws them with the CPU (QWidget) at 25 fps.
 - Right click on a module: its display options. Options and routing are
   saved per application (QSettings "Faust"/<application>, group "meters").

 - The controls are shown in a FitView: when the window is smaller than the
   interface, the whole interface is scaled down proportionally instead of
   scrolling.

 Used by ca-qt-meters.cpp (faust2caqt-meters). Include after QTUI.h.
 ************************************************************************/

#ifndef __QTMeters__
#define __QTMeters__

#include <atomic>
#include <chrono>
#include <climits>
#include <complex>
#include <deque>
#include <functional>
#include <vector>

#include <QActionGroup>
#include <QContextMenuEvent>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QMenu>
#ifndef FQT_METERS_NO_GL
#include <QOpenGLWidget>
#include <QSurfaceFormat>
#endif
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "faust/dsp/dsp.h"

#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#include <mach/mach.h>
#include <sys/resource.h>
#endif

//==============================RING BUFFER=====================================

/**
 * Multichannel ring buffer, one writer (audio thread), one reader (GUI thread).
 * The reader only looks at recent samples, far from the writing position,
 * so no lock is needed: a torn sample would only affect the display.
 */
class MeterRing
{

    public:

        static const int kSize = 1 << 17;   // samples per channel (~2.7 s at 48 kHz)

    private:

        int fChannels;
        std::vector<float> fData;
        std::atomic<uint64_t> fWritten;

    public:

        MeterRing(int channels) : fChannels(channels), fData(size_t(channels) * kSize, 0.f), fWritten(0) {}

        int channels() const { return fChannels; }

        // absolute position of the next sample to be written
        uint64_t written() const { return fWritten.load(std::memory_order_acquire); }

        // audio thread
        void push(FAUSTFLOAT** buffers, int count)
        {
            uint64_t w = fWritten.load(std::memory_order_relaxed);
            for (int c = 0; buffers && c < fChannels; c++) {
                float* dst = &fData[size_t(c) * kSize];
                const FAUSTFLOAT* src = buffers[c];
                for (int i = 0; i < count; i++) dst[(w + i) & (kSize - 1)] = float(src[i]);
            }
            fWritten.store(w + count, std::memory_order_release);
        }

        // copy 'n' samples of channel 'c' starting at absolute position 'start'
        void read(int c, uint64_t start, float* out, int n) const
        {
            if (c < 0 || c >= fChannels) {
                std::fill(out, out + n, 0.f);
                return;
            }
            const float* src = &fData[size_t(c) * kSize];
            for (int i = 0; i < n; i++) out[i] = src[(start + i) & (kSize - 1)];
        }
};

/**
 * What the audio thread measures for the status bar: the block size actually
 * asked by the driver (CoreAudio never tells it otherwise) and the time spent
 * in the DSP compute, compared with the duration of the block (DSP load).
 * Atomics only; the GUI thread reads and resets the per-period values.
 */
struct MeterTiming
{
    std::atomic<int> fBlock{0};                 // last block size
    std::atomic<int> fBlockMin{INT_MAX};        // since the last read
    std::atomic<int> fBlockMax{0};
    std::atomic<uint64_t> fBusyNs{0};           // total time in the DSP compute
    std::atomic<uint64_t> fSamples{0};          // total samples computed
    std::atomic<uint64_t> fWorst{0};            // worst block since the last read, ns per 1024 samples
    std::atomic<uint64_t> fLate{0};             // blocks computed in more time than their duration
    int fSampleRate = 0;

    void record(int count, uint64_t ns)
    {
        fBlock.store(count, std::memory_order_relaxed);
        if (count < fBlockMin.load(std::memory_order_relaxed)) fBlockMin.store(count, std::memory_order_relaxed);
        if (count > fBlockMax.load(std::memory_order_relaxed)) fBlockMax.store(count, std::memory_order_relaxed);
        fBusyNs.fetch_add(ns, std::memory_order_relaxed);
        fSamples.fetch_add(uint64_t(count), std::memory_order_relaxed);
        uint64_t perK = count > 0 ? ns * 1024 / uint64_t(count) : 0;
        if (perK > fWorst.load(std::memory_order_relaxed)) fWorst.store(perK, std::memory_order_relaxed);
        if (fSampleRate > 0 && ns * uint64_t(fSampleRate) > uint64_t(count) * 1000000000ull) fLate.fetch_add(1, std::memory_order_relaxed);
    }
};

/**
 * DSP decorator feeding the rings. The wrapped DSP is not owned: it is
 * deleted by the architecture file as usual.
 */
class MeterDSP : public decorator_dsp
{

    private:

        MeterRing fIn;
        MeterRing fOut;
        MeterTiming fTiming;

    public:

        MeterDSP(::dsp* dsp) : decorator_dsp(dsp), fIn(dsp->getNumInputs()), fOut(dsp->getNumOutputs()) {}
        virtual ~MeterDSP() { fDSP = nullptr; }

        MeterRing& inputs() { return fIn; }
        MeterRing& outputs() { return fOut; }
        MeterTiming& timing() { return fTiming; }

        virtual void init(int sample_rate)
        {
            fTiming.fSampleRate = sample_rate;
            decorator_dsp::init(sample_rate);
        }

        virtual void compute(int count, FAUSTFLOAT** inputs, FAUSTFLOAT** outputs)
        {
            fIn.push(inputs, count);        // before compute: inputs may be reused in place
            auto t0 = std::chrono::steady_clock::now();
            fDSP->compute(count, inputs, outputs);
            auto t1 = std::chrono::steady_clock::now();
            fOut.push(outputs, count);
            fTiming.record(count, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        }

        virtual void compute(double /*date_usec*/, int count, FAUSTFLOAT** inputs, FAUSTFLOAT** outputs)
        {
            compute(count, inputs, outputs);
        }
};

//==============================SHARED STATE=====================================

/**
 * The two signals shown by all the modules, L and R: each one is the average
 * of the inputs and outputs of the DSP routed to it (sum divided by the number
 * of channels, so that the level stays comparable to a single channel). By default
 * L = output 1 and R = output 2 (inputs 1 and 2 for a DSP without outputs).
 */
struct MeterSource
{
    enum { kLeft = 0, kRight = 1 };

    MeterDSP* fDSP = nullptr;
    int fSampleRate = 48000;
    std::vector<char> fRoute[2][2];     // [side][0: inputs, 1: outputs] -> routed
    int fVersion = 0;                   // incremented when the routing changes
    mutable std::vector<float> fTmp;

    MeterRing& ring(int outputs) const { return outputs ? fDSP->outputs() : fDSP->inputs(); }

    // most recent position written in both rings (they advance together)
    uint64_t clock() const { return std::min(fDSP->inputs().written(), fDSP->outputs().written()); }

    void setDefault()
    {
        int io = fDSP->outputs().channels() > 0 ? 1 : 0;
        int n = ring(io).channels();
        for (int side = 0; side < 2; side++) {
            for (int k = 0; k < 2; k++) fRoute[side][k].assign(ring(k).channels(), 0);
            if (n > 0) fRoute[side][io][std::min(side, n - 1)] = 1;
        }
        fVersion++;
    }

    void toggle(int side, int outputs, int channel)
    {
        char& r = fRoute[side][outputs][channel];
        r = !r;
        fVersion++;
    }

    // "o1,o3" <-> routing of one side
    QString save(int side) const
    {
        QStringList l;
        for (int k = 0; k < 2; k++)
            for (int c = 0; c < int(fRoute[side][k].size()); c++)
                if (fRoute[side][k][c]) l << QString("%1%2").arg(k ? "o" : "i").arg(c + 1);
        return l.join(",");
    }

    void load(int side, const QString& text)
    {
        for (int k = 0; k < 2; k++) fRoute[side][k].assign(ring(k).channels(), 0);
        for (const QString& t : text.split(",", Qt::SkipEmptyParts)) {
            int k = t.startsWith("o") ? 1 : 0, c = t.mid(1).toInt() - 1;
            if (c >= 0 && c < int(fRoute[side][k].size())) fRoute[side][k][c] = 1;
        }
        fVersion++;
    }

    // short name of one side, e.g. "O1", "I2", "O1+O3", "4ch" (more than two channels), "-"
    QString label(int side) const
    {
        QStringList l;
        for (int k = 1; k >= 0; k--)
            for (int c = 0; c < int(fRoute[side][k].size()); c++)
                if (fRoute[side][k][c]) l << QString("%1%2").arg(k ? "O" : "I").arg(c + 1);
        if (l.isEmpty()) return "-";
        return l.size() > 2 ? QString("%1ch").arg(l.size()) : l.join("+");
    }

    // 'n' samples of one side starting at absolute position 'start': average of the routed channels
    void readSide(int side, uint64_t start, float* out, int n) const
    {
        std::fill(out, out + n, 0.f);
        fTmp.resize(std::max(n, 1));
        int count = 0;
        for (int k = 0; k < 2; k++) {
            for (int c = 0; c < int(fRoute[side][k].size()); c++) {
                if (!fRoute[side][k][c]) continue;
                ring(k).read(c, start, fTmp.data(), n);
                for (int i = 0; i < n; i++) out[i] += fTmp[i];
                count++;
            }
        }
        if (count > 1) {
            float g = 1.f / count;
            for (int i = 0; i < n; i++) out[i] *= g;
        }
    }

    // the last 'n' samples of L and R
    void readPair(float* left, float* right, int n) const
    {
        uint64_t start = clock() - uint64_t(n);
        readSide(kLeft, start, left, n);
        readSide(kRight, start, right, n);
    }
};

static inline QFont fqtMeterFont(int size = 9, bool bold = true)
{
    QFont f("Menlo");
    f.setStyleHint(QFont::Monospace);
    f.setPointSize(size);
    f.setBold(bold);
    return f;
}

static inline QFont fqtReadoutFont() { return fqtMeterFont(11); }

static inline float fqtDB(float lin) { return 20.f * std::log10(std::max(lin, 1e-9f)); }

#define FQT_METER_BG    FQT_COLOR(0xffffff, 0x1c1c1e)   // module background

// pen of a fixed width in device pixels: drawn by the fast path of the rasterizer
static inline QPen fqtLinePen(const QColor& color, double width = 2.0)
{
    QPen pen(color, width);
    pen.setCosmetic(true);
    return pen;
}
#define FQT_CLIP        QColor(0xe0, 0x30, 0x20)        // peak hold at 0 dBFS

#ifdef FQT_METERS_NO_GL
typedef QWidget MeterSurface;
static const int kMeterFrameMs = 40;    // 25 fps
#else
typedef QOpenGLWidget MeterSurface;
static const int kMeterFrameMs = 16;    // 60 fps
#endif

/**
 * Base class of a module: background, options menu, update from the GUI
 * timer. Modules draw in paintGL() with a QPainter, on the GPU or the CPU.
 * Mode chips: the main display modes of a module, shown as small segmented
 * buttons at its top while the mouse is over it (nothing at rest); a click
 * selects. The right click menu keeps all the options.
 */
class MeterModule : public MeterSurface
{

    protected:

        const MeterSource& fSource;
        QString fKey;   // settings group of the module

        int option(const char* name, int def) const
        {
            return QSettings("Faust", QCoreApplication::applicationName()).value("meters/" + fKey + "/" + name, def).toInt();
        }

        void setOption(const char* name, int value)
        {
            QSettings("Faust", QCoreApplication::applicationName()).setValue("meters/" + fKey + "/" + name, value);
        }

        // a group of checkable entries in the options menu, bound to an int member
        void addChoice(QMenu* menu, const QString& title, const char* name, int& member, const QStringList& labels, const QList<int>& values)
        {
            // a disabled entry as header (QMenu::addSection titles are not shown on macOS)
            if (!menu->isEmpty()) menu->addSeparator();
            menu->addAction(title)->setEnabled(false);
            QActionGroup* group = new QActionGroup(menu);
            for (int i = 0; i < labels.size(); i++) {
                QAction* a = menu->addAction(labels[i]);
                a->setCheckable(true);
                a->setChecked(member == values[i]);
                group->addAction(a);
                int v = values[i];
                QObject::connect(a, &QAction::triggered, [this, name, &member, v]() {
                    member = v;
                    setOption(name, v);
                    optionsChanged();
                });
            }
        }

        virtual void fillMenu(QMenu*) = 0;
        virtual void optionsChanged() {}

        void paintBackground(QPainter& p) { p.fillRect(rect(), FQT_METER_BG); }

        // mode chips: one group per option, bound to an int member like addChoice
        struct ChipGroup { const char* name; int* member; QStringList labels; QList<int> values; };
        struct Chip { QRectF rect; const char* name; int* member; int value; };
        std::vector<Chip> fChips;
        bool fHover = false;

        virtual std::vector<ChipGroup> chipGroups() { return {}; }
        virtual QPointF chipOrigin() const { return QPointF(6, 5); }

        // called at the end of paintGL()
        void paintChips(QPainter& p)
        {
            fChips.clear();
            if (!fHover) return;
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setFont(fqtMeterFont(8, false));
            QFontMetricsF fm(p.font());
            const float h = 16.f, pad = 7.f;
            QPointF o = chipOrigin();
            float x = float(o.x());
            for (const ChipGroup& g : chipGroups()) {
                float w = 2.f;
                for (const QString& l : g.labels) w += float(fm.horizontalAdvance(l)) + 2 * pad;
                QRectF box(x, o.y(), w, h);
                p.setPen(QPen(FQT_BORDER, 1));
                p.setBrush(FQT_METER_BG);
                p.drawRoundedRect(box, h / 2, h / 2);
                float sx = x + 1.f;
                for (int i = 0; i < g.labels.size(); i++) {
                    QRectF seg(sx, o.y(), float(fm.horizontalAdvance(g.labels[i])) + 2 * pad, h);
                    bool on = (*g.member == g.values[i]);
                    if (on) {
                        p.setPen(Qt::NoPen);
                        p.setBrush(FQT_ACCENT);
                        p.drawRoundedRect(seg.adjusted(1.5, 1.5, -1.5, -1.5), (h - 3) / 2, (h - 3) / 2);
                    }
                    p.setPen(on ? FQT_METER_BG : FQT_TEXT);
                    p.drawText(seg, Qt::AlignCenter, g.labels[i]);
                    fChips.push_back({ seg, g.name, g.member, g.values[i] });
                    sx = float(seg.right());
                }
                x = float(box.right()) + 6.f;
            }
            p.setBrush(Qt::NoBrush);
        }

    public:

        MeterModule(const MeterSource& src, const QString& key) : fSource(src), fKey(key)
        {
#ifdef FQT_METERS_NO_GL
            setAttribute(Qt::WA_OpaquePaintEvent);
#else
            QSurfaceFormat f = format();
            f.setSamples(4);    // antialiased lines
            setFormat(f);
#endif
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            setToolTip("Right click: all options");
        }

        virtual void enterEvent(QEnterEvent*) { fHover = true; update(); }
        virtual void leaveEvent(QEvent*) { fHover = false; update(); }

        virtual void mousePressEvent(QMouseEvent* e)
        {
            if (e->button() != Qt::LeftButton) return;
            for (const Chip& c : fChips) {
                if (c.rect.contains(e->position())) {
                    *c.member = c.value;
                    setOption(c.name, c.value);
                    optionsChanged();
                    update();
                    return;
                }
            }
        }

        virtual void tick() = 0;    // read the ring and analyse (GUI thread, once per frame)

#ifdef FQT_METERS_NO_GL
        virtual void paintGL() = 0;
        virtual void paintEvent(QPaintEvent*) { paintGL(); }
#endif

        virtual void contextMenuEvent(QContextMenuEvent* e)
        {
            QMenu menu(this);
            fillMenu(&menu);
            menu.exec(e->globalPos());
        }
};

//==============================WAVEFORM + OSCILLOSCOPE==========================

/**
 * Scrolling waveform flowing into the oscilloscope, on a single time line,
 * in two lanes (L and R, or Mid and Side):
 * - on the left the history, each column is the min/max of 10 ms by default;
 *   it ends at a fixed delay from now, so it scrolls steadily;
 * - then a lens, where the time per pixel shrinks geometrically down to the
 *   zoom of the scope: the envelope thins into the signal itself;
 * - on the right the oscilloscope, starting where the lens ends.
 * With Follow Pitch the scope starts on a rising zero crossing of the first
 * lane and shows one or four periods (same trigger as before); the lens spans
 * from the end of the history to the start of the scope, so the three parts
 * stay continuous and every trace passes through the junction.
 * Options: persistence of the older traces and glow (both off by default),
 * Zoom (1:1 by default) scaling the whole module so that the louder visible
 * part fills 90% of the lanes (shown as xN).
 */
class WaveScopeModule : public MeterModule
{

    private:

        static const int kColumns = 4000;           // history columns (wide windows at 10 ms)
        static const int kWindow = 16384;           // samples searched by the trigger
        static const int kMaxGhosts = 16;

        struct Column { float lo, hi; };
        struct Geometry { QRectF all; float xa, xj, scopeW; int lensW; };

        // history
        std::deque<Column> fCols[2];
        float fLo[2] = { 0.f, 0.f }, fHi[2] = { 0.f, 0.f };
        int fCount = 0;
        int fVersion = -1;
        uint64_t fRead = 0;                         // next sample folded into the columns
        uint64_t fColEnd = 0;                       // absolute end (excluded) of the newest column
        uint64_t fHistEnd = 0;                      // end of the drawn history = start of the lens (fixed delay from now)
        int fPerColumn = 480;

        // lens: min/max per pixel, from the junction leftwards
        std::vector<Column> fLens[2];

        // scope: start and length of the trace, older traces (u 0..1, v -1..1)
        uint64_t fT0 = 0;
        int fLen = 2048;
        float fFreq = 0.f;
        std::deque<std::vector<QPointF>> fTraces[2];

        // options
        int fSpeed = 1;
        int fMidSide = 0;
        int fFollow = 1;
        int fCycles = 1;
        int fPersistence = 0;
        int fGlow = 0;
        int fAutoGain = 0;
        float fGain = 1.f;                          // zoom of the whole module (Zoom)

        std::vector<float> fBuf[2];
        std::vector<QLineF> fLines;
        std::vector<QPointF> fPoints;

        virtual void fillMenu(QMenu* menu)
        {
            addChoice(menu, "Display", "midside", fMidSide, { "Left / Right", "Mid / Side" }, { 0, 1 });
            addChoice(menu, "History", "speed", fSpeed, { "Slow (40 ms / pixel)", "Normal (10 ms / pixel)", "Fast (2.5 ms / pixel)" }, { 0, 1, 2 });
            addChoice(menu, "Follow Pitch", "follow", fFollow, { "On", "Off" }, { 1, 0 });
            addChoice(menu, "Cycle", "cycles", fCycles, { "Single", "Multi" }, { 1, 4 });
            addChoice(menu, "Persistence", "persistence", fPersistence, { "Off", "Short", "Long" }, { 0, 6, kMaxGhosts });
            addChoice(menu, "Glow", "glow", fGlow, { "On", "Off" }, { 1, 0 });
            addChoice(menu, "Scale", "zoom", fAutoGain, { "1:1", "Zoom (on quiet signals)" }, { 0, 1 });
        }

        virtual void optionsChanged() { fVersion = -1; }   // restart the history

        virtual std::vector<ChipGroup> chipGroups()
        {
            return { { "midside", &fMidSide, { "L/R", "M/S" }, { 0, 1 } },
                     { "zoom", &fAutoGain, { "1:1", "Zoom" }, { 0, 1 } } };
        }

        virtual QPointF chipOrigin() const { return QPointF(34, 8); }     // right of the routing button

        Geometry geometry() const
        {
            Geometry g;
            g.all = QRectF(rect()).adjusted(2, 30, -2, -4);     // below the routing button
            float w = float(g.all.width());
            g.scopeW = std::floor(w * 0.38f);
            g.lensW = int(std::max(16.f, w * 0.16f));
            g.xj = float(g.all.right()) - g.scopeW;
            g.xa = g.xj - g.lensW;
            return g;
        }

        // 'n' samples of both lanes (L/R or M/S) from absolute position 'start'
        void readLanes(uint64_t start, int n)
        {
            for (int k = 0; k < 2; k++) {
                fBuf[k].resize(std::max(n, 1));
                fSource.readSide(k, start, fBuf[k].data(), n);
            }
            if (fMidSide) {
                for (int i = 0; i < n; i++) {
                    float l = fBuf[0][i], r = fBuf[1][i];
                    fBuf[0][i] = 0.5f * (l + r);
                    fBuf[1][i] = 0.5f * (l - r);
                }
            }
        }

        // samples covered by the pixels of the lens, S[i] = samples between the junction and pixel i:
        // geometric steps starting at the zoom of the scope, scaled so that the lens spans exactly
        // 'total' samples (from the start of the scope back to the end of the history)
        std::vector<double> lensSpans(const Geometry& g, double total) const
        {
            int n = g.lensW;
            std::vector<double> s(n + 1, 0.0);
            double a = double(fLen) / std::max(1.f, g.scopeW);
            double r = 1.0;
            if (total > a * n) {
                // a * (r^n - 1) / (r - 1) = total, solved by bisection
                auto sum = [&](double q) { return a * (std::pow(q, n) - 1.0) / (q - 1.0); };
                double lo = 1.0 + 1e-9, hi = 4.0;
                for (int it = 0; it < 60; it++) {
                    double mid = 0.5 * (lo + hi);
                    if (sum(mid) < total) lo = mid; else hi = mid;
                }
                r = 0.5 * (lo + hi);
            } else {
                a = total / n;      // short span: uniform steps
            }
            double step = a;
            for (int i = 0; i < n; i++) {
                s[i + 1] = s[i] + step;
                step *= r;
            }
            return s;
        }

        // trace of lane 'k' (fBuf, from 'offset'), at most two points per pixel
        std::vector<QPointF> trace(int k, int offset, int pixels) const
        {
            std::vector<QPointF> pts;
            const float* x = fBuf[k].data() + offset;
            float gain = fGain;
            auto v = [gain](float s) { return std::min(1.f, std::max(-1.f, s * gain)); };
            if (fLen <= 2 * pixels) {
                for (int i = 0; i < fLen; i++) pts.emplace_back(double(i) / (fLen - 1), v(x[i]));
                return pts;
            }
            // min and max of each pixel, in the order they occur
            for (int p = 0; p < pixels; p++) {
                int i0 = int(int64_t(p) * fLen / pixels), i1 = std::max(i0 + 1, int(int64_t(p + 1) * fLen / pixels));
                int imin = i0, imax = i0;
                for (int i = i0; i < i1; i++) {
                    if (x[i] < x[imin]) imin = i;
                    if (x[i] > x[imax]) imax = i;
                }
                int a = std::min(imin, imax), b = std::max(imin, imax);
                pts.emplace_back(double(a) / (fLen - 1), v(x[a]));
                if (b != a) pts.emplace_back(double(b) / (fLen - 1), v(x[b]));
            }
            return pts;
        }

        void paintTrace(QPainter& p, const std::vector<QPointF>& t, const Geometry& g, float cy, float amp)
        {
            fPoints.clear();
            for (const QPointF& pt : t) fPoints.emplace_back(g.xj + pt.x() * g.scopeW, cy - pt.y() * amp);
            p.drawPolyline(fPoints.data(), int(fPoints.size()));
        }

    public:

        WaveScopeModule(const MeterSource& src) : MeterModule(src, "wavescope")
        {
            setMinimumWidth(160);
            fSpeed = option("speed", 1);
            fMidSide = option("midside", 0);
            fFollow = option("follow", 1);
            fCycles = option("cycles", 1);
            fPersistence = std::min(int(kMaxGhosts), std::max(0, option("persistence", 0)));
            fGlow = option("glow", 0);
            fAutoGain = option("zoom", 0);
        }

        virtual void tick()
        {
            uint64_t w = fSource.clock();
            static const int tenthsOfMs[] = { 400, 100, 25 };   // duration of a column
            fPerColumn = std::max(1, fSource.fSampleRate * tenthsOfMs[fSpeed] / 10000);
            if (fVersion != fSource.fVersion) {
                // new routing or options: restart the history
                fVersion = fSource.fVersion;
                fRead = fColEnd = w;
                fCount = 0;
                for (int k = 0; k < 2; k++) { fCols[k].clear(); fTraces[k].clear(); }
            }

            // history: fold the new samples into columns
            int n = int(std::min<uint64_t>(w - fRead, MeterRing::kSize / 2));
            uint64_t start = w - n;
            readLanes(start, n);
            for (int i = 0; i < n; i++) {
                for (int k = 0; k < 2; k++) {
                    float v = fBuf[k][i];
                    if (fCount == 0) fLo[k] = fHi[k] = v;
                    fLo[k] = std::min(fLo[k], v);
                    fHi[k] = std::max(fHi[k], v);
                }
                if (++fCount == fPerColumn) {
                    for (int k = 0; k < 2; k++) {
                        fCols[k].push_back({ fLo[k], fHi[k] });
                        if (int(fCols[k].size()) > kColumns) fCols[k].pop_front();
                    }
                    fColEnd = start + i + 1;
                    fCount = 0;
                }
            }
            fRead = w;

            // scope: rising zero crossings of the first lane with a small hysteresis, searched backwards
            uint64_t winStart = w - kWindow;
            readLanes(winStart, kWindow);
            int len = 2048, s = kWindow - len;
            fFreq = 0.f;
            if (fFollow) {
                std::vector<int> ups;
                bool below = false;
                const std::vector<float>& b = fBuf[0];
                for (int i = kWindow - 1; i > 0 && int(ups.size()) <= fCycles + 1; i--) {
                    if (b[i] < -1e-4f) below = true;
                    if (below && b[i - 1] <= 0.f && b[i] > 0.f) { ups.push_back(i); below = false; }
                }
                if (int(ups.size()) > fCycles + 1) {
                    int period = ups[0] - ups[1];
                    len = std::min(std::max(fCycles * period, 32), kWindow / 2);
                    s = ups[fCycles + 1];
                    if (s + len > kWindow) s = kWindow - len;
                    fFreq = float(fSource.fSampleRate) / std::max(1, period);
                }
            }
            fT0 = winStart + s;
            fLen = len;
            // zoom: the louder visible part (scope and history) fills 90% of the lanes, at most x16;
            // immediate when the signal grows, slow when it decays
            Geometry g = geometry();
            float target = 1.f;
            if (fAutoGain) {
                float pk = 0.f;
                int visible = int(g.xa - g.all.left());
                for (int k = 0; k < 2; k++) {
                    for (int i = s; i < s + len; i++) pk = std::max(pk, std::fabs(fBuf[k][i]));
                    const std::deque<Column>& cols = fCols[k];
                    for (int i = std::max(0, int(cols.size()) - visible); i < int(cols.size()); i++)
                        pk = std::max(pk, std::max(std::fabs(cols[i].lo), std::fabs(cols[i].hi)));
                }
                target = std::min(16.f, std::max(1.f, 0.9f / std::max(pk, 1e-6f)));
            }
            fGain = (target < fGain) ? target : fGain + (target - fGain) * 0.05f;
            int pixels = std::max(2, int(g.scopeW));
            for (int k = 0; k < 2; k++) {
                fTraces[k].push_front(trace(k, s, pixels));
                while (int(fTraces[k].size()) > fPersistence + 1) fTraces[k].pop_back();
            }

            // lens: pixel i covers [T0 - S[i+1], T0 - S[i]], the linearly interpolated signal is rasterized.
            // The history ends at a fixed delay from now (the oldest start of the scope), so it scrolls
            // steadily while the start of the scope follows the trigger
            fHistEnd = w - kWindow;
            std::vector<double> spans = lensSpans(g, double(fT0 - fHistEnd));
            int lensSamples = int(std::ceil(spans[g.lensW])) + 2;
            uint64_t lensStart = fT0 - uint64_t(lensSamples);
            readLanes(lensStart, lensSamples + 2);
            for (int k = 0; k < 2; k++) {
                const std::vector<float>& b = fBuf[k];
                auto at = [&](double t) {
                    int i = std::max(0, std::min(int(t), lensSamples));
                    float f = float(t - i);
                    return b[i] * (1 - f) + b[i + 1] * f;
                };
                fLens[k].resize(g.lensW);
                for (int i = 0; i < g.lensW; i++) {
                    double t0 = lensSamples - spans[i + 1], t1 = lensSamples - spans[i];
                    float va = at(t0), vb = at(t1);
                    float lo = std::min(va, vb), hi = std::max(va, vb);
                    for (int j = int(t0) + 1; j < t1; j++) { lo = std::min(lo, b[j]); hi = std::max(hi, b[j]); }
                    fLens[k][i] = { lo, hi };
                }
            }
            update();
        }

        virtual void paintGL()
        {
            QPainter p(this);
            paintBackground(p);
            Geometry g = geometry();
            float lh = float(g.all.height()) / 2;
            static const char* names[2][2] = { { "L", "R" }, { "M", "S" } };
            // newest history column that ends before the lens begins
            int skip = int(std::max<int64_t>(0, (int64_t(fColEnd) - int64_t(fHistEnd) + fPerColumn - 1) / fPerColumn));

            for (int k = 0; k < 2; k++) {
                float cy = float(g.all.top()) + lh * (k + 0.5f), amp = lh * 0.45f;
                auto y = [&](float v) { return cy - std::min(1.f, std::max(-1.f, v * fGain)) * amp; };
                p.setRenderHint(QPainter::Antialiasing, false);
                p.fillRect(QRectF(g.all.left(), cy, g.all.width(), 1), FQT_TRACK);

                // history: one vertical line per pixel (on the pixel centers, as the lens: no gap between them)
                fLines.clear();
                const std::deque<Column>& cols = fCols[k];
                int visible = int(g.xa - g.all.left());
                for (int i = 0; i < visible && skip + i < int(cols.size()); i++) {
                    const Column& c = cols[cols.size() - 1 - skip - i];
                    float x = g.xa - 0.5f - i, y0 = y(c.hi), y1 = y(c.lo);
                    fLines.emplace_back(x, y0, x, std::max(y1, y0 + 1));
                }
                p.setPen(QPen(FQT_ACCENT, 1));
                p.drawLines(fLines.data(), int(fLines.size()));

                // lens: a smooth band from max to min, outlined so that it never gets thinner than the scope line
                int nl = std::min(int(fLens[k].size()), g.lensW);
                if (nl > 1) {
                    QPolygonF band(2 * nl);
                    for (int i = 0; i < nl; i++) {
                        const Column& c = fLens[k][i];
                        float x = g.xj - 0.5f - i;
                        band[i] = QPointF(x, y(c.hi));
                        band[2 * nl - 1 - i] = QPointF(x, y(c.lo));
                    }
                    p.setRenderHint(QPainter::Antialiasing, true);
                    p.setPen(fqtLinePen(FQT_ACCENT, 2.0));
                    p.setBrush(FQT_ACCENT);
                    p.drawPolygon(band);
                    p.setBrush(Qt::NoBrush);
                }

                // scope: older traces fading out, then the newest (with its glow)
                p.setRenderHint(QPainter::Antialiasing, true);
                QColor c = FQT_ACCENT;
                const std::deque<std::vector<QPointF>>& traces = fTraces[k];
                for (int i = int(traces.size()) - 1; i >= 1; i--) {
                    float fade = 1.f - float(i) / (fPersistence + 1);
                    c.setAlphaF(0.45f * fade * fade);
                    p.setPen(fqtLinePen(c, 1.5));
                    paintTrace(p, traces[i], g, cy, amp);
                }
                if (!traces.empty()) {
                    if (fGlow) {
                        c.setAlphaF(fqtDark() ? 0.22f : 0.14f);
                        p.setPen(fqtLinePen(c, 7.0));
                        paintTrace(p, traces[0], g, cy, amp);
                    }
                    p.setPen(fqtLinePen(FQT_ACCENT, 2.0));
                    paintTrace(p, traces[0], g, cy, amp);
                }

                p.setFont(fqtMeterFont(8));
                p.setPen(FQT_TEXT_DIM);
                p.drawText(QRectF(g.all.left() + 2, cy - lh / 2, 20, lh), Qt::AlignLeft | Qt::AlignVCenter, names[fMidSide][k]);
            }

            // period followed by the scope and its zoom
            QStringList info;
            if (fGain > 1.05f) info << QString::fromUtf8("\u00d7%1").arg(fGain, 0, 'f', 1);
            if (fFreq > 0.f) info << QString("%1 Hz").arg(fFreq, 0, 'f', fFreq < 100.f ? 1 : 0);
            if (!info.isEmpty()) {
                p.setFont(fqtMeterFont(8, false));
                p.setPen(FQT_TEXT_DIM);
                p.drawText(QRectF(g.xj, g.all.bottom() - 12, g.scopeW - 2, 12), Qt::AlignRight | Qt::AlignVCenter, info.join("  "));
            }
            paintChips(p);
        }
};

//==============================LEVELS=====================================

/**
 * Peak and level of L and R, linear dB scale 0..-60 (MiniMeters style), peak
 * hold. Level modes as the MiniMeters Loudness module: RMS fast (0.3 s), RMS
 * slow (1 s), LUFS momentary (0.4 s) and short-term (3 s), K-weighted as in
 * ITU-R BS.1770. The readout shows the louder side (RMS) or the loudness of
 * L + R (LUFS). LUFS Integrated: gated loudness since the start or the last
 * reset (absolute gate -70 LUFS, relative gate -10 LU, 400 ms blocks every
 * 100 ms), the bars show the momentary level. A click on the bars (or on
 * the readout) selects the next mode; the menu resets the integrated loudness and the
 * maximum peak (in the tooltip).
 */
class LevelsModule : public MeterModule
{

    private:

        enum { kRMSFast, kRMSSlow, kLUFSMomentary, kLUFSShortTerm, kLUFSIntegrated };
        static constexpr float kFloor = -60.f;
        static const int kMaxBlocks = 400;      // 10 ms blocks: 4 s of history

        // biquad (direct form I)
        struct Biquad {
            double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            double tick(double x)
            {
                double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
                x2 = x1; x1 = x; y2 = y1; y1 = y;
                return y;
            }
        };

        // per channel: K-weighting filters, energy of the current block, history of the blocks
        struct Channel {
            Biquad shelf, highpass;
            double raw = 0, weighted = 0;
            int count = 0;
            std::deque<std::pair<double, double>> blocks;  // mean squares (raw, K-weighted)
            float peak = kFloor, hold = kFloor, level = kFloor;
            int holdAge = 0;
        };

        std::vector<Channel> fChannels;
        int fVersion = -1;
        uint64_t fRead = 0;
        int fMode = kRMSFast;
        float fLoudness = kFloor;
        std::vector<float> fBuffer;
        std::vector<double> fGating;            // energy of the 400 ms gating blocks (sum of the channels)
        int fHop = 0;                           // 10 ms blocks since the last gating block
        int fNewBlocks = 0;                     // 10 ms blocks completed during this tick
        float fIntegrated = kFloor;
        float fMaxPeak = kFloor;

        bool isLUFS() const { return fMode == kLUFSMomentary || fMode == kLUFSShortTerm || fMode == kLUFSIntegrated; }

        // BS.1770 gated loudness of the stored blocks (gates compared as energies: no log per block)
        void integrate()
        {
            static const double absolute = std::pow(10.0, (-70.0 + 0.691) / 10.0);
            double sum = 0.0;
            int n = 0;
            for (double z : fGating) if (z > absolute) { sum += z; n++; }
            if (n == 0) { fIntegrated = kFloor; return; }
            double gate = std::max(absolute, 0.1 * sum / n);     // -10 LU
            sum = 0.0; n = 0;
            for (double z : fGating) if (z > gate) { sum += z; n++; }
            fIntegrated = n ? float(-0.691 + 10.0 * std::log10(sum / n)) : kFloor;
        }

        void reset()
        {
            fGating.clear();
            fHop = 0;
            fIntegrated = fMaxPeak = kFloor;
        }

        void initFilters(Channel& ch)
        {
            // BS.1770 K-weighting for any sample rate (pre-filter shelf + RLB high-pass)
            double fs = fSource.fSampleRate;
            double K = std::tan(M_PI * 1681.974450955533 / fs), Q = 0.7071752369554196;
            double Vh = std::pow(10.0, 3.999843853973347 / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
            double a0 = 1.0 + K / Q + K * K;
            ch.shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
            ch.shelf.b1 = 2.0 * (K * K - Vh) / a0;
            ch.shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
            ch.shelf.a1 = 2.0 * (K * K - 1.0) / a0;
            ch.shelf.a2 = (1.0 - K / Q + K * K) / a0;
            K = std::tan(M_PI * 38.13547087602444 / fs); Q = 0.5003270373238773;
            a0 = 1.0 + K / Q + K * K;
            ch.highpass.b0 = 1.0; ch.highpass.b1 = -2.0; ch.highpass.b2 = 1.0;
            ch.highpass.a1 = 2.0 * (K * K - 1.0) / a0;
            ch.highpass.a2 = (1.0 - K / Q + K * K) / a0;
        }

        // mean square of the last 'blocks' blocks (K-weighted or not)
        static double mean(const Channel& ch, int blocks, bool weighted)
        {
            int n = std::min(blocks, int(ch.blocks.size()));
            if (n == 0) return 0.0;
            double s = 0.0;
            for (int i = 0; i < n; i++) {
                const auto& b = ch.blocks[ch.blocks.size() - 1 - i];
                s += weighted ? b.second : b.first;
            }
            return s / n;
        }

        static constexpr float kBar = 8.f, kGap = 2.f;      // narrow bars, as in MiniMeters

        float y(float db, const QRectF& r) const
        {
            float v = std::min(0.f, std::max(kFloor, db));
            return r.top() + r.height() * (v / kFloor);
        }

        static const char* unit(int mode)
        {
            static const char* units[] = { "RMS", "RMS", "LUFS", "LUFS", "LUFS" };     // momentary / short-term / integrated: see the menu
            return units[mode];
        }

        // the module is as wide as the scale and the bars: the readout floats over
        // the next module, as in MiniMeters (MetersPanel::placeReadout)
        void fitWidth(int nch)
        {
            int scaleW = QFontMetrics(fqtMeterFont(9)).horizontalAdvance("50 -") + 4;
            setFixedWidth(scaleW + 2 + int(std::max(1, nch) * (kBar + kGap)) + 4);
        }

        QRectF barsRect() const
        {
            int scaleW = QFontMetrics(fqtMeterFont(9)).horizontalAdvance("50 -") + 4;
            return QRectF(scaleW + 2, 8, width() - scaleW - 6, height() - 16);
        }

        virtual void optionsChanged() { fitWidth(2); }

        virtual void fillMenu(QMenu* menu)
        {
            addChoice(menu, "Level", "mode", fMode,
                      { "RMS Fast (0.3 s)", "RMS Slow (1 s)", "LUFS Momentary (0.4 s)", "LUFS Short-Term (3 s)", "LUFS Integrated (gated)" },
                      { kRMSFast, kRMSSlow, kLUFSMomentary, kLUFSShortTerm, kLUFSIntegrated });
            menu->addSeparator();
            QObject::connect(menu->addAction("Reset Integrated and Max Peak"), &QAction::triggered, [this]() { reset(); });
        }

    public:

        // text and vertical position (module coordinates) of the readout
        QString readoutText() const
        {
            if (fChannels.empty()) return QString();
            float value = readoutValue();
            return (value <= kFloor ? QString("-inf") : QString::number(value, 'f', 1)) + unit(fMode);
        }

        float readoutY() const { return y(readoutValue(), barsRect()); }

        float readoutValue() const
        {
            float loudest = kFloor;
            for (const Channel& ch : fChannels) loudest = std::max(loudest, ch.level);
            if (fMode == kLUFSIntegrated) return fIntegrated;
            return isLUFS() ? fLoudness : loudest;
        }

        LevelsModule(const MeterSource& src) : MeterModule(src, "levels")
        {
            fMode = std::min(int(kLUFSIntegrated), std::max(0, option("mode", kRMSFast)));
            fitWidth(2);
            setCursor(Qt::PointingHandCursor);
        }

        virtual void mousePressEvent(QMouseEvent* e)
        {
            if (e->button() == Qt::LeftButton) nextMode();
        }

        // next level mode (click on the bars or on the readout)
        void nextMode()
        {
            fMode = (fMode + 1) % (kLUFSIntegrated + 1);
            setOption("mode", fMode);
            optionsChanged();
        }

        static QString modeName(int mode)
        {
            static const char* names[] = { "RMS Fast", "RMS Slow", "LUFS Momentary", "LUFS Short-Term", "LUFS Integrated" };
            return names[mode];
        }

        QString mode() const { return modeName(fMode); }
        QString next() const { return modeName((fMode + 1) % (kLUFSIntegrated + 1)); }

        virtual void tick()
        {
            uint64_t w = fSource.clock();
            int nch = 2;
            if (fVersion != fSource.fVersion) {
                // new routing: restart the analysis
                fVersion = fSource.fVersion;
                fChannels.assign(nch, Channel());
                for (Channel& ch : fChannels) initFilters(ch);
                fRead = w - std::min<uint64_t>(w, uint64_t(fSource.fSampleRate) / 2);
                reset();
            }
            // process the samples written since the last tick
            int n = int(std::min<uint64_t>(w - fRead, MeterRing::kSize / 2));
            uint64_t start = w - n;
            int blockSize = std::max(1, fSource.fSampleRate / 100);
            fBuffer.resize(std::max(n, 1));
            for (int c = 0; c < nch; c++) {
                Channel& ch = fChannels[c];
                fSource.readSide(c, start, fBuffer.data(), n);
                float pk = 0.f;
                for (int i = 0; i < n; i++) {
                    double x = fBuffer[i];
                    double k = ch.highpass.tick(ch.shelf.tick(x));
                    pk = std::max(pk, std::fabs(fBuffer[i]));
                    ch.raw += x * x;
                    ch.weighted += k * k;
                    if (++ch.count == blockSize) {
                        ch.blocks.emplace_back(ch.raw / blockSize, ch.weighted / blockSize);
                        if (int(ch.blocks.size()) > kMaxBlocks) ch.blocks.pop_front();
                        if (c == 0) fNewBlocks++;
                        ch.raw = ch.weighted = 0.0;
                        ch.count = 0;
                    }
                }
                float pdb = fqtDB(pk);
                fMaxPeak = std::max(fMaxPeak, pdb);
                ch.peak = std::max(pdb, ch.peak - 1.5f);      // fast attack, ~45 dB/s release
                if (pdb >= ch.hold || ++ch.holdAge > 45) { ch.hold = pdb; ch.holdAge = 0; }
            }
            fRead = w;

            // gating blocks of 400 ms every 100 ms (the channels advance together)
            int added = fNewBlocks;
            fNewBlocks = 0;
            bool changed = false;
            for (int b = 0; b < added; b++) {
                if (++fHop < 10) continue;
                fHop = 0;
                double z = 0.0;
                for (const Channel& ch : fChannels) z += mean(ch, 40, true);
                fGating.push_back(z);
                changed = true;
            }
            if (changed) integrate();
            setToolTip(QString("Click: next mode (%2)\nMax peak %1 dBFS (reset: right click)\nRight click: options").arg(fMaxPeak <= kFloor ? QString("-inf") : QString::number(fMaxPeak, 'f', 1)).arg(next()));

            // levels and readout
            static const int windows[] = { 30, 100, 40, 300, 40 };    // in blocks of 10 ms
            bool lufs = isLUFS();
            double sum = 0.0;
            for (Channel& ch : fChannels) {
                double ms = mean(ch, windows[fMode], lufs);
                sum += ms;
                ch.level = lufs ? float(-0.691 + 10.0 * std::log10(std::max(ms, 1e-12))) : float(10.0 * std::log10(std::max(ms, 1e-12)));
            }
            fLoudness = float(-0.691 + 10.0 * std::log10(std::max(sum, 1e-12)));
            update();
        }

        virtual void paintGL()
        {
            QPainter p(this);
            paintBackground(p);
            p.setFont(fqtMeterFont(9));
            QFontMetrics sfm(p.font());

            // scale on the left
            int scaleW = sfm.horizontalAdvance("50 -") + 4;
            QRectF r = barsRect();
            p.setPen(FQT_TEXT);
            float lastY = -100.f;
            for (int m : { 0, 6, 12, 24, 36, 50 }) {
                float yy = y(float(-m), r);
                if (yy - lastY < sfm.height()) continue;    // not enough room: skip the label
                p.drawText(QRectF(0, yy - 7, scaleW, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(m) + " -");
                lastY = yy;
            }

            // one bar per channel: level over the full width, peak as a thin bar, peak hold
            int nch = int(fChannels.size());
            if (nch == 0) return;
            float gap = kGap, bw = kBar;
            for (int c = 0; c < nch; c++) {
                const Channel& ch = fChannels[c];
                float x = r.left() + c * (bw + gap);
                float yl = y(ch.level, r), yp = y(ch.peak, r);
                p.fillRect(QRectF(x, r.top(), bw, r.height()), FQT_TRACK);
                p.fillRect(QRectF(x, yl, bw, r.bottom() - yl), FQT_ACCENT);
                p.fillRect(QRectF(x, yp, std::max(2.f, bw * 0.35f), r.bottom() - yp), FQT_ACCENT);
                p.fillRect(QRectF(x, y(ch.hold, r), bw, 2), ch.hold > -0.1f ? FQT_CLIP : FQT_SIGNAL);
            }

        }
};

//==============================STEREOMETER=====================================

/**
 * Stereometer of the left/right pair with the correlation meter (+1..-1) on
 * its right, as in MiniMeters. Display modes: Scaled (logarithmic radius,
 * quiet signals stay visible) and Linear (1:1); both rotated by 45 degrees:
 * mid up, side horizontal, with faint L, R and mono axes. Phosphor: the
 * samples fade with their age (the last 2048, in four bands).
 */
class StereoModule : public MeterModule
{

    private:

        enum { kScaled, kLinear };
        static const int kPoints = 2048;
        static const int kBands = 4;            // age bands of the phosphor
        std::vector<float> fL, fR;
        std::vector<QPointF> fDots;
        float fCorr = 0.f;
        int fMode = kScaled;
        int fPhosphor = 1;

        virtual void fillMenu(QMenu* menu)
        {
            addChoice(menu, "Display", "mode", fMode, { "Scaled", "Linear" }, { kScaled, kLinear });
            addChoice(menu, "Phosphor", "phosphor", fPhosphor, { "On", "Off" }, { 1, 0 });
        }

        // map a pair of samples to the unit square, center = silence
        QPointF map(float l, float r) const
        {
            float x = (r - l) / std::sqrt(2.f), y = (l + r) / std::sqrt(2.f);
            if (fMode == kScaled) {
                // radius on a 48 dB logarithmic scale
                float rad = std::sqrt(x * x + y * y);
                if (rad < 1e-9f) return QPointF(0, 0);
                float k = std::max(0.f, 1.f + fqtDB(rad) / 48.f) / rad;
                x *= k; y *= k;
            }
            return QPointF(x, y);
        }

    public:

        StereoModule(const MeterSource& src) : MeterModule(src, "stereo")
        {
            setMinimumWidth(90);
            fMode = option("mode", kScaled) == kLinear ? kLinear : kScaled;
            fPhosphor = option("phosphor", 1);
        }

        virtual std::vector<ChipGroup> chipGroups()
        {
            return { { "mode", &fMode, { "Scaled", "Linear" }, { kScaled, kLinear } } };
        }

        virtual void tick()
        {
            fL.resize(kPoints); fR.resize(kPoints);
            fSource.readPair(fL.data(), fR.data(), kPoints);
            double lr = 0, ll = 0, rr = 0;
            for (int i = 0; i < kPoints; i++) { lr += fL[i] * fR[i]; ll += fL[i] * fL[i]; rr += fR[i] * fR[i]; }
            float c = (ll > 1e-12 && rr > 1e-12) ? float(lr / std::sqrt(ll * rr)) : 0.f;
            fCorr = fCorr * 0.7f + c * 0.3f;
            update();
        }

        virtual void paintGL()
        {
            QPainter p(this);
            paintBackground(p);
            QRectF all = QRectF(rect()).adjusted(4, 8, -4, -8);
            p.setFont(fqtMeterFont(9));
            QFontMetrics fm(p.font());

            // correlation bar on the right, labels on its left
            float lw = fm.horizontalAdvance("+1") + 4;
            QRectF bar(all.right() - 10, all.top(), 10, all.height());
            auto ycorr = [&](float v) { return float(bar.top() + bar.height() * (1 - v) / 2); };
            p.fillRect(bar, FQT_TRACK);
            p.setPen(FQT_TEXT);
            const char* labels[] = { "+1", "0", "-1" };
            const float values[] = { 1, 0, -1 };
            for (int i = 0; i < 3; i++) {
                float yy = ycorr(values[i]);
                p.drawText(QRectF(bar.left() - lw - 4, yy - 7, lw, 14), Qt::AlignRight | Qt::AlignVCenter, labels[i]);
                p.fillRect(QRectF(bar.left(), yy - 0.5f, bar.width(), 1), FQT_SIGNAL);
            }
            p.fillRect(QRectF(bar.left(), ycorr(fCorr) - 2, bar.width(), 4), FQT_ACCENT);

            // stereometer: square centered in the remaining space
            QRectF area(all.left(), all.top(), bar.left() - lw - 8 - all.left(), all.height());
            float half = std::min(float(area.width()), float(area.height())) / 2;
            QPointF c = area.center();

            // axes: mono (vertical), L and R (diagonals)
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(FQT_TRACK, 1));
            float d = half * 0.7071f;
            p.drawLine(QPointF(c.x(), c.y() + half), QPointF(c.x(), c.y() - half));
            p.drawLine(QPointF(c.x() - d, c.y() - d), QPointF(c.x() + d, c.y() + d));
            p.drawLine(QPointF(c.x() + d, c.y() - d), QPointF(c.x() - d, c.y() + d));
            p.setFont(fqtMeterFont(8));
            p.setPen(FQT_TEXT_DIM);
            p.drawText(QRectF(c.x() - d - 14, c.y() - d - 14, 12, 12), Qt::AlignCenter, "L");
            p.drawText(QRectF(c.x() + d + 2, c.y() - d - 14, 12, 12), Qt::AlignCenter, "R");

            fDots.clear();
            for (int i = 0; i < int(fL.size()); i++) {
                QPointF m = map(fL[i], fR[i]);
                float x = std::max(-1.f, std::min(1.f, float(m.x()))), y = std::max(-1.f, std::min(1.f, float(m.y())));
                fDots.emplace_back(c.x() + x * half, c.y() - y * half);
            }
            // oldest band first; without phosphor only the newest half, at full intensity.
            // Opaque dots (accent mixed with the background) without antialiasing: Qt draws
            // opaque points in batches, transparent ones one by one
            int n = int(fDots.size()), band = n / kBands;
            QColor bg = FQT_METER_BG, fg = FQT_ACCENT;
            p.setRenderHint(QPainter::Antialiasing, false);
            for (int b = fPhosphor ? 0 : kBands / 2; b < kBands; b++) {
                float t = fPhosphor ? float(b + 1) / kBands : 1.f;
                t *= t;
                QColor col(int(bg.red() + (fg.red() - bg.red()) * t), int(bg.green() + (fg.green() - bg.green()) * t),
                           int(bg.blue() + (fg.blue() - bg.blue()) * t));
                p.setPen(fqtLinePen(col, 2.0));
                p.drawPoints(fDots.data() + b * band, (b == kBands - 1) ? n - b * band : band);
            }
            paintChips(p);
        }
};

//==============================SPECTRUM=====================================

/**
 * FFT spectrum (Hann window), logarithmic frequency axis from 20 Hz to 24 kHz
 * (or Nyquist), 4.5 dB/octave tilt around 1 kHz, display range -80..-20 dB
 * (tilted), smoothing, readout of the loudest frequency (level, frequency,
 * note). The FFT size (2048, 4096 or 8192 at 48 kHz) grows with the sample
 * rate, so that the frequency resolution does not depend on it. Two curves,
 * as in MiniMeters: L and R, or Mid and Side; the first in the accent color,
 * the second dimmed. The first curve has a peak hold curve (dotted, slow
 * decay). Spectrogram view: the louder of the
 * two curves, time scrolling to the left (newest on the right, as the
 * waveform), logarithmic frequency going up; analysed only while shown.
 * Mode chips (mouse over the module): Curves / Spectrogram, L/R / M/S.
 */
class SpectrumModule : public MeterModule
{

    private:

        enum { kLeftRight, kMidSide };
        static constexpr float kTilt = 4.5f;            // dB/octave
        static constexpr float kTop = -20.f, kBottom = -80.f;
        static constexpr float kMinFreq = 20.f;

        int fSizeOption = 4096;                         // FFT size at 48 kHz (menu)
        int fSize = 4096;                               // actual FFT size
        int fChannels = kLeftRight;
        int fSmoothing = 85;                            // release, in percent
        std::vector<float> fBuffer[2], fSmooth[2];      // one per curve
        std::vector<float> fWindow;
        std::vector<std::complex<float>> fFFT, fTwiddle;
        std::vector<int> fBitRev;
        float fPeakFreq = 0.f, fPeakDB = -200.f;
        enum { kCurves, kSpectrogram };
        int fView = kCurves;
        int fHoldOn = 1;
        int fHistory = 1;                               // spectrogram: ticks per row
        std::vector<float> fHold;                       // peak hold of the first curve
        QImage fSpec;                                   // spectrogram, one byte per pixel, columns in a ring
        int fSpecHead = 0, fSpecTicks = 0;              // newest column
        std::vector<QPointF> fCurve;

        void init()
        {
            int n = fSize;
            for (int c = 0; c < 2; c++) { fBuffer[c].assign(n, 0.f); fSmooth[c].assign(n / 2, -200.f); }
            fHold.assign(n / 2, -200.f);
            fWindow.resize(n); fFFT.resize(n); fTwiddle.resize(n / 2); fBitRev.resize(n);
            double sum = 0;
            for (int i = 0; i < n; i++) { fWindow[i] = float(0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1))); sum += fWindow[i]; }
            for (int i = 0; i < n; i++) fWindow[i] *= float(2.0 / sum);    // full-scale sine = 0 dB
            for (int i = 0; i < n / 2; i++) fTwiddle[i] = std::polar(1.f, float(-2 * M_PI * i / n));
            int bits = 0;
            while ((1 << bits) < n) bits++;
            for (int i = 0; i < n; i++) {
                int r = 0;
                for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
                fBitRev[i] = r;
            }
        }

        // in-place iterative radix-2 FFT
        void fft()
        {
            int n = fSize;
            for (int i = 0; i < n; i++) if (i < fBitRev[i]) std::swap(fFFT[i], fFFT[fBitRev[i]]);
            for (int len = 2; len <= n; len <<= 1) {
                int step = n / len;
                for (int i = 0; i < n; i += len) {
                    for (int j = 0; j < len / 2; j++) {
                        std::complex<float> u = fFFT[i + j], v = fFFT[i + j + len / 2] * fTwiddle[j * step];
                        fFFT[i + j] = u + v;
                        fFFT[i + j + len / 2] = u - v;
                    }
                }
            }
        }

        // FFT of one buffer into its smoothed spectrum (immediate attack, smooth release)
        void analyse(int c)
        {
            for (int i = 0; i < fSize; i++) fFFT[i] = std::complex<float>(fBuffer[c][i] * fWindow[i], 0.f);
            fft();
            float release = fSmoothing / 100.f;
            std::vector<float>& sm = fSmooth[c];
            for (int k = 1; k < fSize / 2; k++) {
                float db = fqtDB(std::abs(fFFT[k]));
                sm[k] = (db > sm[k]) ? db : sm[k] * release + db * (1 - release);
            }
        }

        virtual void fillMenu(QMenu* menu)
        {
            addChoice(menu, "Display", "channels", fChannels, { "Left / Right", "Mid / Side" }, { kLeftRight, kMidSide });
            addChoice(menu, "FFT Size (at 48 kHz)", "size", fSizeOption, { "2048", "4096", "8192" }, { 2048, 4096, 8192 });
            addChoice(menu, "Smoothing", "smoothing", fSmoothing, { "Low", "Medium", "High" }, { 50, 85, 95 });
            addChoice(menu, "View", "view", fView, { "Curves", "Spectrogram" }, { kCurves, kSpectrogram });
            addChoice(menu, "Peak Hold", "hold", fHoldOn, { "On", "Off" }, { 1, 0 });
            addChoice(menu, "Spectrogram Speed", "history", fHistory, { "Fast (60 columns / s)", "Slow (15 columns / s)" }, { 1, 4 });
        }

        // FFT size for the sample rate: x2 at 96 kHz, x4 at 192 kHz
        void updateSize()
        {
            int ratio = 1;
            while (fSource.fSampleRate >= 96000 * ratio * 0.9) ratio *= 2;
            fSize = std::min(fSizeOption * ratio, 32768);
            if (int(fWindow.size()) != fSize) init();
        }

        virtual void optionsChanged() { updateSize(); fSpec = QImage(); }

        float maxFreq() const { return std::min(fSource.fSampleRate / 2.f, 24000.f); }
        float binHz() const { return float(fSource.fSampleRate) / fSize; }
        float tilt(float f) const { return kTilt * std::log2(std::max(f, 1.f) / 1000.f); }
        // position on the logarithmic frequency axis, 0..1
        float warp(float f) const { return std::log(f / kMinFreq) / std::log(maxFreq() / kMinFreq); }
        float unwarp(float t) const { return kMinFreq * std::pow(maxFreq() / kMinFreq, t); }

        float xOf(float f, const QRectF& r) const { return float(r.left() + r.width() * warp(f)); }
        float fOf(float x, const QRectF& r) const { return unwarp(float((x - r.left()) / r.width())); }
        float yOf(float db, const QRectF& r) const { return float(r.top() + r.height() * (kTop - db) / (kTop - kBottom)); }

        // highest smoothed level of curve 'c' between two frequencies (interpolated inside a single bin)
        float levelBetween(int c, float f0, float f1) const { return levelBetween(fSmooth[c], f0, f1); }

        float levelBetween(const std::vector<float>& sm, float f0, float f1) const
        {
            int last = fSize / 2 - 1;
            int k0 = std::max(1, int(f0 / binHz())), k1 = std::min(last, std::max(k0, int(f1 / binHz())));
            if (k1 > k0) {
                float db = -200.f;
                for (int k = k0; k <= k1; k++) db = std::max(db, sm[k]);
                return db;
            }
            float t = f0 / binHz() - k0;
            return sm[k0] * (1 - t) + sm[std::min(k0 + 1, last)] * t;
        }

        // one point per pixel of a spectrum (tilted)
        void curvePoints(const std::vector<float>& sm, const QRectF& r)
        {
            fCurve.clear();
            for (int x = int(r.left()); x <= int(r.right()); x++) {
                float f0 = fOf(float(x), r), f1 = fOf(float(x + 1), r);
                float y = std::min(float(r.bottom()) + 2.f, yOf(levelBetween(sm, f0, f1) + tilt(f0), r));
                fCurve.emplace_back(x, std::max(float(r.top()), y));
            }
        }

        void paintCurve(QPainter& p, int c, const QRectF& r, const QColor& color)
        {
            curvePoints(fSmooth[c], r);
            p.setPen(fqtLinePen(color));
            p.drawPolyline(fCurve.data(), int(fCurve.size()));
        }

        // vertical position of a frequency in the spectrogram (logarithmic, low at the bottom)
        float yOfFreq(float f, const QRectF& r) const { return float(r.bottom() - r.height() * warp(f)); }

        // spectrogram column: level of the louder curve per pixel row, 0..255
        void spectrogramColumn(const QRectF& r)
        {
            int w = int(r.width()), h = int(r.height());
            if (w < 2 || h < 2) return;
            if (fSpec.width() != w || fSpec.height() != h) {
                fSpec = QImage(w, h, QImage::Format_Indexed8);
                fSpec.fill(0);
                fSpecHead = 0;
            }
            if (++fSpecTicks < fHistory) return;
            fSpecTicks = 0;
            fSpecHead = (fSpecHead + 1) % w;
            for (int y = 0; y < h; y++) {
                float f0 = unwarp(float(h - 1 - y) / h), f1 = unwarp(float(h - y) / h);
                float db = std::max(levelBetween(0, f0, f1), levelBetween(1, f0, f1)) + tilt(f0);
                float t = std::min(1.f, std::max(0.f, (db - kBottom) / (kTop - kBottom)));
                fSpec.scanLine(y)[fSpecHead] = uchar(255.f * t * t);    // quadratic: the noise floor stays dark
            }
        }

        void paintSpectrogram(QPainter& p, const QRectF& r)
        {
            if (fSpec.isNull()) return;
            // color table from the theme: background to accent
            QColor a = FQT_METER_BG, b = FQT_ACCENT;
            QVector<QRgb> table(256);
            for (int i = 0; i < 256; i++) {
                float t = i / 255.f;
                table[i] = qRgb(int(a.red() + (b.red() - a.red()) * t), int(a.green() + (b.green() - a.green()) * t), int(a.blue() + (b.blue() - a.blue()) * t));
            }
            fSpec.setColorTable(table);
            // oldest columns (after the head) on the left, newest on the right
            int w = fSpec.width(), h = fSpec.height(), old = w - fSpecHead - 1;
            if (old > 0) p.drawImage(QRectF(r.left(), r.top(), old, h), fSpec, QRectF(fSpecHead + 1, 0, old, h));
            p.drawImage(QRectF(r.left() + old, r.top(), fSpecHead + 1, h), fSpec, QRectF(0, 0, fSpecHead + 1, h));
        }

        static QString note(float f)
        {
            static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            float m = 69.f + 12.f * std::log2(f / 440.f);
            int k = int(std::lround(m));
            int cents = int(std::lround((m - k) * 100.f));
            return QString("%1%2 %3%4 Cents").arg(names[((k % 12) + 12) % 12]).arg(k / 12 - 1).arg(cents < 0 ? "- " : "+ ").arg(std::abs(cents));
        }

    public:

        SpectrumModule(const MeterSource& src) : MeterModule(src, "spectrum")
        {
            setMinimumWidth(120);
            fChannels = option("channels", kLeftRight) == kMidSide ? kMidSide : kLeftRight;
            fSmoothing = option("smoothing", 85);
            fSizeOption = option("size", 4096);
            fView = option("view", kCurves) == kSpectrogram ? kSpectrogram : kCurves;
            fHoldOn = option("hold", 1);
            fHistory = option("history", 1) == 4 ? 4 : 1;
            if (fSizeOption != 2048 && fSizeOption != 8192) fSizeOption = 4096;
            updateSize();
        }

        virtual std::vector<ChipGroup> chipGroups()
        {
            return { { "view", &fView, { "Curves", "Spectrogram" }, { kCurves, kSpectrogram } },
                     { "channels", &fChannels, { "L/R", "M/S" }, { kLeftRight, kMidSide } } };
        }

        virtual QPointF chipOrigin() const { return QPointF(8, 18); }   // below the frequency labels

        virtual void tick()
        {
            fSource.readPair(fBuffer[0].data(), fBuffer[1].data(), fSize);
            if (fChannels == kMidSide) {
                for (int i = 0; i < fSize; i++) {
                    float l = fBuffer[0][i], r = fBuffer[1][i];
                    fBuffer[0][i] = 0.5f * (l + r);
                    fBuffer[1][i] = 0.5f * (l - r);
                }
            }
            for (int c = 0; c < 2; c++) analyse(c);
            if (fHoldOn) {
                for (int k = 1; k < fSize / 2; k++) fHold[k] = std::max(fSmooth[0][k], fHold[k] - 0.1f);     // ~6 dB/s
            }
            if (fView == kSpectrogram) {
                spectrogramColumn(QRectF(rect()).adjusted(4, 4, -4, -2));
                update();
                return;     // no readout in the spectrogram
            }

            // loudest bin (tilted) over the curves, refined by parabolic interpolation
            int best = 1, bestCurve = 0;
            for (int c = 0; c < 2; c++) {
                int last = std::min(fSize / 2 - 1, int(maxFreq() / binHz()));
                for (int k = 1; k < last; k++) {
                    if (k * binHz() > kMinFreq && fSmooth[c][k] + tilt(k * binHz()) > fSmooth[bestCurve][best] + tilt(best * binHz())) {
                        best = k;
                        bestCurve = c;
                    }
                }
            }
            const std::vector<float>& sm = fSmooth[bestCurve];
            float a = sm[best - 1], b = sm[best], c = sm[std::min(best + 1, fSize / 2 - 1)];
            float d = (a - 2 * b + c) != 0.f ? 0.5f * (a - c) / (a - 2 * b + c) : 0.f;
            fPeakFreq = (best + d) * binHz();
            fPeakDB = b - 0.25f * (a - c) * d;
            update();
        }

        virtual void paintGL()
        {
            QPainter p(this);
            paintBackground(p);
            QRectF r = QRectF(rect()).adjusted(4, 4, -4, -2);
            if (fView == kSpectrogram) paintSpectrogram(p, r);
            p.setFont(fqtMeterFont(8, false));
            QFontMetricsF fm(p.font());

            // grid: 1..9 per decade, labels on the decades
            float lastLabel = -1e9f;
            for (float dec = 10.f; dec < maxFreq(); dec *= 10.f) {
                for (int k = 1; k < 10; k++) {
                    float f = dec * k;
                    if (f < kMinFreq || f > maxFreq()) continue;
                    p.setPen(QPen(FQT_TRACK, 1));
                    if (fView == kSpectrogram) {
                        // decades only (horizontal lines), labels on the left above their line
                        if (k != 1) continue;
                        float yy = yOfFreq(f, r);
                        p.drawLine(QPointF(r.left(), yy), QPointF(r.right(), yy));
                        if (k == 1 && dec >= 100.f && yy - 12 > r.top()) {
                            p.setPen(FQT_TEXT_DIM);
                            p.drawText(QPointF(r.left() + 3, yy - 3), dec >= 1000.f ? QString("%1kHz").arg(int(dec / 1000)) : QString("%1Hz").arg(int(dec)));
                        }
                        continue;
                    }
                    float x = xOf(f, r);
                    p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
                    if (k == 1 && dec >= 100.f) {
                        QString label = dec >= 1000.f ? QString("%1kHz").arg(int(dec / 1000)) : QString("%1Hz").arg(int(dec));
                        float w = fm.horizontalAdvance(label);
                        // labels on the right of their line, on the left near the window buttons,
                        // skipped when they would overlap the previous one
                        float lx = (x + 3 + w > width() - 34) ? x - 3 - w : x + 3;
                        if (lx < lastLabel + 6 || lx < r.left() || lx + w > width() - 34) continue;
                        p.setPen(FQT_TEXT_DIM);
                        p.drawText(QPointF(lx, r.top() + 10), label);
                        lastLabel = lx + w;
                    }
                }
            }

            // curves: the second one first (dimmed), the first one on top
            p.setRenderHint(QPainter::Antialiasing, true);
            if (fView == kCurves) {
                if (fHoldOn) {
                    // peak hold: thin dotted line in the accent color, no gray
                    curvePoints(fHold, r);
                    QPen pen = fqtLinePen(FQT_ACCENT, 1.0);
                    pen.setStyle(Qt::DotLine);
                    p.setPen(pen);
                    p.drawPolyline(fCurve.data(), int(fCurve.size()));
                }
                paintCurve(p, 1, r, FQT_TEXT_DIM);
                paintCurve(p, 0, r, FQT_ACCENT);
            }

            // legend: L R or M S in the colors of the curves, bottom left
            if (fView == kCurves) {
                p.setFont(fqtMeterFont(9));
                p.setPen(FQT_ACCENT);
                p.drawText(QPointF(r.left() + 4, r.bottom() - 4), fChannels == kMidSide ? "M" : "L");
                p.setPen(FQT_TEXT_DIM);
                p.drawText(QPointF(r.left() + 16, r.bottom() - 4), fChannels == kMidSide ? "S" : "R");
            }

            // readout of the loudest frequency (curves only)
            if (fView == kCurves && fPeakDB > -120.f && fPeakFreq > kMinFreq) {
                p.setFont(fqtReadoutFont());
                QFontMetrics pfm(p.font());
                QString txt = QString("%1dB | %2Hz | %3").arg(fPeakDB, 0, 'f', 2).arg(fPeakFreq, 0, 'f', 2).arg(note(fPeakFreq));
                if (pfm.horizontalAdvance(txt) + 6 > r.width()) txt = QString("%1dB | %2Hz").arg(fPeakDB, 0, 'f', 1).arg(fPeakFreq, 0, 'f', 0);
                if (pfm.horizontalAdvance(txt) + 6 > r.width()) txt = QString("%1Hz").arg(fPeakFreq, 0, 'f', 0);
                QRectF box = QRectF(pfm.boundingRect(txt)).adjusted(-3, -2, 3, 2);
                float px = std::max(float(r.left()), std::min(xOf(fPeakFreq, r) + 6, float(r.right() - box.width())));
                float py = std::max(float(r.top()) + 14.f, yOf(fPeakDB + tilt(fPeakFreq), r));
                box.moveTo(px, std::min(py, float(r.bottom() - box.height())));
                // keep clear of the window buttons in the top right corner, and of the mode chips
                if (box.right() > width() - 34 && box.top() < 32) box.moveTop(32);
                if (fHover && box.top() < 38) box.moveTop(38);
                p.fillRect(box, FQT_METER_BG);
                p.setPen(FQT_TEXT);
                p.drawText(box, Qt::AlignCenter, txt);
            }
            paintChips(p);
        }
};

//==============================FIT VIEW=====================================

/**
 * Shows a widget (the QTGUI controls) at its natural size, filling the view;
 * when the view is smaller than the natural size, the widget is scaled down
 * proportionally (no scrolling). The widget stays fully interactive through
 * a QGraphicsProxyWidget.
 */
class FitView : public QGraphicsView
{

    private:

        QGraphicsScene fScene;
        QWidget* fContent;
        QGraphicsProxyWidget* fProxy;

    protected:

        virtual void resizeEvent(QResizeEvent* e)
        {
            QGraphicsView::resizeEvent(e);
            QSize natural = fContent->sizeHint().expandedTo(fContent->minimumSizeHint());
            double w = viewport()->width(), h = viewport()->height();
            double s = std::min(1.0, std::min(w / std::max(1, natural.width()), h / std::max(1, natural.height())));
            fProxy->setScale(s);
            fProxy->resize(w / s, h / s);   // fills the view at any scale
            fScene.setSceneRect(0, 0, w, h);
        }

        virtual void drawBackground(QPainter* p, const QRectF& r)
        {
            p->fillRect(r, FQT_COLOR(0xf5f5f7, 0x1c1c1e));
        }

    public:

        FitView(QWidget* content) : fContent(content)
        {
            setScene(&fScene);
            setFrameShape(QFrame::NoFrame);
            setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            setAlignment(Qt::AlignLeft | Qt::AlignTop);
            setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
            content->setParent(nullptr);
            fProxy = fScene.addWidget(content);
            setMinimumSize(80, 60);
        }

        virtual QSize sizeHint() const { return fContent->sizeHint(); }
};

//==============================PANEL=====================================

/**
 * Readout of the levels module: a small box floating over the left edge of
 * the next module, at the height of the level (as in MiniMeters). It is also
 * the mode control of the levels: while the mouse is over the levels or the
 * box, it shows a chevron (clickable, as the mode chips of the other modules);
 * a click selects the next level mode.
 */
class MeterReadout : public QWidget
{

    private:

        QString fText;
        bool fHover = false;
        std::function<void()> fClicked;

        void fit()
        {
            QFontMetrics fm(fqtMeterFont(10));
            resize(fm.horizontalAdvance(fText) + 8 + (fHover ? 12 : 0), fm.height() + 2);
        }

    public:

        MeterReadout(QWidget* parent, std::function<void()> clicked) : QWidget(parent), fClicked(clicked)
        {
            setCursor(Qt::PointingHandCursor);
        }

        void setText(const QString& text)
        {
            if (text == fText) return;
            fText = text;
            fit();
            update();
        }

        void setHover(bool hover)
        {
            if (hover == fHover) return;
            fHover = hover;
            fit();
            update();
        }

        virtual void mousePressEvent(QMouseEvent* e)
        {
            if (e->button() == Qt::LeftButton) fClicked();
        }

        virtual void paintEvent(QPaintEvent*)
        {
            QPainter p(this);
            p.fillRect(rect(), FQT_ACCENT);
            p.setFont(fqtMeterFont(10));
            p.setPen(FQT_METER_BG);
            QRect text = rect().adjusted(0, 0, fHover ? -12 : 0, 0);
            p.drawText(text, Qt::AlignCenter, fText);
            if (fHover) p.drawText(QRect(text.right(), 0, 12, height()), Qt::AlignCenter, QString::fromUtf8("\u25B8"));
        }
};

/**
 * Divider between the strip and the controls: a thin bar of the window color
 * with a small grip in the middle (it must be painted to receive the mouse).
 */
class MeterSplitterHandle : public QSplitterHandle
{

    public:

        MeterSplitterHandle(Qt::Orientation o, QSplitter* parent) : QSplitterHandle(o, parent)
        {
            setCursor(Qt::SizeVerCursor);
            setToolTip("Drag to resize the meters, up to close them");
        }

        virtual void paintEvent(QPaintEvent*)
        {
            QPainter p(this);
            p.fillRect(rect(), FQT_COLOR(0xf5f5f7, 0x1c1c1e));
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(FQT_BORDER);
            p.drawRoundedRect(QRectF(width() / 2.0 - 18, height() / 2.0 - 1.5, 36, 3), 1.5, 1.5);
        }
};

class MeterSplitter : public QSplitter
{

    protected:

        virtual QSplitterHandle* createHandle() { return new MeterSplitterHandle(orientation(), this); }

    public:

        MeterSplitter() : QSplitter(Qt::Vertical) {}
};

/**
 * Routing matrix (popup of the routing button): one row per output and per
 * input of the DSP, two columns L and R; a click on a square sends the
 * channel to that side or removes it. Several channels on a side are
 * averaged (sum / number of channels).
 */
class MeterRouting : public QWidget
{

    private:

        MeterSource& fSource;
        std::function<void()> fChanged;
        struct Cell { QRectF rect; int side, outputs, channel; };
        std::vector<Cell> fCells;
        QRectF fReset;
        static constexpr float kRow = 18.f, kSquare = 12.f;

        int rows() const { return std::max(fSource.ring(1).channels(), fSource.ring(0).channels()); }

    public:

        MeterRouting(MeterSource& source, std::function<void()> changed, QWidget* parent)
            : QWidget(parent, Qt::Popup), fSource(source), fChanged(changed)
        {
            // two blocks side by side: OUT and IN, each with columns L and R
            int blocks = (fSource.ring(0).channels() > 0 ? 1 : 0) + (fSource.ring(1).channels() > 0 ? 1 : 0);
            resize(16 + blocks * 92, int(46 + rows() * kRow + 28));
        }

        virtual void mousePressEvent(QMouseEvent* e)
        {
            QPointF pos = e->position();
            if (!rect().contains(pos.toPoint())) { close(); return; }
            for (const Cell& c : fCells) {
                if (c.rect.adjusted(-3, -3, 3, 3).contains(pos)) {
                    fSource.toggle(c.side, c.outputs, c.channel);
                    fChanged();
                    update();
                    return;
                }
            }
            if (fReset.contains(pos)) {
                fSource.setDefault();
                fChanged();
                update();
            }
        }

        virtual void paintEvent(QPaintEvent*)
        {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(FQT_BORDER, 1));
            p.setBrush(FQT_METER_BG);
            p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
            p.setFont(fqtMeterFont(9));
            fCells.clear();
            float x0 = 12;
            for (int k = 1; k >= 0; k--) {          // outputs first, then inputs
                int n = fSource.ring(k).channels();
                if (n == 0) continue;
                p.setPen(FQT_TEXT_DIM);
                p.drawText(QRectF(x0, 8, 40, 14), Qt::AlignLeft | Qt::AlignVCenter, k ? "OUT" : "IN");
                p.drawText(QRectF(x0 + 38, 24, kSquare, 14), Qt::AlignCenter, "L");
                p.drawText(QRectF(x0 + 60, 24, kSquare, 14), Qt::AlignCenter, "R");
                for (int c = 0; c < n; c++) {
                    float y = 42 + c * kRow;
                    p.setPen(FQT_TEXT);
                    p.drawText(QRectF(x0, y - 7, 30, 14), Qt::AlignLeft | Qt::AlignVCenter, QString::number(c + 1));
                    for (int side = 0; side < 2; side++) {
                        QRectF sq(x0 + 38 + side * 22, y - kSquare / 2, kSquare, kSquare);
                        bool on = fSource.fRoute[side][k][c];
                        p.setPen(QPen(on ? FQT_ACCENT : FQT_BORDER, 1));
                        p.setBrush(on ? FQT_ACCENT : Qt::transparent);
                        p.drawRoundedRect(sq, 2, 2);
                        fCells.push_back({ sq, side, k, c });
                    }
                }
                x0 += 92;
            }
            // reset to the default routing
            p.setPen(FQT_TEXT_DIM);
            fReset = QRectF(12, height() - 22, width() - 24, 16);
            p.drawText(fReset, Qt::AlignLeft | Qt::AlignVCenter, "Reset (1 > L, 2 > R)");
        }
};

/**
 * Status bar below the controls, always shown (also with the strip closed),
 * updated once per second, repainted only when what it shows has changed
 * (a repaint recomposes the whole window, which holds OpenGL widgets):
 * - one small bar per CPU core (load of the whole system, any number of cores),
 *   the total and the share of the application, both in % of the whole Mac
 *   (the tooltip also gives the application in % of one core);
 * - the DSP load: time spent in the DSP compute / duration of the block
 *   (average and worst block of the period), and the number of "late" blocks,
 *   computed in more time than their duration (likely dropouts);
 * - the block size measured in the audio callback (CoreAudio does not show
 *   it), its duration, the sample rate;
 * - the default output device and its output latency (buffer + device
 *   latency + safety offset).
 * Cost: a few system calls per update, nothing on the audio thread except
 * two clock reads per block (MeterTiming).
 */
class MeterStatusBar : public QWidget
{

    private:

        MeterTiming& fTiming;
        int fSampleRate;
        QTimer fTimer;
        int fUpdates = 0;

        // system and application CPU
        std::vector<uint64_t> fBusy, fTotal;    // ticks per core at the last update
        std::vector<float> fCores;              // load per core, 0..1
        float fSystem = 0.f, fApp = 0.f;
        double fAppTime = 0.0;
        std::chrono::steady_clock::time_point fWall;

        // audio
        uint64_t fBusyNs = 0, fSamples = 0;
        float fLoad = 0.f, fWorstLoad = 0.f;
        int fBlock = 0, fBlockMin = 0, fBlockMax = 0;
        uint64_t fLate = 0;

        // device
        QString fDevice;
        float fLatencyMs = 0.f;
        int fDeviceBuffer = 0;

        void readCPU()
        {
#ifdef __APPLE__
            natural_t count = 0;
            processor_info_array_t info = nullptr;
            mach_msg_type_number_t size = 0;
            if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &count, &info, &size) == KERN_SUCCESS) {
                processor_cpu_load_info_t load = (processor_cpu_load_info_t)info;
                fBusy.resize(count, 0); fTotal.resize(count, 0); fCores.resize(count, 0.f);
                float sum = 0.f;
                for (natural_t c = 0; c < count; c++) {
                    const unsigned int* t = load[c].cpu_ticks;
                    uint64_t busy = uint64_t(t[CPU_STATE_USER]) + t[CPU_STATE_SYSTEM] + t[CPU_STATE_NICE];
                    uint64_t total = busy + t[CPU_STATE_IDLE];
                    uint64_t db = busy - fBusy[c], dt = total - fTotal[c];
                    if (fTotal[c] > 0 && dt > 0) fCores[c] = float(db) / float(dt);
                    fBusy[c] = busy; fTotal[c] = total;
                    sum += fCores[c];
                }
                fSystem = count ? sum / count : 0.f;
                vm_deallocate(mach_task_self(), vm_address_t(info), vm_size_t(size) * sizeof(integer_t));
            }
            struct rusage ru;
            getrusage(RUSAGE_SELF, &ru);
            double t = ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + 1e-6 * (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
            auto now = std::chrono::steady_clock::now();
            double wall = std::chrono::duration<double>(now - fWall).count();
            if (fAppTime > 0.0 && wall > 0.0) fApp = float((t - fAppTime) / wall);
            fAppTime = t;
            fWall = now;
#endif
        }

        void readAudio()
        {
            uint64_t busy = fTiming.fBusyNs.load(std::memory_order_relaxed);
            uint64_t samples = fTiming.fSamples.load(std::memory_order_relaxed);
            if (samples > fSamples && fSampleRate > 0) {
                fLoad = float(double(busy - fBusyNs) * fSampleRate / (double(samples - fSamples) * 1e9));
            }
            fBusyNs = busy; fSamples = samples;
            fWorstLoad = float(double(fTiming.fWorst.exchange(0, std::memory_order_relaxed)) * fSampleRate / (1024.0 * 1e9));
            fBlock = fTiming.fBlock.load(std::memory_order_relaxed);
            fBlockMin = fTiming.fBlockMin.exchange(INT_MAX, std::memory_order_relaxed);
            fBlockMax = fTiming.fBlockMax.exchange(0, std::memory_order_relaxed);
            if (fBlockMin == INT_MAX) fBlockMin = fBlockMax = fBlock;
            fLate = fTiming.fLate.load(std::memory_order_relaxed);
        }

#ifdef __APPLE__
        template <typename T>
        static bool property(AudioObjectID id, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, T& value)
        {
            AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
            UInt32 size = sizeof(T);
            return AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, &value) == noErr;
        }
#endif

        void readDevice()
        {
#ifdef __APPLE__
            AudioObjectID dev = 0;
            if (!property(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, dev) || !dev) return;
            CFStringRef name = nullptr;
            if (property(dev, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, name) && name) {
                char buf[256] = { 0 };
                if (CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8)) fDevice = QString::fromUtf8(buf);
                CFRelease(name);
            }
            UInt32 buffer = 0, latency = 0, safety = 0;
            property(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, buffer);
            property(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, latency);
            property(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, safety);
            fDeviceBuffer = int(buffer);
            fLatencyMs = fSampleRate > 0 ? 1000.f * float(buffer + latency + safety) / fSampleRate : 0.f;
#endif
        }

        void refresh()
        {
            readCPU();
            readAudio();
            if (fUpdates++ % 2 == 0) readDevice();      // every 2 s: the default device may change
            QStringList tip;
            QStringList cores;
            for (int c = 0; c < int(fCores.size()); c++) cores << QString("%1").arg(int(std::lround(fCores[c] * 100)));
            tip << QString("CPU per core (%): %1").arg(cores.join(" "))
                << QString("cpu: whole Mac %1%, app: share of the whole Mac %2% (= %3% of one core, as in Activity Monitor)")
                       .arg(int(std::lround(fSystem * 100))).arg(int(std::lround(fApp * 100 / std::max<size_t>(1, fCores.size())))).arg(int(std::lround(fApp * 100)))
                << QString("DSP load: time in the DSP compute / duration of the block, average and worst block")
                << QString("Late blocks: computed in more time than their duration (%1 since the start)").arg(fLate)
                << QString("Block size from the audio callback: %1 (min %2, max %3), device buffer %4")
                       .arg(fBlock).arg(fBlockMin).arg(fBlockMax).arg(fDeviceBuffer)
                << QString("Output latency: device buffer + latency + safety offset");
            if (tip.join("\n") != toolTip()) setToolTip(tip.join("\n"));

            // what is shown: bar heights and texts; repaint only when they change
            std::vector<int> bars;
            for (float load : fCores) bars.push_back(int(std::lround(10.f * std::min(1.f, std::max(0.f, load)))));
            auto pct = [](float v) { return QString("%1%").arg(int(std::lround(v * 100))); };
            float blockMs = fSampleRate > 0 ? 1000.f * fBlock / fSampleRate : 0.f;
            QString block = (fBlockMin != fBlockMax) ? QString("%1-%2").arg(fBlockMin).arg(fBlockMax) : QString::number(fBlock);
            QStringList items;
            items << QString("cpu %1").arg(pct(fSystem))
                  << QString("app %1").arg(pct(fCores.empty() ? fApp : fApp / fCores.size()))
                  << QString("dsp %1 (max %2)").arg(pct(fLoad)).arg(pct(fWorstLoad))
                  << QString("block %1 (%2 ms)").arg(block).arg(blockMs, 0, 'f', 1)
                  << QString("%1 kHz").arg(fSampleRate / 1000.0, 0, 'g', 4)
                  << QString("late %1").arg(fLate);
            QString right = fDevice.isEmpty() ? QString() : QString("%1 \u00b7 out %2 ms").arg(fDevice).arg(fLatencyMs, 0, 'f', 1);
            if (bars != fBars || items != fItems || right != fRight) {
                fBars = bars; fItems = items; fRight = right;
                update();
            }
        }

        std::vector<int> fBars;     // shown: bar heights (0..10)
        QStringList fItems;         // shown: texts
        QString fRight;

    public:

        static const int kHeight = 20;

        MeterStatusBar(MeterTiming& timing, int sample_rate) : fTiming(timing), fSampleRate(sample_rate)
        {
            setFixedHeight(kHeight);
            fWall = std::chrono::steady_clock::now();
            QObject::connect(&fTimer, &QTimer::timeout, [this]() { refresh(); });
            fTimer.start(1000);
            refresh();
        }

        virtual void paintEvent(QPaintEvent*)
        {
            QPainter p(this);
            p.fillRect(rect(), FQT_COLOR(0xf5f5f7, 0x1c1c1e));
            p.fillRect(QRectF(0, 0, width(), 1), FQT_BORDER);
            p.setFont(fqtMeterFont(9, false));
            QFontMetrics fm(p.font());
            float x = 8, cy = height() / 2.f + 0.5f;

            // one bar per core
            float bh = 10.f;
            for (int h : fBars) {
                QRectF bar(x, cy - bh / 2, 3, bh);
                p.fillRect(bar, FQT_TRACK);
                p.fillRect(QRectF(bar.left(), bar.bottom() - h, 3, h), FQT_SIGNAL);
                x += 4;
            }
            x += 6;

            const QStringList& items = fItems;
            const QString& right = fRight;
            int rightW = right.isEmpty() ? 0 : fm.horizontalAdvance(right) + 16;
            QString sep = QString::fromUtf8("  ·  ");
            for (int i = 0; i < items.size(); i++) {
                QString t = (i ? sep : QString()) + items[i];
                int w = fm.horizontalAdvance(t);
                if (x + w > width() - 8 - rightW && i > 0) { rightW = 0; if (x + w > width() - 8) break; }
                // late blocks in the clip color, as a warning
                p.setPen(i == items.size() - 1 && fLate > 0 ? FQT_CLIP : FQT_TEXT_DIM);
                p.drawText(QPointF(x, cy + fm.ascent() / 2.f - 1), t);
                x += w;
            }
            if (rightW > 0 && x + rightW <= width() - 8) {
                p.setPen(FQT_TEXT_DIM);
                p.drawText(QRectF(0, 0, width() - 8, height()), Qt::AlignRight | Qt::AlignVCenter, right);
            }
        }
};

/**
 * Columns of the strip: waveform + scope | levels + stereometer | spectrum,
 * in a horizontal splitter. Dragging a border widens the module on its left
 * and narrows the one on its right; a double click on a border restores
 * three equal thirds. The border looks like the 1 px separators, but can be
 * grabbed on 5 px. The proportions are kept when the window is resized and
 * saved per application (MetersPanel).
 */
class MeterColumnHandle : public QSplitterHandle
{

    private:

        std::function<void()> fReset;

    public:

        MeterColumnHandle(Qt::Orientation o, QSplitter* parent, std::function<void()> reset)
            : QSplitterHandle(o, parent), fReset(reset)
        {
            setCursor(Qt::SplitHCursor);
            setToolTip("Drag to resize the modules, double click: equal thirds");
        }

        virtual void mouseDoubleClickEvent(QMouseEvent*) { fReset(); }

        virtual void paintEvent(QPaintEvent*)
        {
            QPainter p(this);
            p.fillRect(rect(), FQT_METER_BG);
            p.fillRect(QRectF(width() / 2, 0, 1, height()), FQT_BORDER);
        }
};

class MeterColumns : public QSplitter
{

    private:

        std::vector<double> fFractions { 1.0 / 3, 1.0 / 3, 1.0 / 3 };
        std::function<void()> fChanged;

    protected:

        virtual QSplitterHandle* createHandle()
        {
            return new MeterColumnHandle(orientation(), this, [this]() { setFractions({ 1.0 / 3, 1.0 / 3, 1.0 / 3 }); fChanged(); });
        }

        virtual void resizeEvent(QResizeEvent* e)
        {
            QSplitter::resizeEvent(e);
            apply();
        }

    public:

        MeterColumns(std::function<void()> changed) : QSplitter(Qt::Horizontal), fChanged(changed)
        {
            setHandleWidth(5);
            setChildrenCollapsible(false);
            QObject::connect(this, &QSplitter::splitterMoved, [this]() {
                // new proportions from the sizes chosen by the user
                QList<int> sz = sizes();
                double total = 0;
                for (int v : sz) total += v;
                if (total <= 0 || sz.size() != int(fFractions.size())) return;
                for (int i = 0; i < sz.size(); i++) fFractions[i] = sz[i] / total;
                fChanged();
            });
        }

        const std::vector<double>& fractions() const { return fFractions; }

        void setFractions(const std::vector<double>& f)
        {
            double total = 0;
            for (double v : f) total += v;
            if (f.size() != fFractions.size() || total <= 0) return;
            for (size_t i = 0; i < f.size(); i++) fFractions[i] = f[i] / total;
            apply();
        }

        void apply()
        {
            int total = width() - handleWidth() * (count() - 1);
            if (total <= 0 || count() != int(fFractions.size())) return;
            QList<int> sz;
            for (double v : fFractions) sz << int(std::lround(v * total));
            setSizes(sz);
        }
};

/**
 * The strip: the modules side by side (MiniMeters horizontal layout) in
 * three resizable columns (MeterColumns), separated by 1 px lines, with a round button in the top left corner (same
 * style as the window buttons on the right, routing in its tooltip) that
 * opens the routing matrix. It sits in a vertical splitter above the controls: drag the divider
 * to resize it, up to close it, down to reopen it (height saved per application).
 *
 * Usage (see ca-qt-meters.cpp):
 *      MetersPanel* meters = MetersPanel::attach(window, meterDSP, sample_rate);   // before QTGUI::run()
 *      interface->run();
 *      meters->fitWindow();                                                        // after QTGUI::run()
 */
class MetersPanel : public QWidget
{

    public:

        static const int kHeight = 170;     // default height
        static const int kMinHeight = 80;   // below this, dragging the divider closes the strip
        static const int kMaxHeight = 600;

    private:

        MeterSource fSource;
        std::vector<MeterModule*> fModules;
        QTimer fTimer;
        QToolButton* fRoutingButton;
        LevelsModule* fLevels = nullptr;
        MeterReadout* fReadout = nullptr;
        QMainWindow* fWindow = nullptr;
        QSplitter* fSplitter = nullptr;
        MeterColumns* fColumns = nullptr;
        MeterStatusBar* fStatus = nullptr;

        static QSettings settings() { return QSettings("Faust", QCoreApplication::applicationName()); }

        static QToolButton* roundButton(QWidget* parent, int width, const QString& tip)
        {
            QToolButton* b = new QToolButton(parent);
            b->setObjectName("themeButton");    // round style of the window buttons (Grey.qss / GreyDark.qss)
            b->setFixedSize(width, 22);
            b->setCursor(Qt::PointingHandCursor);
            b->setToolTip(tip);
            return b;
        }

        // routing button text ("O1 | O2") and saved routing
        // routing button: an icon, the routing in its tooltip
        void routingChanged()
        {
            fRoutingButton->setToolTip(QString("Routing: L = %1, R = %2\nClick: inputs and outputs shown as L and R")
                                       .arg(fSource.label(MeterSource::kLeft)).arg(fSource.label(MeterSource::kRight)));
            settings().setValue("meters/route_l", fSource.save(MeterSource::kLeft));
            settings().setValue("meters/route_r", fSource.save(MeterSource::kRight));
        }

        void routingMatrix()
        {
            MeterRouting* m = new MeterRouting(fSource, [this]() { routingChanged(); }, this);
            m->setAttribute(Qt::WA_DeleteOnClose);
            m->move(fRoutingButton->mapToGlobal(QPoint(0, fRoutingButton->height() + 4)));
            m->show();
        }

        // the readout of the levels module, over the left edge of the next module
        void placeReadout()
        {
            QString text = fLevels->readoutText();
            fReadout->setVisible(!text.isEmpty());
            fReadout->setText(text);
            fReadout->setHover(fLevels->underMouse() || fReadout->underMouse());
            fReadout->setToolTip(QString("%1\nClick: %2").arg(fLevels->mode()).arg(fLevels->next()));
            QRect g(fLevels->mapTo(this, QPoint(0, 0)), fLevels->size());
            int y = g.top() + int(fLevels->readoutY()) - fReadout->height() / 2;
            y = std::max(g.top() + 2, std::min(y, g.bottom() - fReadout->height() - 2));
            fReadout->move(g.right() + 1, y);
            fReadout->raise();
        }

        // proportions of the columns, saved as "0.333,0.333,0.333"
        void columnsChanged()
        {
            QStringList l;
            for (double v : fColumns->fractions()) l << QString::number(v, 'f', 4);
            settings().setValue("meters/columns", l.join(","));
        }

        bool isOpen() const { return fSplitter && fSplitter->sizes().value(0) > 0; }

        void saveState()
        {
            int h = fSplitter->sizes().value(0);
            // closed strip: hide the modules, so that nothing is drawn or composited
            for (MeterModule* m : fModules) m->setVisible(h > 0);
            settings().setValue("meters/height", h);
        }

    public:

        MetersPanel(MeterDSP* dsp, int sample_rate, QWidget* parent = nullptr) : QWidget(parent)
        {
            fSource.fDSP = dsp;
            fSource.fSampleRate = sample_rate > 0 ? sample_rate : 48000;
            fSource.setDefault();
            QSettings st = settings();
            if (st.contains("meters/route_l")) {
                fSource.load(MeterSource::kLeft, st.value("meters/route_l").toString());
                fSource.load(MeterSource::kRight, st.value("meters/route_r").toString());
            }
            setMinimumHeight(kMinHeight);
            setMaximumHeight(kMaxHeight);

            QHBoxLayout* row = new QHBoxLayout(this);
            row->setContentsMargins(0, 0, 0, 1);    // bottom line
            row->setSpacing(0);

            // order of the MiniMeters horizontal layout (the oscilloscope continues the waveform, in the
            // same module), in three resizable columns; the levels keep their width in the middle one
            fColumns = new MeterColumns([this]() { columnsChanged(); });
            row->addWidget(fColumns);
            WaveScopeModule* wave = new WaveScopeModule(fSource);
            fLevels = new LevelsModule(fSource);
            StereoModule* stereo = new StereoModule(fSource);
            SpectrumModule* spectrum = new SpectrumModule(fSource);
            fModules = { wave, fLevels, stereo, spectrum };
            QWidget* middle = new QWidget();
            QHBoxLayout* pair = new QHBoxLayout(middle);
            pair->setContentsMargins(0, 0, 0, 0);
            pair->setSpacing(1);                    // separator: the panel background shows through
            pair->addWidget(fLevels);
            pair->addWidget(stereo, 1);
            fColumns->addWidget(wave);
            fColumns->addWidget(middle);
            fColumns->addWidget(spectrum);
            QStringList saved = settings().value("meters/columns").toString().split(",", Qt::SkipEmptyParts);
            if (saved.size() == 3) fColumns->setFractions({ saved[0].toDouble(), saved[1].toDouble(), saved[2].toDouble() });

            fReadout = new MeterReadout(this, [this]() { fLevels->nextMode(); });
            fRoutingButton = roundButton(this, 22, QString());
            fRoutingButton->setText(QString::fromUtf8("\u21C4"));     // arrows: inputs/outputs to L and R
            fRoutingButton->move(6, 4);
            QObject::connect(fRoutingButton, &QToolButton::clicked, [this]() { routingMatrix(); });
            routingChanged();

            QObject::connect(&fTimer, &QTimer::timeout, [this]() {
                if (!isVisible() || (fSplitter && !isOpen())) return;     // closed strip: no analysis
                for (MeterModule* m : fModules) m->tick();
                placeReadout();
            });
            fTimer.start(kMeterFrameMs);
        }

        /**
         * Put the strip above the central widget of the QTGUI window and add the toggle button
         */
        static MetersPanel* attach(QWidget* window, MeterDSP* dsp, int sample_rate)
        {
            MetersPanel* panel = new MetersPanel(dsp, sample_rate);
            panel->fWindow = qobject_cast<QMainWindow*>(window);
            if (!panel->fWindow) return panel;

            // the controls: QTGUI, taken out of its scroll area and shown in a FitView
            QWidget* controls = panel->fWindow->takeCentralWidget();
            QScrollArea* scroll = qobject_cast<QScrollArea*>(controls);
            if (scroll && scroll->widget()) {
                QWidget* gui = scroll->takeWidget();
                // extra height goes below the controls instead of stretching their groups
                if (QBoxLayout* box = qobject_cast<QBoxLayout*>(gui->layout())) box->addStretch(1);
                controls = new FitView(gui);
                scroll->deleteLater();
            }
            panel->fSplitter = new MeterSplitter();
            panel->fSplitter->setHandleWidth(7);
            panel->fSplitter->addWidget(panel);
            panel->fSplitter->addWidget(controls);
            panel->fSplitter->setCollapsible(0, true);
            panel->fSplitter->setCollapsible(1, false);
            // same stretch: when the window is resized, strip and controls keep their proportions
            // the status bar below everything
            QWidget* central = new QWidget();
            QVBoxLayout* column = new QVBoxLayout(central);
            column->setContentsMargins(0, 0, 0, 0);
            column->setSpacing(0);
            column->addWidget(panel->fSplitter, 1);
            panel->fStatus = new MeterStatusBar(dsp->timing(), panel->fSource.fSampleRate);
            column->addWidget(panel->fStatus);
            panel->fWindow->setCentralWidget(central);
            QObject::connect(panel->fSplitter, &QSplitter::splitterMoved, [panel]() { panel->saveState(); });

            return panel;
        }

        /**
         * QTGUI::run() fits the window to the controls only: make room for the strip
         */
        void fitWindow()
        {
            if (!fWindow) return;
            int h = settings().value("meters/height", kHeight).toInt();
            QScreen* screen = QGuiApplication::primaryScreen();
            int maxH = screen ? screen->availableGeometry().height() - 40 : 900;
            int controls = fWindow->height();      // the window was fitted to the controls by QTGUI::run()
            int status = fStatus ? fStatus->height() : 0;
            fWindow->resize(std::max(fWindow->width(), 760), std::min(controls + h + fSplitter->handleWidth() + status, maxH));
            fSplitter->setSizes({ h, controls });   // rescaled by the splitter if the screen is too small
            saveState();
        }

        virtual void paintEvent(QPaintEvent*)
        {
            // separators between the modules and bottom line
            QPainter p(this);
            p.fillRect(rect(), FQT_BORDER);
        }
};

#endif
/**************************  END  QTMeters.h **************************/
