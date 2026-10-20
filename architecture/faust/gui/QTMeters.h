/************************** BEGIN QTMeters.h **************************
 Analysis strip for the Faust QT applications (Luca Spanedda), modelled on
 MiniMeters: waveform, levels/loudness, stereometer, oscilloscope and spectrum,
 above the controls of the QTGUI window.

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
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "faust/dsp/dsp.h"

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
 * DSP decorator feeding the rings. The wrapped DSP is not owned: it is
 * deleted by the architecture file as usual.
 */
class MeterDSP : public decorator_dsp
{

    private:

        MeterRing fIn;
        MeterRing fOut;

    public:

        MeterDSP(::dsp* dsp) : decorator_dsp(dsp), fIn(dsp->getNumInputs()), fOut(dsp->getNumOutputs()) {}
        virtual ~MeterDSP() { fDSP = nullptr; }

        MeterRing& inputs() { return fIn; }
        MeterRing& outputs() { return fOut; }

        virtual void compute(int count, FAUSTFLOAT** inputs, FAUSTFLOAT** outputs)
        {
            fIn.push(inputs, count);        // before compute: inputs may be reused in place
            fDSP->compute(count, inputs, outputs);
            fOut.push(outputs, count);
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
            setToolTip("Right click: options");
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

//==============================WAVEFORM=====================================

/**
 * Scrolling waveform of L and R (or Mid and Side) in two lanes, as in
 * MiniMeters: each column is the min/max of 10 ms by default, the newest on
 * the right.
 */
class WaveformModule : public MeterModule
{

    private:

        static const int kColumns = 2000;           // 20 s of history at 10 ms
        struct Trace { std::deque<std::pair<float, float>> cols; float lo = 0.f, hi = 0.f; };
        Trace fTraces[2];
        int fCount = 0;
        int fVersion = -1;
        uint64_t fRead = 0;
        int fSpeed = 1;
        int fMidSide = 0;
        std::vector<float> fIn[2];
        std::vector<QLineF> fLines;

        virtual void fillMenu(QMenu* menu)
        {
            addChoice(menu, "Display", "midside", fMidSide, { "Left / Right", "Mid / Side" }, { 0, 1 });
            addChoice(menu, "Speed", "speed", fSpeed, { "Slow (40 ms / pixel)", "Normal (10 ms / pixel)", "Fast (2.5 ms / pixel)" }, { 0, 1, 2 });
        }

        virtual void optionsChanged() { fVersion = -1; }   // restart the history

        void paintTrace(QPainter& p, const Trace& t, const QRectF& all, float cy, float amp)
        {
            int n = std::min(int(t.cols.size()), int(all.width()));
            fLines.clear();
            for (int i = 0; i < n; i++) {
                const auto& c = t.cols[t.cols.size() - 1 - i];
                float x = float(all.right()) - i;
                float y0 = cy - std::min(1.f, std::max(-1.f, c.second)) * amp;
                float y1 = cy - std::min(1.f, std::max(-1.f, c.first)) * amp;
                fLines.emplace_back(x, y0, x, std::max(y1, y0 + 1));
            }
            p.setPen(QPen(FQT_ACCENT, 1));
            p.drawLines(fLines.data(), int(fLines.size()));
        }

    public:

        WaveformModule(const MeterSource& src) : MeterModule(src, "waveform")
        {
            setMinimumWidth(80);
            fSpeed = option("speed", 1);
            fMidSide = option("midside", 0);
        }

        virtual void tick()
        {
            uint64_t w = fSource.clock();
            static const int tenthsOfMs[] = { 400, 100, 25 };   // duration of a column
            int perColumn = std::max(1, fSource.fSampleRate * tenthsOfMs[fSpeed] / 10000);
            if (fVersion != fSource.fVersion) {
                // new routing or options: restart the history
                fVersion = fSource.fVersion;
                fRead = w;
                fCount = 0;
                for (Trace& t : fTraces) t = Trace();
            }
            int n = int(std::min<uint64_t>(w - fRead, MeterRing::kSize / 2));
            for (int k = 0; k < 2; k++) {
                fIn[k].resize(std::max(n, 1));
                fSource.readSide(k, w - n, fIn[k].data(), n);
            }
            for (int i = 0; i < n; i++) {
                float l = fIn[0][i], r = fIn[1][i];
                float v[2] = { l, r };
                if (fMidSide) { v[0] = 0.5f * (l + r); v[1] = 0.5f * (l - r); }
                for (int k = 0; k < 2; k++) {
                    Trace& t = fTraces[k];
                    if (fCount == 0) t.lo = t.hi = v[k];
                    t.lo = std::min(t.lo, v[k]);
                    t.hi = std::max(t.hi, v[k]);
                }
                if (++fCount == perColumn) {
                    for (Trace& t : fTraces) {
                        t.cols.emplace_back(t.lo, t.hi);
                        if (int(t.cols.size()) > kColumns) t.cols.pop_front();
                    }
                    fCount = 0;
                }
            }
            fRead = w;
            update();
        }

        virtual void paintGL()
        {
            QPainter p(this);
            paintBackground(p);
            QRectF all = QRectF(rect()).adjusted(2, 30, -2, -4);     // below the routing button
            float lh = float(all.height()) / 2;
            static const char* names[2][2] = { { "L", "R" }, { "M", "S" } };
            p.setFont(fqtMeterFont(8));
            for (int k = 0; k < 2; k++) {
                float cy = float(all.top()) + lh * (k + 0.5f), amp = lh * 0.45f;
                p.fillRect(QRectF(all.left(), cy, all.width(), 1), FQT_TRACK);
                paintTrace(p, fTraces[k], all, cy, amp);
                p.setPen(FQT_TEXT_DIM);
                p.drawText(QRectF(all.left() + 2, cy - lh / 2, 20, lh), Qt::AlignLeft | Qt::AlignVCenter, names[fMidSide][k]);
            }
        }
};

//==============================LEVELS=====================================

/**
 * Peak and level of L and R, linear dB scale 0..-60 (MiniMeters style), peak
 * hold. Level modes as the MiniMeters Loudness module: RMS fast (0.3 s), RMS
 * slow (1 s), LUFS momentary (0.4 s) and short-term (3 s), K-weighted as in
 * ITU-R BS.1770. The readout shows the louder side (RMS) or the loudness of
 * L + R (LUFS).
 */
class LevelsModule : public MeterModule
{

    private:

        enum { kRMSFast, kRMSSlow, kLUFSMomentary, kLUFSShortTerm };
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
            static const char* units[] = { "RMS", "RMS", "LUFS", "LUFS" };     // momentary / short-term: see the menu
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
                      { "RMS Fast (0.3 s)", "RMS Slow (1 s)", "LUFS Momentary (0.4 s)", "LUFS Short-Term (3 s)" },
                      { kRMSFast, kRMSSlow, kLUFSMomentary, kLUFSShortTerm });
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
            bool lufs = (fMode == kLUFSMomentary || fMode == kLUFSShortTerm);
            float loudest = kFloor;
            for (const Channel& ch : fChannels) loudest = std::max(loudest, ch.level);
            return lufs ? fLoudness : loudest;
        }

        LevelsModule(const MeterSource& src) : MeterModule(src, "levels")
        {
            fMode = option("mode", kRMSFast);
            fitWidth(2);
        }

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
                        ch.raw = ch.weighted = 0.0;
                        ch.count = 0;
                    }
                }
                float pdb = fqtDB(pk);
                ch.peak = std::max(pdb, ch.peak - 1.5f);      // fast attack, ~45 dB/s release
                if (pdb >= ch.hold || ++ch.holdAge > 45) { ch.hold = pdb; ch.holdAge = 0; }
            }
            fRead = w;

            // levels and readout
            static const int windows[] = { 30, 100, 40, 300 };    // in blocks of 10 ms
            bool lufs = (fMode == kLUFSMomentary || fMode == kLUFSShortTerm);
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
 * mid up, side horizontal.
 */
class StereoModule : public MeterModule
{

    private:

        enum { kScaled, kLinear };
        static const int kPoints = 2048;
        std::vector<float> fL, fR;
        std::vector<QPointF> fDots;
        float fCorr = 0.f;
        int fMode = kScaled;

        virtual void fillMenu(QMenu* menu)
        {
            addChoice(menu, "Display", "mode", fMode, { "Scaled", "Linear" }, { kScaled, kLinear });
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
            fDots.clear();
            for (int i = 0; i < int(fL.size()); i++) {
                QPointF m = map(fL[i], fR[i]);
                float x = std::max(-1.f, std::min(1.f, float(m.x()))), y = std::max(-1.f, std::min(1.f, float(m.y())));
                fDots.emplace_back(c.x() + x * half, c.y() - y * half);
            }
            p.setPen(fqtLinePen(FQT_ACCENT, 2.0));
            p.drawPoints(fDots.data(), int(fDots.size()));
        }
};

//==============================OSCILLOSCOPE=====================================

/**
 * Oscilloscope of L (accent color) and R (dimmed) overlaid, as the spectrum.
 * Follow Pitch: the trace starts on a rising zero crossing of L and its
 * length follows the period (Single: one cycle, Multi: four cycles);
 * otherwise the last 2048 samples, free running.
 */
class ScopeModule : public MeterModule
{

    private:

        static const int kWindow = 16384;
        std::vector<float> fBuffer, fBuffer2, fTrace, fTrace2;
        int fFollow = 1;
        int fCycles = 1;

        void paintTrace(QPainter& p, const std::vector<float>& trace, const QRectF& r, const QColor& color)
        {
            int len = int(trace.size());
            if (len < 2) return;
            QPainterPath path;
            for (int i = 0; i < len; i++) {
                float v = std::min(1.f, std::max(-1.f, trace[i]));
                QPointF pt(r.left() + r.width() * i / (len - 1), r.center().y() - v * r.height() / 2);
                if (i == 0) path.moveTo(pt); else path.lineTo(pt);
            }
            p.setPen(fqtLinePen(color));
            p.drawPath(path);
        }

        virtual void fillMenu(QMenu* menu)
        {
            addChoice(menu, "Follow Pitch", "follow", fFollow, { "On", "Off" }, { 1, 0 });
            addChoice(menu, "Cycle", "cycles", fCycles, { "Single", "Multi" }, { 1, 4 });
        }

    public:

        ScopeModule(const MeterSource& src) : MeterModule(src, "scope")
        {
            setMinimumWidth(50);
            fFollow = option("follow", 1);
            fCycles = option("cycles", 1);
        }

        virtual void tick()
        {
            fBuffer.resize(kWindow);
            fBuffer2.resize(kWindow);
            fSource.readPair(fBuffer.data(), fBuffer2.data(), kWindow);
            int len = 2048, start = kWindow - len;
            if (fFollow) {
                // rising zero crossings with a small hysteresis, searched backwards
                std::vector<int> ups;
                bool below = false;
                for (int i = kWindow - 1; i > 0 && int(ups.size()) <= fCycles + 1; i--) {
                    if (fBuffer[i] < -1e-4f) below = true;
                    if (below && fBuffer[i - 1] <= 0.f && fBuffer[i] > 0.f) { ups.push_back(i); below = false; }
                }
                if (int(ups.size()) > fCycles + 1) {
                    int period = ups[0] - ups[1];
                    len = std::min(std::max(fCycles * period, 32), kWindow / 2);
                    start = ups[fCycles + 1];
                    if (start + len > kWindow) start = kWindow - len;
                }
            }
            fTrace.assign(fBuffer.begin() + start, fBuffer.begin() + start + len);
            fTrace2.assign(fBuffer2.begin() + start, fBuffer2.begin() + start + len);
            update();
        }

        virtual void paintGL()
        {
            QPainter p(this);
            paintBackground(p);
            QRectF r = QRectF(rect()).adjusted(6, 8, -6, -8);
            p.setPen(QPen(FQT_TRACK, 1));
            p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
            p.setRenderHint(QPainter::Antialiasing, true);
            paintTrace(p, fTrace2, r, FQT_TEXT_DIM);
            paintTrace(p, fTrace, r, FQT_ACCENT);
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
 * the second dimmed.
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

        void init()
        {
            int n = fSize;
            for (int c = 0; c < 2; c++) { fBuffer[c].assign(n, 0.f); fSmooth[c].assign(n / 2, -200.f); }
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
        }

        // FFT size for the sample rate: x2 at 96 kHz, x4 at 192 kHz
        void updateSize()
        {
            int ratio = 1;
            while (fSource.fSampleRate >= 96000 * ratio * 0.9) ratio *= 2;
            fSize = std::min(fSizeOption * ratio, 32768);
            if (int(fWindow.size()) != fSize) init();
        }

        virtual void optionsChanged() { updateSize(); }

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
        float levelBetween(int c, float f0, float f1) const
        {
            const std::vector<float>& sm = fSmooth[c];
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

        void paintCurve(QPainter& p, int c, const QRectF& r, const QColor& color)
        {
            QPainterPath path;
            for (int x = int(r.left()); x <= int(r.right()); x++) {
                float f0 = fOf(float(x), r), f1 = fOf(float(x + 1), r);
                float y = std::min(float(r.bottom()) + 2.f, yOf(levelBetween(c, f0, f1) + tilt(f0), r));
                QPointF pt(x, std::max(float(r.top()), y));
                if (x == int(r.left())) path.moveTo(pt); else path.lineTo(pt);
            }
            p.setPen(fqtLinePen(color));
            p.drawPath(path);
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
            if (fSizeOption != 2048 && fSizeOption != 8192) fSizeOption = 4096;
            updateSize();
        }

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
            p.setFont(fqtMeterFont(8, false));
            QFontMetricsF fm(p.font());

            // grid: 1..9 per decade, labels on the decades
            float lastLabel = -1e9f;
            for (float dec = 10.f; dec < maxFreq(); dec *= 10.f) {
                for (int k = 1; k < 10; k++) {
                    float f = dec * k;
                    if (f < kMinFreq || f > maxFreq()) continue;
                    float x = xOf(f, r);
                    p.setPen(QPen(FQT_TRACK, 1));
                    p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
                    if (k == 1 && dec >= 100.f) {
                        QString label = dec >= 1000.f ? QString("%1kHz").arg(int(dec / 1000)) : QString("%1Hz").arg(int(dec));
                        float w = fm.horizontalAdvance(label);
                        // labels on the right of their line, on the left near the window buttons,
                        // skipped when they would overlap the previous one
                        float lx = (x + 3 + w > width() - 64) ? x - 3 - w : x + 3;
                        if (lx < lastLabel + 6 || lx < r.left() || lx + w > width() - 64) continue;
                        p.setPen(FQT_TEXT_DIM);
                        p.drawText(QPointF(lx, r.top() + 10), label);
                        lastLabel = lx + w;
                    }
                }
            }

            // curves: the second one first (dimmed), the first one on top
            p.setRenderHint(QPainter::Antialiasing, true);
            paintCurve(p, 1, r, FQT_TEXT_DIM);
            paintCurve(p, 0, r, FQT_ACCENT);

            // legend: L R or M S in the colors of the curves, bottom left
            p.setFont(fqtMeterFont(9));
            p.setPen(FQT_ACCENT);
            p.drawText(QPointF(r.left() + 4, r.bottom() - 4), fChannels == kMidSide ? "M" : "L");
            p.setPen(FQT_TEXT_DIM);
            p.drawText(QPointF(r.left() + 16, r.bottom() - 4), fChannels == kMidSide ? "S" : "R");

            // readout of the loudest frequency
            if (fPeakDB > -120.f && fPeakFreq > kMinFreq) {
                p.setFont(fqtReadoutFont());
                QFontMetrics pfm(p.font());
                QString txt = QString("%1dB | %2Hz | %3").arg(fPeakDB, 0, 'f', 2).arg(fPeakFreq, 0, 'f', 2).arg(note(fPeakFreq));
                if (pfm.horizontalAdvance(txt) + 6 > r.width()) txt = QString("%1dB | %2Hz").arg(fPeakDB, 0, 'f', 1).arg(fPeakFreq, 0, 'f', 0);
                if (pfm.horizontalAdvance(txt) + 6 > r.width()) txt = QString("%1Hz").arg(fPeakFreq, 0, 'f', 0);
                QRectF box = QRectF(pfm.boundingRect(txt)).adjusted(-3, -2, 3, 2);
                float px = std::max(float(r.left()), std::min(xOf(fPeakFreq, r) + 6, float(r.right() - box.width())));
                float py = std::max(float(r.top()) + 14.f, yOf(fPeakDB + tilt(fPeakFreq), r));
                box.moveTo(px, std::min(py, float(r.bottom() - box.height())));
                // keep clear of the window buttons in the top right corner
                if (box.right() > width() - 64 && box.top() < 32) box.moveTop(32);
                p.fillRect(box, FQT_METER_BG);
                p.setPen(FQT_TEXT);
                p.drawText(box, Qt::AlignCenter, txt);
            }
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
 * the next module, at the height of the level (as in MiniMeters). It lets
 * the mouse through.
 */
class MeterReadout : public QWidget
{

    private:

        QString fText;

    public:

        MeterReadout(QWidget* parent) : QWidget(parent)
        {
            setAttribute(Qt::WA_TransparentForMouseEvents);
        }

        void setText(const QString& text)
        {
            if (text == fText) return;
            fText = text;
            QFontMetrics fm(fqtMeterFont(10));
            resize(fm.horizontalAdvance(text) + 8, fm.height() + 2);
            update();
        }

        virtual void paintEvent(QPaintEvent*)
        {
            QPainter p(this);
            p.fillRect(rect(), FQT_ACCENT);
            p.setFont(fqtMeterFont(10));
            p.setPen(FQT_METER_BG);
            p.drawText(rect(), Qt::AlignCenter, fText);
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
 * The strip: the modules side by side (MiniMeters horizontal layout),
 * separated by 1 px lines, with a round button in the top left corner (same
 * style as the window buttons on the right) showing what is routed to L and R,
 * that opens the routing matrix. It sits in a vertical splitter above the controls: drag the divider
 * to resize it or to close it; the round button next to the theme button of
 * QTUI.h closes it or reopens it at its last height (saved per application).
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
        QToolButton* fToggle = nullptr;
        int fLastHeight = kHeight;          // height to restore when reopened

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
        void routingChanged()
        {
            QString text = fSource.label(MeterSource::kLeft) + " | " + fSource.label(MeterSource::kRight);
            fRoutingButton->setText(text);
            fRoutingButton->setFixedWidth(QFontMetrics(fRoutingButton->font()).horizontalAdvance(text) + 22);
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
            QRect g = fLevels->geometry();
            int y = g.top() + int(fLevels->readoutY()) - fReadout->height() / 2;
            y = std::max(g.top() + 2, std::min(y, g.bottom() - fReadout->height() - 2));
            fReadout->move(g.right() + 1, y);
            fReadout->raise();
        }

        void addModule(MeterModule* m, int stretch)
        {
            fModules.push_back(m);
            static_cast<QHBoxLayout*>(layout())->addWidget(m, stretch);
        }

        // the toggle sits on the left of the theme button (22 px wide, 6 px from the edge)
        void placeToggle()
        {
            if (fToggle) {
                fToggle->move(fWindow->width() - 22 - 6 - fToggle->width() - 6, 4);
                fToggle->raise();
            }
        }

        bool isOpen() const { return fSplitter && fSplitter->sizes().value(0) > 0; }

        // toggle button: open at the last height or close, growing or shrinking the window
        void setOpen(bool open)
        {
            int current = fSplitter->sizes().value(0);
            int controls = fSplitter->sizes().value(1);
            int h = open ? fLastHeight : 0;
            if (h == current) return;
            QScreen* screen = QGuiApplication::primaryScreen();
            int maxH = screen ? screen->availableGeometry().height() - 40 : 900;
            fWindow->resize(fWindow->width(), std::min(fWindow->height() + h - current, maxH));
            fSplitter->setSizes({ h, controls });
            saveState();
        }

        void saveState()
        {
            int h = fSplitter->sizes().value(0);
            // closed strip: hide the modules, so that nothing is drawn or composited
            for (MeterModule* m : fModules) m->setVisible(h > 0);
            if (h > 0) fLastHeight = h;
            fToggle->setToolTip(h > 0 ? "Hide meters (or drag the divider)" : "Show meters (or drag the divider down)");
            settings().setValue("meters/height", h);
            settings().setValue("meters/last_height", fLastHeight);
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
            fLastHeight = std::max(int(kMinHeight), settings().value("meters/last_height", kHeight).toInt());

            QHBoxLayout* row = new QHBoxLayout(this);
            row->setContentsMargins(0, 0, 0, 1);    // bottom line
            row->setSpacing(1);                     // separators: the panel background shows through

            // order and proportions of the MiniMeters horizontal layout
            addModule(new WaveformModule(fSource), 386);
            fLevels = new LevelsModule(fSource);
            addModule(fLevels, 0);
            addModule(new StereoModule(fSource), 341);
            addModule(new ScopeModule(fSource), 161);
            addModule(new SpectrumModule(fSource), 423);

            fReadout = new MeterReadout(this);
            fRoutingButton = roundButton(this, 60, "Routing: inputs and outputs shown as L and R");
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
            panel->fWindow->setCentralWidget(panel->fSplitter);
            QObject::connect(panel->fSplitter, &QSplitter::splitterMoved, [panel]() { panel->saveState(); });

            panel->fToggle = new QToolButton(panel->fWindow);
            panel->fToggle->setObjectName("themeButton");          // same round style as the theme button
            panel->fToggle->setFixedSize(22, 22);
            panel->fToggle->setCursor(Qt::PointingHandCursor);
            panel->fToggle->setText(QString::fromUtf8("\u223F"));  // sine wave
            QObject::connect(panel->fToggle, &QToolButton::clicked, [panel]() { panel->setOpen(!panel->isOpen()); });
            panel->fWindow->installEventFilter(panel);
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
            fWindow->resize(std::max(fWindow->width(), 760), std::min(controls + h + fSplitter->handleWidth(), maxH));
            fSplitter->setSizes({ h, controls });   // rescaled by the splitter if the screen is too small
            saveState();
            placeToggle();
        }

        bool eventFilter(QObject* obj, QEvent* event) override
        {
            if (obj == fWindow && event->type() == QEvent::Resize) placeToggle();
            return QWidget::eventFilter(obj, event);
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
