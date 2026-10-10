#include "Knob.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "../GuiFpu.h"

namespace sq8l::gui {

namespace {
// 0x4020d97c: 0.8 * pi as Single (pointer angle at the maximum).
constexpr uint32_t kAngleMaxBits = 0x4020d97cu;
float bitsToFloat(uint32_t b) {
    float f;
    static_assert(sizeof f == sizeof b, "");
    std::memcpy(&f, &b, sizeof f);
    return f;
}
}  // namespace

Knob::Knob(std::string name) : Control(std::move(name), true) {
    // TGraphKnobB_v011 (0x479d08), the parts that matter without caption/value text.
    showCaption_ = true;   // +0x2b5 / +0x2b6 default True (the form sets False)
    showValue_ = true;
    restoreMouse_ = true;  // +0x228
    setBounds(0, 0, 0x22, 0x36);
    layout(0x10);
    minFineFac_ = 0.2f;
    maxFinePixDist_ = 200;
    maxPixDist_ = 256;
    min_ = 0.0f;
    max_ = 127.0f;
    range_ = 128.0f;
    pixFactor_ = static_cast<float>(static_cast<double>(range_) / (static_cast<double>(maxPixDist_) * 0.5));
    step_ = 0.0f;
    intMode_ = false;
    snapToZero_ = true;
    snapZone_ = 0.0f;
    angleMax_ = bitsToFloat(kAngleMaxBits);
    angleMin_ = -angleMax_;
    angleRange_ = static_cast<float>(static_cast<double>(angleMax_) - static_cast<double>(angleMin_));
    angleK_ = static_cast<float>(static_cast<double>(angleRange_) / static_cast<double>(range_));
    setValue(0.0f);
}

// ------------------------------------------------------------------ value model

// x87 compares: the branch conditions are written so that unordered operands (NaN) take
// the same path as on the original (fcomp + sahf + jbe/jae/jb/je).
void Knob::setValue(float v) {
    bool changed = v < value_ || v > value_;  // setne after fcomp (unordered -> false)
    if (step_ > 0.0f) {
        int32_t q = fpu::roundEven(static_cast<float>(static_cast<double>(v) / static_cast<double>(step_)));
        v = static_cast<float>(static_cast<double>(q) * static_cast<double>(step_));
    }
    if (v > max_)
        v = max_;
    else if (!(v >= min_))
        v = min_;
    if (v < value_ || v > value_ || force_) {
        value_ = v;
        angle_ = static_cast<float>((static_cast<double>(v) - static_cast<double>(min_)) * static_cast<double>(angleK_) +
                                    static_cast<double>(angleMin_));
    }
    if (force_ || changed || !textValid_) textValid_ = true;  // FUN_0047b218 formats the value text
    if ((changed || force_) && loaded_) invalidate();
    force_ = false;
}

void Knob::recalcRange() {
    range_ = static_cast<float>(static_cast<double>(max_) - static_cast<double>(min_));
    snapZone_ = static_cast<float>(fpu::mulExt(fpu::kExt004Hi, fpu::kExt004Lo, range_));
    if (maxPixDist_ > 0)
        pixFactor_ = static_cast<float>(static_cast<double>(range_) /
                                        (static_cast<double>(maxPixDist_) * static_cast<double>(0.5f)));
    else
        pixFactor_ = 0.0f;
    if (range_ > 0.0f)
        angleK_ = static_cast<float>(static_cast<double>(angleRange_) / static_cast<double>(range_));
    else
        angleK_ = 0.0f;
    force_ = true;
    setValue(value_);
    if (loaded_) invalidate();
}

void Knob::setMinValue(float v) {
    if (v > max_) v = max_;
    min_ = v;
    recalcRange();
}

void Knob::setMaxValue(float v) {
    if (!(v >= min_)) v = min_;
    max_ = v;
    recalcRange();
}

void Knob::setValueStep(float v) {
    step_ = v >= 0.0f ? v : 0.0f;
    // Frac() runs with the rounding control forced to truncation.
    intMode_ = v > 0.0f && (static_cast<double>(v) - std::trunc(static_cast<double>(v))) == 0.0;
    if (intMode_) numDecimals_ = 0;
    loaded_ = false;
    setMinValue(min_);
    loaded_ = true;  // (sic) the original leaves the "loaded" flag set
    setValue(value_);
}

void Knob::setMaxPixDist(int pixels) {
    int p = std::abs(pixels);
    if (p > 0x4000)
        p = 0x4000;
    else if (p < 1)
        p = 1;
    maxPixDist_ = p;
    pixFactor_ = static_cast<float>(static_cast<double>(range_) / static_cast<double>(p));
}

void Knob::setMaxFinePixDist(int pixels) { maxFinePixDist_ = pixels < 0 ? 0 : pixels; }

void Knob::layout(int radius) {
    // FUN_0047af34
    if (gif_ && gif_->loaded()) radius = std::max(gif_->frameWidth(), gif_->frameHeight()) / 2;
    int captionH = showCaption_ ? captionHeight_ + 1 : 0;
    int valueH = showValue_ ? valueHeight_ + 1 : 0;
    radius_ = radius;
    setHeight(captionH + radius * 2 + valueH);
    if (width_ < radius * 2) setWidth(radius * 2);
    centerX_ = width_ / 2;
    centerY_ = captionH + radius;
    frameX_ = centerX_ - radius;
    frameY_ = centerY_ - radius;
    valueTop_ = height_ - valueHeight_ + 1;
    if (loaded_) invalidate();
}

void Knob::setRadius(int r) { layout(r); }

void Knob::setAniGif(const Sprite* gif) {
    gif_ = gif;
    layout(radius_);
    invalidate();
}

void Knob::setActive(bool a) {
    if (a == active_) return;
    active_ = a;
    invalidate();
}

int Knob::frameIndex() const {
    if (!gif_ || !gif_->loaded()) return -1;
    float r = static_cast<float>(static_cast<double>(max_) - static_cast<double>(min_));
    int idx = 0;
    if (r < 0.0f || r > 0.0f) {
        float t = static_cast<float>((static_cast<double>(value_) - static_cast<double>(min_)) / static_cast<double>(r) *
                                     static_cast<double>(gif_->count()));
        idx = fpu::roundEven(t);
    }
    if (idx < 0)
        idx = 0;
    else if (idx >= gif_->count())
        idx = gif_->count() - 1;
    return idx;
}

// ------------------------------------------------------------------ painting

void Knob::paint(Canvas& canvas) {
    loaded_ = true;
    if (!gif_ || !gif_->loaded()) return;
    const ImageView f = gif_->frame(frameIndex());
    // The frame covers the whole control, so its own corner pixel is the backdrop the knob
    // sits on: fading towards it sinks the knob into the panel without reading the form.
    if (active_)
        canvas.draw(frameX_, frameY_, f);
    else
        canvas.drawFaded(frameX_, frameY_, f, f ? f.at(0, 0) : 0, kFadeAmount);
}

// ------------------------------------------------------------------ drag

void Knob::beginDrag(ShiftState, int x, int y) {
    if (onGetMousePos)
        onGetMousePos(*this);
    else if (getCursorPos)
        getCursorPos(savedMouseX, savedMouseY);
    downX_ = x;
    downY_ = y;
    lastX_ = x;
    lastY_ = y;
    if (!(value_ >= 0.0f))
        acc_ = static_cast<float>(static_cast<double>(value_) - static_cast<double>(snapZone_));
    else
        acc_ = static_cast<float>(static_cast<double>(value_) + static_cast<double>(snapZone_));
    dragActive_ = true;
}

void Knob::dragMove(ShiftState shift, int x, int y) {
    if (!dragActive_) return;
    int dx = std::abs(x - downX_) + 1;
    if (dx > maxFinePixDist_) dx = maxFinePixDist_;
    int dy = lastY_ - y;
    float fac;
    if (shift & ssShift) {
        fac = minFineFac_;
    } else {
        if (maxFinePixDist_ < 1)
            fac = 0.0f;
        else
            fac = static_cast<float>(1.0 - static_cast<double>(dx) / static_cast<double>(maxFinePixDist_));
        if (!(fac >= minFineFac_)) fac = minFineFac_;
    }
    float delta = static_cast<float>(static_cast<double>(dy) * static_cast<double>(fac) * static_cast<double>(pixFactor_));
    acc_ = static_cast<float>(static_cast<double>(acc_) + static_cast<double>(delta));
    if (snapToZero_ && !(std::fabs(acc_) >= snapZone_))
        setValue(0.0f);
    else if (!(acc_ >= 0.0f))
        setValue(static_cast<float>(static_cast<double>(acc_) + static_cast<double>(snapZone_)));
    else
        setValue(static_cast<float>(static_cast<double>(acc_) - static_cast<double>(snapZone_)));
    if (onChange) onChange(*this);
    lastX_ = x;
    lastY_ = y;
}

void Knob::endDrag() {
    if (!dragActive_) return;
    dragActive_ = false;
    if (!restoreMouse_) return;
    if (onRestoreMousePos)
        onRestoreMousePos(*this);
    else if (setCursorPos)
        setCursorPos(savedMouseX, savedMouseY);
}

void Knob::cancelDrag() { dragActive_ = false; }

// ------------------------------------------------------------------ mouse

void Knob::mouseDown(MouseButton button, ShiftState shift, int x, int y) {
    Control::mouseDown(button, shift, x, y);
    setFocus();
    bool inValue = showValue_ && y >= valueTop_;
    if (button == MouseButton::Left && !inValue) {
        dragging_ = true;
        beginDrag(shift, x, y);
    } else if (inValue) {
        labelClick_ = true;
    }
}

void Knob::mouseMove(ShiftState shift, int x, int y) {
    Control::mouseMove(shift, x, y);
    if (mouseCaptureControl() != this)
        endDrag();
    else if (dragging_)
        dragMove(shift, x, y);
}

void Knob::mouseUp(MouseButton button, ShiftState shift, int x, int y) {
    Control::mouseUp(button, shift, x, y);
    if (dragging_ && button == MouseButton::Left) {
        dragging_ = false;
        endDrag();
    }
    labelClick_ = false;
}

void Knob::dblClick() {
    cancelDrag();
    Control::dblClick();
}

// ------------------------------------------------------------------ raw state

Knob::State Knob::state() const {
    State s{};
    s.value = value_;
    s.min = min_;
    s.max = max_;
    s.range = range_;
    s.step = step_;
    s.pixFactor = pixFactor_;
    s.snapZone = snapZone_;
    s.minFineFac = minFineFac_;
    s.acc = acc_;
    s.angle = angle_;
    s.angleK = angleK_;
    s.maxPixDist = maxPixDist_;
    s.maxFinePixDist = maxFinePixDist_;
    s.radius = radius_;
    s.frameX = frameX_;
    s.frameY = frameY_;
    s.downX = downX_;
    s.downY = downY_;
    s.lastX = lastX_;
    s.lastY = lastY_;
    s.snapToZero = snapToZero_;
    s.intMode = intMode_;
    s.dragging = dragging_;
    s.dragActive = dragActive_;
    s.restoreMouse = restoreMouse_;
    s.loaded = loaded_;
    s.force = force_;
    s.textValid = textValid_;
    return s;
}

void Knob::setState(const State& s) {
    value_ = s.value;
    min_ = s.min;
    max_ = s.max;
    range_ = s.range;
    step_ = s.step;
    pixFactor_ = s.pixFactor;
    snapZone_ = s.snapZone;
    minFineFac_ = s.minFineFac;
    acc_ = s.acc;
    angle_ = s.angle;
    angleK_ = s.angleK;
    maxPixDist_ = s.maxPixDist;
    maxFinePixDist_ = s.maxFinePixDist;
    radius_ = s.radius;
    frameX_ = s.frameX;
    frameY_ = s.frameY;
    downX_ = s.downX;
    downY_ = s.downY;
    lastX_ = s.lastX;
    lastY_ = s.lastY;
    snapToZero_ = s.snapToZero;
    intMode_ = s.intMode;
    dragging_ = s.dragging;
    dragActive_ = s.dragActive;
    restoreMouse_ = s.restoreMouse;
    loaded_ = s.loaded;
    force_ = s.force;
    textValid_ = s.textValid;
    invalidate();
}

}  // namespace sq8l::gui
