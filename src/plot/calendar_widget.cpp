#include "plot/calendar_widget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "plot/plot_widget.h"  // widen()
#include "util/assert.h"

namespace plot {

using Microsoft::WRL::ComPtr;

namespace {

constexpr float kFontPx = 12.0f;
constexpr float kSmallPx = 11.0f;
constexpr float kPad = 8.0f;
constexpr float kHeaderRow = 30.0f;
constexpr float kWeekdayRow = 20.0f;
constexpr float kCellPad = 5.0f;
constexpr float kLine = 15.0f;
constexpr float kStepsBar = 4.0f;
constexpr float kChip = 22.0f;
constexpr int kColumns = 7;
constexpr int kMaxRows = 6;
constexpr size_t kMaxActivityLines = 3;
constexpr double kLbPerKg = 2.20462262;  // display unit; the database stays in kg

constexpr Color kBg = rgb(0xFFFFFF);
constexpr Color kGrid = rgb(0xD0D0D0);
constexpr Color kWeekend = rgb(0xF6F6F6);
constexpr Color kText = rgb(0x222222);
constexpr Color kMuted = rgb(0x666666);
constexpr Color kFaint = rgb(0xAAAAAA);
constexpr Color kHover = rgb(0x1F77B4);
constexpr Color kStepsFill = rgb(0xBCBD22);
constexpr Color kStepsTrack = rgb(0xEAEAEA);
constexpr Color kScoreGood = rgb(0x2CA02C);
constexpr Color kScoreFair = rgb(0xFF9800);
constexpr Color kScorePoor = rgb(0xD62728);
constexpr Color kTooltipBg = rgb(0xFFFFFF, 0.97f);

const wchar_t* kWeekdays[kColumns] = {L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun"};

D2D1_COLOR_F to_d2d(const Color& c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }

Color score_color(int64_t score) {
  if (score >= 80) return kScoreGood;
  if (score >= 60) return kScoreFair;
  return kScorePoor;
}

std::string thousands(int64_t v) {
  std::string s = std::to_string(v);
  std::string out;
  int n = 0;
  for (size_t i = s.size(); i > 0; --i) {
    out.insert(out.begin(), s[i - 1]);
    if (++n % 3 == 0 && i > 1) out.insert(out.begin(), ',');
  }
  return out;
}

}  // namespace

bool CalendarWidget::init(ID2D1Factory* d2d, IDWriteFactory* dwrite) {
  G_ASSERT(d2d != nullptr && dwrite != nullptr);
  d2d_ = d2d;
  dwrite_ = dwrite;
  set_dpi_scale(scale_);
  return font_ && font_bold_ && font_small_;
}

void CalendarWidget::set_dpi_scale(float scale) {
  G_ASSERT(scale > 0.1f && scale < 10.0f);
  scale_ = scale;
  G_REQUIRE_VOID(dwrite_ != nullptr);
  auto make = [&](float px, DWRITE_FONT_WEIGHT w, ComPtr<IDWriteTextFormat>& out) {
    out.Reset();
    dwrite_->CreateTextFormat(L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, s(px), L"en-us", &out);
    if (out) out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  };
  make(kFontPx, DWRITE_FONT_WEIGHT_NORMAL, font_);
  make(kFontPx, DWRITE_FONT_WEIGHT_SEMI_BOLD, font_bold_);
  make(kSmallPx, DWRITE_FONT_WEIGHT_NORMAL, font_small_);
}

void CalendarWidget::set_month(MonthData m) {
  G_ASSERT(m.days.size() <= 31);
  G_ASSERT(m.first_weekday >= 0 && m.first_weekday < kColumns);
  month_ = std::move(m);
}

void CalendarWidget::set_hover(float px, float py, bool inside) {
  hover_ = inside;
  hover_px_ = px;
  hover_py_ = py;
}

void CalendarWidget::compute_cells() {
  cells_.clear();
  const int n = static_cast<int>(month_.days.size());
  G_REQUIRE_VOID(n > 0);
  const int rows = std::min(kMaxRows, (month_.first_weekday + n + kColumns - 1) / kColumns);
  const float left = rect_.left + s(kPad);
  const float right = rect_.right - s(kPad);
  const float top = rect_.top + s(kPad + kHeaderRow + kWeekdayRow);
  const float bottom = rect_.bottom - s(kPad);
  const float cw = std::max(20.0f, (right - left) / kColumns);
  const float ch = std::max(20.0f, (bottom - top) / static_cast<float>(rows));
  for (int d = 1; d <= n; ++d) {
    const int slot = month_.first_weekday + d - 1;
    const int row = slot / kColumns;
    const int col = slot % kColumns;
    if (row >= rows) break;
    CellRect c;
    c.day = d;
    c.r = D2D1::RectF(left + col * cw, top + row * ch, left + (col + 1) * cw,
                      top + (row + 1) * ch);
    cells_.push_back(c);
  }
}

int CalendarWidget::cell_at(float px, float py) const {
  for (const CellRect& c : cells_) {
    if (px >= c.r.left && px < c.r.right && py >= c.r.top && py < c.r.bottom) return c.day;
  }
  return 0;
}

void CalendarWidget::ensure_brush(ID2D1RenderTarget* rt) {
  if (brush_ && brush_rt_ == rt) return;
  brush_.Reset();
  rt->CreateSolidColorBrush(to_d2d(kText), &brush_);
  brush_rt_ = rt;
}

void CalendarWidget::text(ID2D1RenderTarget* rt, const std::wstring& str, const D2D1_RECT_F& box,
                          DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT valign,
                          const Color& c, bool bold) {
  IDWriteTextFormat* f = bold ? font_bold_.Get() : font_.Get();
  G_REQUIRE_VOID(f != nullptr && brush_);
  f->SetTextAlignment(align);
  f->SetParagraphAlignment(valign);
  brush_->SetColor(to_d2d(c));
  rt->DrawTextW(str.c_str(), static_cast<UINT32>(str.size()), f, box, brush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float CalendarWidget::measure(const std::wstring& str, bool bold) {
  IDWriteTextFormat* f = bold ? font_bold_.Get() : font_.Get();
  G_REQUIRE_RET(f != nullptr && dwrite_, 0.0f);
  ComPtr<IDWriteTextLayout> layout;
  G_REQUIRE_RET(SUCCEEDED(dwrite_->CreateTextLayout(str.c_str(), static_cast<UINT32>(str.size()),
                                                    f, 4096.0f, 100.0f, &layout)),
                0.0f);
  DWRITE_TEXT_METRICS m{};
  G_REQUIRE_RET(SUCCEEDED(layout->GetMetrics(&m)), 0.0f);
  return m.widthIncludingTrailingWhitespace;
}

void CalendarWidget::render(ID2D1RenderTarget* rt) {
  G_ASSERT(rt != nullptr);
  ensure_brush(rt);
  G_REQUIRE_VOID(brush_);
  brush_->SetColor(to_d2d(kBg));
  rt->FillRectangle(rect_, brush_.Get());
  if (!has_month()) {
    text(rt, L"No data", rect_, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
         kMuted);
    return;
  }
  compute_cells();

  // Month title and weekday header.
  const float left = rect_.left + s(kPad);
  const float right = rect_.right - s(kPad);
  text(rt, widen(month_.title), D2D1::RectF(left, rect_.top + s(kPad), right,
                                            rect_.top + s(kPad + kHeaderRow)),
       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kText, true);
  const float cw = (right - left) / kColumns;
  const float wy = rect_.top + s(kPad + kHeaderRow);
  for (int c = 0; c < kColumns; ++c) {
    text(rt, kWeekdays[c], D2D1::RectF(left + c * cw, wy, left + (c + 1) * cw, wy + s(kWeekdayRow)),
         DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kMuted);
  }

  const int hovered = hover_ ? cell_at(hover_px_, hover_py_) : 0;
  for (const CellRect& c : cells_) {
    const int slot = month_.first_weekday + c.day - 1;
    const bool weekend = (slot % kColumns) >= 5;
    draw_cell(rt, month_.days[static_cast<size_t>(c.day - 1)], c.r, weekend, c.day == hovered);
  }
  if (hovered > 0) draw_tooltip(rt, month_.days[static_cast<size_t>(hovered - 1)]);
}

void CalendarWidget::draw_cell(ID2D1RenderTarget* rt, const DayCell& d, const D2D1_RECT_F& r,
                               bool weekend, bool hovered) {
  if (weekend) {
    brush_->SetColor(to_d2d(kWeekend));
    rt->FillRectangle(r, brush_.Get());
  }
  brush_->SetColor(to_d2d(hovered ? kHover : kGrid));
  rt->DrawRectangle(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f),
                    brush_.Get(), hovered ? 2.0f : 1.0f);

  const float x = r.left + s(kCellPad);
  const float w = r.right - r.left - s(kCellPad * 2);
  float y = r.top + s(kCellPad);
  const Color day_color = d.has_data() ? kText : kFaint;
  text(rt, std::to_wstring(d.day), D2D1::RectF(x, y, x + s(30.0f), y + s(kLine)),
       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, day_color, true);

  // Sleep score chip, top right.
  if (d.sleep_score) {
    const D2D1_RECT_F chip = D2D1::RectF(r.right - s(kCellPad + kChip + 6.0f), y - s(1.0f),
                                         r.right - s(kCellPad), y + s(kLine + 1.0f));
    brush_->SetColor(to_d2d(score_color(*d.sleep_score)));
    rt->FillRoundedRectangle(D2D1::RoundedRect(chip, s(3.0f), s(3.0f)), brush_.Get());
    text(rt, std::to_wstring(*d.sleep_score), chip, DWRITE_TEXT_ALIGNMENT_CENTER,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kBg, true);
  }
  y += s(kLine + 3.0f);
  if (r.bottom - y < s(kLine)) return;

  // Steps with a goal bar.
  if (d.steps) {
    text(rt, widen(thousands(*d.steps) + " steps"), D2D1::RectF(x, y, x + w, y + s(kLine)),
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kMuted);
    y += s(kLine);
    const float frac = static_cast<float>(
        std::clamp(static_cast<double>(*d.steps) / static_cast<double>(month_.step_goal), 0.0, 1.0));
    brush_->SetColor(to_d2d(kStepsTrack));
    rt->FillRectangle(D2D1::RectF(x, y, x + w, y + s(kStepsBar)), brush_.Get());
    brush_->SetColor(to_d2d(kStepsFill));
    rt->FillRectangle(D2D1::RectF(x, y, x + w * frac, y + s(kStepsBar)), brush_.Get());
    y += s(kStepsBar + 4.0f);
  }
  if (r.bottom - y < s(kLine)) return;

  // Resting HR, then Body Battery range, each on its own line so narrow
  // cells do not clip the range.
  if (d.resting_hr) {
    text(rt, widen("RHR " + std::to_string(*d.resting_hr)), D2D1::RectF(x, y, x + w, y + s(kLine)),
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kMuted);
    y += s(kLine);
  }
  if (d.bb_high && d.bb_low && r.bottom - y >= s(kLine)) {
    text(rt, widen("BB " + std::to_string(*d.bb_low) + " to " + std::to_string(*d.bb_high)),
         D2D1::RectF(x, y, x + w, y + s(kLine)), DWRITE_TEXT_ALIGNMENT_LEADING,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kMuted);
    y += s(kLine);
  }

  // Activities in the small font.
  G_REQUIRE_VOID(font_small_);
  font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  font_small_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  const size_t n = std::min(d.activities.size(), kMaxActivityLines);
  for (size_t i = 0; i < n && r.bottom - y >= s(kLine); ++i) {
    brush_->SetColor(to_d2d(colors::kBodyBattery));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + s(3.0f), y + s(kLine / 2)), s(2.5f), s(2.5f)),
                    brush_.Get());
    const std::wstring ws = widen(d.activities[i]);
    brush_->SetColor(to_d2d(kText));
    rt->DrawTextW(ws.c_str(), static_cast<UINT32>(ws.size()), font_small_.Get(),
                  D2D1::RectF(x + s(10.0f), y, x + w, y + s(kLine)), brush_.Get(),
                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
    y += s(kLine);
  }
  if (d.activities.size() > n && r.bottom - y >= s(kLine)) {
    text(rt, L"+" + std::to_wstring(d.activities.size() - n) + L" more",
         D2D1::RectF(x + s(10.0f), y, x + w, y + s(kLine)), DWRITE_TEXT_ALIGNMENT_LEADING,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kFaint);
  }
}

void CalendarWidget::draw_tooltip(ID2D1RenderTarget* rt, const DayCell& d) {
  G_ASSERT(rt != nullptr && brush_);
  G_ASSERT(d.day >= 1 && d.day <= 31);
  std::vector<std::wstring> lines;
  char buf[96] = {};
  std::snprintf(buf, sizeof(buf), "%d %s", d.day, month_.title.c_str());
  lines.push_back(widen(buf));
  if (d.steps) lines.push_back(widen(thousands(*d.steps) + " steps"));
  if (d.resting_hr) lines.push_back(widen("Resting HR " + std::to_string(*d.resting_hr) + " bpm"));
  if (d.sleep_score || d.sleep_hours) {
    std::string t = "Sleep";
    if (d.sleep_hours) {
      std::snprintf(buf, sizeof(buf), " %.1f h", *d.sleep_hours);
      t += buf;
    }
    if (d.sleep_score) t += "  score " + std::to_string(*d.sleep_score);
    lines.push_back(widen(t));
  }
  if (d.bb_high && d.bb_low) {
    lines.push_back(widen("Body Battery " + std::to_string(*d.bb_low) + " to " +
                          std::to_string(*d.bb_high)));
  }
  if (d.avg_stress) lines.push_back(widen("Avg stress " + std::to_string(*d.avg_stress)));
  if (d.weight_kg) {
    std::snprintf(buf, sizeof(buf), "Weight %.1f lb", *d.weight_kg * kLbPerKg);
    lines.push_back(widen(buf));
  }
  for (const std::string& a : d.activities) lines.push_back(widen("- " + a));
  if (lines.size() == 1) lines.push_back(L"No data");

  float w = 0.0f;
  for (size_t i = 0; i < lines.size(); ++i) w = std::max(w, measure(lines[i], i == 0));
  const float bw = w + s(kPad * 2);
  const float bh = s(kLine) * static_cast<float>(lines.size()) + s(kPad * 2);
  float bx = hover_px_ + s(14.0f);
  float by = hover_py_ + s(14.0f);
  if (bx + bw > rect_.right) bx = hover_px_ - s(14.0f) - bw;
  if (by + bh > rect_.bottom) by = rect_.bottom - bh;
  const D2D1_RECT_F box = D2D1::RectF(bx, by, bx + bw, by + bh);
  brush_->SetColor(to_d2d(kTooltipBg));
  rt->FillRectangle(box, brush_.Get());
  brush_->SetColor(to_d2d(kGrid));
  rt->DrawRectangle(box, brush_.Get(), 1.0f);
  float y = by + s(kPad);
  for (size_t i = 0; i < lines.size(); ++i) {
    text(rt, lines[i], D2D1::RectF(bx + s(kPad), y, bx + bw, y + s(kLine)),
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kText, i == 0);
    y += s(kLine);
  }
}

}  // namespace plot
