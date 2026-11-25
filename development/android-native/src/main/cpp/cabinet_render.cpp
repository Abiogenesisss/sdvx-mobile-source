#include "cabinet.h"
#include "cabinet_crew_catalog.h"
#include "cabinet_rewards.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_stdinc.h>
#include <RmlUi/Core.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <numbers>
#include <sstream>
#include <numeric>

namespace sdvx {
namespace {
constexpr const char* EntryNotices[] = {
#include "entry_notice_2026101300.inc"
};
constexpr Color Glass{.035f, .065f, .059f, .86f};
constexpr Color Green{.69f, .89f, .30f, 1};
constexpr Color Cyan{.34f, .89f, .93f, 1};
constexpr Color ReceiptText{.878f, .878f, .878f, 1};
constexpr Color ReceiptAmount{0, .81f, .13f, 1};
constexpr Rect Screen{0, 0, Width, Height};
constexpr float HeaderTextY = 20;
constexpr std::array<const char*, ButtonCount> Buttons{
    "BT-A", "BT-B", "BT-C", "BT-D", "FX-L", "FX-R", "START", "BACK", "呼出副屏"};
constexpr std::array<int, 8> ModeTexts{5, 6, 7, 1, 2, 4, 8, 10};
constexpr int ModePriceWidth = 240, ModePriceHeight = 48;
constexpr float RowX = 522, RowTop = 873, RowHeight = 174;

Rect uiBounds(Rml::Element& element) {
    const auto origin = element.GetAbsoluteOffset();
    const auto size = element.GetBox().GetSize();
    return {origin.x, origin.y, size.x, size.y};
}

void uiPixels(Rml::Element& element, Rml::PropertyId property, float pixels) {
    const Rml::Property value{pixels, Rml::Unit::PX};
    const auto* current = element.GetLocalProperty(property);
    if (!current || *current != value) element.SetProperty(property, value);
}

Color uiColor(const Rml::Element& element, const char* attribute, Color fallback = White) {
    if (const auto* value = element.GetAttribute(attribute)) {
        const auto rgba = value->Get<std::string>();
        std::sscanf(rgba.c_str(), "%f %f %f %f", &fallback.r, &fallback.g, &fallback.b, &fallback.a);
    }
    return fallback;
}

int wrap(int value, int count) { return ((value % count) + count) % count; }

std::string lower(std::string_view value) {
    std::string result(value);
    for (char& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}

std::string gradeAsset(std::string_view grade) {
    std::string name = lower(grade);
    if (name.empty()) return "no";
    if (name.back() == '+') name.replace(name.size() - 1, 1, "_plus");
    return name;
}

std::string outcomeAsset(std::string_view outcome) {
    std::string name = lower(outcome);
    if (name.empty()) return "no";
    return name == "spuc" ? "puc" : name;
}


std::string bpmString(const Selection& track) {
    char result[32];
    if (track.bpmMin != track.bpmMax)
        std::snprintf(result, sizeof(result), "%g–%g", track.bpmMin / 100., track.bpmMax / 100.);
    else std::snprintf(result, sizeof(result), "%g", track.bpmMin / 100.);
    return result;
}

std::string levelString(int tenths) {
    char result[16];
    if (tenths >= 180 || tenths % 10) std::snprintf(result, sizeof(result), "%.1f", tenths / 10.);
    else std::snprintf(result, sizeof(result), "%02d", tenths / 10);
    return result;
}


}

void Cabinet::useField(Rml::Element& element) {
    if (element.IsClassSet("original-text")) uiLayers_.push_back(&element);
    else pendingFields_.push_back(&element);
}

void Cabinet::setUiText(Rml::Element& element, std::string_view value) {
    if (!element.GetNumChildren()) {
        element.AppendChild(document_->CreateTextNode(std::string(value)));
        uiDirty_ = true;
        return;
    }
    auto& text = static_cast<Rml::ElementText&>(*element.GetChild(0));
    if (text.GetText() != value) {
        text.SetText(std::string(value));
        uiDirty_ = true;
    }
}

void Cabinet::field(const char* id, std::string_view value) {
    if (!document_) return;
    auto& element = *document_->GetElementById(id);
    setUiText(element, value);
    useField(element);
}

void Cabinet::rowField(const char* id, int row, int column, std::string_view value) {
    if (!document_) return;
    auto& element = *document_->GetElementById(id)->GetChild(row)->GetChild(column);
    setUiText(element, value);
    useField(element);
}

void Cabinet::art(std::string_view name, Rect bounds) {
    render_.image("menu/art/" + std::string(name) + ".png", bounds);
}

void Cabinet::ap(std::string_view folder, std::string_view animation, std::string_view clip,
                 float x, float y, double ms, bool loop, const Bindings& bindings) {
    const auto size = render_.afpSize(folder, animation, clip);
    if (size.x > 0 && size.y > 0)
        render_.afp(folder, animation, clip, ms, loop, {x, y, size.x, size.y}, bindings);
}

void Cabinet::draw(double nowMs) {
    if (startup_) {
        drawStartup(startup_->started < 0 ? 0 : nowMs - startup_->started);
        return;
    }
    if (lanePreview_) {
        const double elapsed = nowMs - coveredAt_;
        if (settingsReturn_ == Page::Songs) drawSongs(elapsed);
        else if (settingsReturn_ == Page::Matching) drawMatching(elapsed);
        else drawRoom(nowMs - roomStarted_);
        flushLabels();
        const double opening = render_.afpDuration("selection_ui", "lane_preview", "in");
        render_.afp("selection_ui", "lane_preview", lanePreview_->timeMs < opening ? "in" : "in_end",
                    lanePreview_->timeMs);
        lanePreview_->draw(render_, nowMs, options_, held_);
        footer();
        flushLabels();
        return;
    }
    // NEXT AREA replaces the full canvas with opaque bg_iseki during its
    // entrance and hold. Keep drawing the destination under the exit AFP.
    if (transition_ && transition_->kind == TransitionKind::NextArea
        && transition_->phase == TransitionPhase::Covering
        && nowMs - transition_->started < transition_->inMs + transition_->holdMs) {
        drawTransition(*transition_, nowMs - transition_->started);
        return;
    }
    drawScene(std::max(0., nowMs - entered_));
    if (messageUntil_ > nowMs && page_ != Page::Stage) {
        flushLabels();
        render_.afp("session_dialog", "win_connect", "in_end", 0);
        field("notice", message_);
    }
    flushLabels();
    if (achievementPopupAt_ >= 0 && landscape()) {
        const double elapsed = nowMs - achievementPopupAt_;
        const double intro = render_.afpDuration("subscreen", "win_sbm_common_small", "in");
        render_.afp("subscreen", "win_sbm_common_small", elapsed < intro ? "in" : "in_end", elapsed, false, CrewPanel);
        if (elapsed >= intro) {
            field("achievement-heading", "ACHIEVEMENTを達成しました");
            field("achievement-message", achievementRewardText_);
            flushLabels();
        }
    }
    if (transition_) drawTransition(*transition_, nowMs - transition_->started);
}

void Cabinet::drawStartup(double elapsed) {
    // Match the white flash, red field, logo fade and e-amusement dissolve in the arcade recording.
    const auto fade = [elapsed](double from, double to) {
        return static_cast<float>(std::clamp((elapsed - from) / (to - from), 0., 1.));
    };
    if (elapsed < 2325) {
        render_.image(StartupWarning, Screen);
        render_.rect(Screen, {1, 1, 1, fade(1980, 2325)});
    } else {
        constexpr Color KonamiRed{182 / 255.f, 0, 20 / 255.f};
        render_.rect(Screen, KonamiRed);
        render_.image(StartupKonami, Screen, {1, 1, 1, fade(2900, 3200) * (1 - fade(5350, 5550))});
        if (elapsed < 2650) render_.rect(Screen, {1, 1, 1, 1 - fade(2325, 2650)});
        if (elapsed >= 5770) render_.image(StartupAmusement, Screen, {1, 1, 1, fade(5770, 6145)});
    }
    constexpr Color credit{.58f, .66f, .52f};
    render_.text("EXTRA PASELI: 0", 61, 1893, 22, credit);
    render_.text("CREDIT: 0", 990, 1893, 22, credit, TextAlign::Right);
}

void Cabinet::flushLabels() {
    if (pendingFields_.empty() && uiLayers_.empty()) return;
    const bool hasOverlay = !pendingFields_.empty();
    // Flush at the existing AFP layer boundaries. Keep DOM nodes and the cached
    // overlay when the same fields are drawn again with the same values.
    if (!pendingFields_.empty() && visibleFields_ != pendingFields_) {
        for (auto* field : visibleFields_) field->SetClass("shown", false);
        for (auto* field : pendingFields_) field->SetClass("shown", true);
        visibleFields_.swap(pendingFields_);
        uiDirty_ = true;
    }
    pendingFields_.clear();
    if (hasOverlay) render_.drawUi(uiDirty_);
    drawUiLayers(!hasOverlay || !uiDirty_);
    uiDirty_ = false;
}

void Cabinet::drawUiLayers(bool updateLayout) {
    if (uiLayers_.empty()) return;
    if (updateLayout) document_->GetContext()->Update();
    for (auto* element : uiLayers_) drawUiElement(*element);
    uiLayers_.clear();
}

void Cabinet::uiImage(Rml::Element& element, std::string_view path) {
    element.SetAttribute("data-image", std::string(path));
}

void Cabinet::uiLayer(const char* id, std::string_view path) {
    if (!document_) return;
    auto& element = *document_->GetElementById(id);
    if (!path.empty()) uiImage(element, path);
    useField(element);
}

void Cabinet::drawUiElement(Rml::Element& element, float x, float y, float scale) {
    // RmlUi provides the unrounded layout. Draw these layers into the same
    // framebuffer as AFP to retain the original filtering and blend order.
    const auto local = uiBounds(element);
    const Rect bounds{x + local.x * scale, y + local.y * scale, local.w * scale, local.h * scale};
    if (const auto* polygon = element.GetAttribute("data-polygon")) {
        std::istringstream coordinates(polygon->Get<std::string>());
        std::vector<Vec2> points;
        for (float px, py; coordinates >> px >> py;)
            points.push_back({bounds.x + px * scale, bounds.y + py * scale});
        render_.polygon(points, uiColor(element, "data-color"));
        return;
    }
    if (const auto* image = element.GetAttribute("data-image")) {
        const auto path = image->Get<std::string>();
        if (!path.empty() && bounds.w > 0 && bounds.h > 0)
            render_.image(path, bounds, uiColor(element, "data-color"));
        return;
    }
    if (element.GetTagName() == "img") {
        render_.image(element.GetAttribute<std::string>("src", "").substr(3), bounds);
        return;
    }
    if (element.HasAttribute("data-background")) {
        const auto fill = element.IsClassSet("meter") ? Cyan : element.IsClassSet("highlight") ? Green : Glass;
        render_.rect(bounds, uiColor(element, "data-color", fill));
        return;
    }
    if (const auto* baseline = element.GetAttribute("data-baseline")) {
        const auto& style = element.GetComputedValues();
        const auto align = style.text_align() == Rml::Style::TextAlign::Center ? TextAlign::Center
            : style.text_align() == Rml::Style::TextAlign::Right ? TextAlign::Right : TextAlign::Left;
        const float textX = x + (local.x + (align == TextAlign::Center ? local.w * .5f : align == TextAlign::Right ? local.w : 0)) * scale;
        const float textY = y + (local.y + baseline->Get<float>()) * scale;
        auto font = TextFont::Body;
        const auto family = style.font_family();
        for (const auto candidate : {TextFont::Display, TextFont::MaruGothicW4, TextFont::Digital, TextFont::MaruGothicW4Fixed})
            if (SDL_strcasecmp(family.c_str(), Render::fontFamily(candidate)) == 0) { font = candidate; break; }
        const auto c = style.color();
        const Color color = element.IsClassSet("binding-prompt") ? Green
            : element.IsClassSet("bpm") ? Cyan
            : element.IsClassSet("receipt-text") ? ReceiptText
            : element.IsClassSet("receipt-amount") ? ReceiptAmount
            : Color{c.red / 255.f, c.green / 255.f, c.blue / 255.f, c.alpha / 255.f};
        const auto& value = static_cast<const Rml::ElementText&>(*element.GetChild(0)).GetText();
        const float stretch = element.GetAttribute<float>("data-scale-x", 1);
        if (stretch != 1) render_.pushTransform({stretch, 0, -textX * (stretch - 1), 0, 1, 0});
        const int outline = element.GetAttribute<int>("data-outline", 0);
        render_.text(value, textX, textY, style.font_size() * scale, uiColor(element, "data-color", color), align,
                     element.HasAttribute("data-unlimited") ? 0.f : bounds.w, font, outline,
                     uiColor(element, "data-outline-color", {0, 0, 0, 1}));
        if (stretch != 1) render_.popTransform();
        return;
    }
    const bool clip = element.GetComputedValues().overflow_x() == Rml::Style::Overflow::Hidden;
    if (clip) render_.pushClip(bounds);
    for (int i = 0; i < element.GetNumChildren(); ++i) drawUiElement(*element.GetChild(i), x, y, scale);
    if (clip) render_.popClip();
}

void Cabinet::drawScene(double elapsed) {
    switch (page_) {
    case Page::Title:
        render_.afp("title_original", "title", "in", elapsed, true);
        field("title-credit", "CREDIT: " + std::to_string(credits_));
        break;
    case Page::Pin: drawPin(elapsed); break;
    case Page::Room: drawRoom(now_ - roomStarted_); break;
    case Page::Generator: case Page::Valkyrie: drawGenerator(elapsed); break;
    case Page::Crew: drawCrew(elapsed); break;
    case Page::Caution: {
        art("bg_iseki", Screen);
        const double closing = render_.afpDuration("shutter_nabla", "shutter_default", "in");
        const bool covered = elapsed >= closing;
        // Keep the logo and center plates underneath the caution AFP so that
        // its last frames reveal the same shutter that opens onto the wheel.
        render_.afp("shutter_nabla", "shutter_default", covered ? "loop" : "in",
                    covered ? elapsed - closing : elapsed, covered);
        render_.afp("caution_original", "caution", "0", elapsed);
        if (sessionPaidWithPaseli_)
            field("caution-paseli", "PASELI: " + std::to_string(paseli_));
        field("caution-credit", "CREDIT: " + std::to_string(credits_));
        break;
    }
    case Page::Mode: drawMode(elapsed); break;
    case Page::Skill: drawSkill(elapsed); break;
    case Page::Credit: drawCredit(elapsed); break;
    case Page::Songs: drawSongs(elapsed); break;
    case Page::Matching: drawMatching(elapsed); break;
    case Page::Result:
        drawResult(elapsed);
        if (elapsed < resultShineDuration_ && !characterView_ && !(held_[FXL] && held_[FXR]))
            render_.afp("result_grade", resultShine_, resultShineClip(), elapsed, false, Screen);
        break;
    case Page::Summary: drawSummary(elapsed); break;
    case Page::Exit: drawExit(elapsed); break;
    case Page::Saved: {
        art("bg_iseki", Screen);
        const auto duration = render_.afpDuration("session_dialog", "win_connect", "in");
        render_.afp("session_dialog", "win_connect", elapsed < duration ? "in" : "in_end", elapsed);
        if (elapsed >= duration)
            field("saved", "プレーデータを保存しました");
        break;
    }
    case Page::GameEnd: {
        const auto intro = render_.afpDuration("entry_flow", "game_over_all", "in");
        render_.afp("entry_flow", "game_over_all", elapsed < intro ? "in" : "loop",
                    elapsed < intro ? elapsed : elapsed - intro, elapsed >= intro);
        const double artwork = elapsed - 850;
        if (artwork >= 0)
            render_.afp("entry_flow", "2025nabla", artwork < 2500 ? "in" : "300", artwork);
        break;
    }
    case Page::Settings: drawSettings(elapsed); break;
    case Page::Stage: break;
    }
}

void Cabinet::drawPin(double) {
    art("bg_iseki", Screen);
    const bool covered = transition_ && transition_->next == Page::Pin;
    const double elapsed = now_ - entryAt_;
    const bool deciding = entryNext_ && now_ < entryOutAt_;
    const bool leaving = entryNext_ && !deciding;
    if (!covered && (entryStep_ == EntryStep::CardRead || entryStep_ == EntryStep::Loading
            || entryStep_ == EntryStep::PaymentCheck || entryStep_ == EntryStep::PaymentNotice)) {
        const bool closing = entryOutAt_ >= 0;
        const auto folder = entryFolder();
        const auto movie = entryMovie();
        const double intro = render_.afpDuration(folder, movie, "in");
        Bindings text;
        if (folder == "entry_wait") text.emplace("text_usr", entryStep_ == EntryStep::Loading
            ? ImageBinding{"entry_wait/txt_wait.png", {}, {221,88,638,24}} : ImageBinding{""});
        render_.afp(folder, movie, closing ? "out" : elapsed < intro ? "in" : "in_end",
                    closing ? now_ - entryOutAt_ : elapsed, false, Screen, text);
        if (!closing && elapsed >= intro) {

            if (entryStep_ == EntryStep::CardRead || entryStep_ == EntryStep::PaymentCheck) {
                field("entry-confirmation", entryStep_ == EntryStep::CardRead ? "確認しました！" : "PASELI確認中。");
            } else if (entryStep_ == EntryStep::PaymentNotice) {
                field("entry-notice-1", EntryNotices[0]);
                field("entry-notice-2", EntryNotices[1]);
                field("entry-notice-3", EntryNotices[2]);
                field("entry-notice-4", EntryNotices[3]);
                field("entry-notice-5", EntryNotices[4]);
                field("entry-notice-6", EntryNotices[5]);
            }
            flushLabels();
        }
    } else if (!covered) {
        const auto movie = entryMovie();
        const double intro = render_.afpDuration(entryFolder(), movie, "in");
        const bool opening = elapsed < intro;
        const auto clip = leaving ? "out" : opening ? "in" : "in_end";
        const double clipTime = leaving ? now_ - entryOutAt_ : opening ? elapsed : elapsed - intro;
        Bindings bindings;
        const auto glyph = [](char c) -> std::string {
            const auto name = c == ' ' ? "sp" : c == '!' ? "exc" : c == '?' ? "ques" : c == '#' ? "sharp"
                : c == '$' ? "dollar" : c == '&' ? "and" : c == '*' ? "ast" : c == '.' ? "dot" : c == '-' ? "bar" : "";
            return "entry_original/name_moji_" + (name[0] ? std::string(name) : lower(std::string(1,c))) + ".png";
        };
        const auto selectedGlyph = nameIndex_ < int(NameCharacters.size()) ? glyph(NameCharacters[nameIndex_])
            : nameIndex_ == int(NameCharacters.size()) ? "entry_original/name_moji_back.png" : "entry_original/name_moji_ok.png";
        bindings["name_selection"] = selectedGlyph;
        bindings["profile"] = ImageBinding{"runtime/entry-profile", {0,500,1080,-500}};
        render_.afp(entryFolder(), movie, clip, clipTime, false, Screen, bindings);
        if (!leaving && !opening) {
            std::string code = cardId_;
            if (code.size() == 16) {
                if (!held_[FXL]) code.replace(4,8,"XXXXXXXX");
                for (int at=12; at>0; at-=4) code.insert(at," ");
            }
            switch (entryStep_) {
            case EntryStep::Registration:
                ap("entry_original", "confirmation_new", deciding ? (entryFocus_ ? "decide_transfer" : "decide_new") : entryFocus_ ? "transfer_loop" : "new_loop", 125,804,deciding ? now_ - entryOutAt_ + 420 : elapsed-intro,!deciding);
                break;
            case EntryStep::Terms:
                ap("session_dialog", "yes_no", entryFocus_ ? "no_loop" : "yes_loop", 0,1233,elapsed-intro,true);
                break;
            case EntryStep::Transfer:
                field("transfer-code", code);
                break;
            case EntryStep::Code:
                field("entry-code", code);
                break;
            case EntryStep::Profile:
                field("profile-code", code);
                break;
            case EntryStep::Name: {
                constexpr std::array<float,10> centers{125,193,261,329,397,683,751,819,887,955};
                for (int i=0; i<10; ++i) {
                    const int offset = i<5 ? i-5 : i-4;
                    const int index = wrap(nameIndex_+offset,int(NameCharacters.size())+2);
                    const auto image = index<int(NameCharacters.size()) ? glyph(NameCharacters[index])
                        : index==int(NameCharacters.size()) ? "entry_original/name_moji_back.png" : "entry_original/name_moji_ok.png";
                    render_.image(image,{centers[i]-43.2f,847.2f,86.4f,89.6f});
                }
                ap("entry_original","entry_09_scroll","min",90,979,nameIndex_*7500./(NameCharacters.size()+1));
                constexpr std::array<float,8> positions{315,379,444,508,573,637,702,766};
                for (size_t i=0; i<draftName_.size(); ++i)
                    render_.image(glyph(draftName_[i]),{positions[i]-32.4f,1048.4f,64.8f,67.2f});
                const int cursor = std::min(int(draftName_.size()),7);
                const auto size = render_.afpSize("entry_original","name_eff","loop");
                ap("entry_original","name_eff","loop",positions[cursor]-size.x/2,1082-size.y/2,elapsed,true);
                break;
            }
            case EntryStep::Pin: {
                const auto text = entryAuthenticated_ ? (pinPhase_ == PinPhase::Verify ? "txt_correct" : "txt_complete")
                    : !pinError_.empty() ? (pinPhase_ == PinPhase::Verify ? "txt_incorrect" : "txt_retry")
                    : pinPhase_ == PinPhase::Create ? "txt_newnumber" : pinPhase_ == PinPhase::Repeat ? "txt_again" : "txt_pinnumber";
                const auto size = render_.imageSize(std::string("entry_original/")+text+".png");
                render_.image(std::string("entry_original/")+text+".png",{540-size.x/2,685-size.y/2,size.x,size.y});
                Bindings pin;
                const bool submitted = pinVerification_ || entryAuthenticated_;
                for (int digit=1;digit<=4;++digit) {
                    pin.emplace("pin_"+std::to_string(digit),submitted || digit<=int(pin_.size()) ? "entry_original/num_on.png":"entry_original/num_off.png");
                    const bool newFilled=pinPhase_==PinPhase::Repeat || submitted || digit<=int(pin_.size());
                    const bool confirmFilled=submitted || (pinPhase_==PinPhase::Repeat && digit<=int(pin_.size()));
                    pin.emplace("new_"+std::to_string(digit),newFilled?"entry_original/num_on.png":"entry_original/num_off.png");
                    pin.emplace("confirm_"+std::to_string(digit),confirmFilled?"entry_original/num_on.png":"entry_original/num_off.png");
                }
                ap("entry_original",pinPhase_==PinPhase::Verify?"pin_01":"pin_02","0",558,750,0,false,pin);
                field("pin-code", code);
                break;
            }
            default: break;
            }
        }
    }
    header("titletext_title_entry");
    // Registration in the recording has no countdown. Existing-card verification retains its timeout.
    if (!covered && entryStep_ == EntryStep::Pin && pinPhase_ == PinPhase::Verify) footer();
    else field("pin-credit", "CREDIT: " + std::to_string(credits_));
    if (!covered && entryStep_ == EntryStep::PaymentNotice)
        field("pin-paseli", "PASELI: " + std::to_string(paseli_));
    ap("cabinet_timer","operation_card_entry","operation_01",0,1794,0);
}

void Cabinet::drawExit(double elapsed) {
    if (premiumExit()) {
        drawSongs(exitElapsed_);
        flushLabels();
        if (exitDecidedAt_ > 0 && now_ - exitDecidedAt_ >= 420) {
            render_.afp("session_dialog", "win_yes_no", "out", now_ - exitDecidedAt_ - 420);
            return;
        }
    } else art("bg_iseki", Screen);
    const auto duration = render_.afpDuration("session_dialog", "win_yes_no", "in");
    render_.afp("session_dialog", "win_yes_no", elapsed < duration ? "in" : "in_end", elapsed);
    if (elapsed < duration) return;
    field("exit-question", continuing_ ? "コンティニューしますか？" : premiumExit() ? "プレーを終了します。よろしいですか？" : "プレーを終了しますか？");
    const bool deciding = exitDecidedAt_ > 0;
    const double phase = elapsed - duration;
    const auto choiceIn = render_.afpDuration("session_dialog", "yes_no", "choice_in");
    const auto clip = deciding ? (exitFocus_ == 0 ? "decide_yes" : "decide_no")
        : phase < choiceIn ? "choice_in" : exitFocus_ == 0 ? "yes_loop" : "no_loop";
    ap("session_dialog", "yes_no", clip, 0, 961, deciding ? now_ - exitDecidedAt_ : phase,
       !deciding && phase >= choiceIn);
    if (continuing_) footer();
}

void Cabinet::drawGenerator(double elapsed) {
    const bool valkyrie = page_ == Page::Valkyrie;
    {
        render_.afp("generator_vii", "bg_gene", "in_end", 0);
        const auto selector = valkyrie ? "generator_sel" : "app_generator_sel";
        const double intro = render_.afpDuration("generator_vii", selector, "in");
        char banner[64];
        std::snprintf(banner, sizeof(banner), "valkyrie/banners/bn_gene_%02d.png", valkyrieVolume_);
        render_.afp("generator_vii", selector, elapsed < intro ? "in" : "in_end", elapsed, false, Screen,
                    {{"banner",valkyrie ? banner : "generator_vii/appeal-parts.png"}});
        if (elapsed >= intro) {
            if (!valkyrie) {
                field("generator-heading", "アピールパーツジェネレーター 第1弾");
                field("generator-price", std::to_string(GeneratorPrice) + " × " + std::to_string(generatorCount_));
                field("generator-owned", std::to_string(generatedParts_.size()) + "/162");
                field("generator-packets", std::to_string(packets_));
            } else field("valkyrie-heading", "VALKYRIE GENERATOR VOL." + std::to_string(valkyrieVolume_));
        }
        if (!valkyrie && generatorPhase_ == 1) {
            flushLabels();
            const auto phase = now_ - generatorAt_;
            const auto intro = render_.afpDuration("session_dialog", "win_yes_no", "in");
            render_.afp("session_dialog", "win_yes_no", phase < intro ? "in" : "in_end", phase);
            field("generator-question", "PACKETを" + std::to_string(GeneratorPrice * generatorCount_) + "消費して生成を開始します");
            ap("session_dialog", "yes_no", generatorFocus_ ? "no_loop" : "yes_loop", 0, 961, phase, true);
        }
    }
    if (!valkyrie && generatorPhase_ >= 2) {
        flushLabels();
        const auto& [base, image] = generatorRewards_[generatorRewardIndex_];
        render_.afp("generator_vii", generatorPhase_ == 2 ? "anime_gene" : "parts_get", "in", now_ - generatorAt_, false, Screen,
            {{"board_base", base ? image : ""}, {"board_sticker", base ? "" : image}});
    }
    ap("subscreen", valkyrie ? "operation_generator_valkyrie" : "operation_generator", "operation_01", 0,1785,0);
    field("generator-credit", "CREDIT: " + std::to_string(credits_));
}

void Cabinet::header(std::string_view name) {
    ap("menu", "cmn_title", "in_end", 0, 0, 0);
    if (name == "titletext_title_generator") {
        render_.image("entry_flow/txt_generator.png", {0, HeaderTextY, 1082, 42});
        return;
    }
    const auto folder = name == "titletext_track_2nd" || name == "titletext_track_final" ||
        name == "titletext_track_ex" ? "track_header"
        : name == "titletext_title_entry" || name == "titletext_title_total_result" || name == "titletext_track_premium"
        ? "entry_flow" : "menu";
    ap(folder, name, "in_end", 0, HeaderTextY, 0);
}

void Cabinet::footer(bool result) {
    const auto page = lanePreview_ ? settingsReturn_ : premiumExit() ? Page::Songs : page_;
    const double entered = lanePreview_ ? coveredAt_ : entered_;
    const bool practice = practiceUntil_ > 0 && (page == Page::Songs || page == Page::Matching || page == Page::Result);
    double deadline = entered + SongTime;
    switch (page) {
    case Page::Pin: deadline = entered_ + PinTime; break;
    case Page::Mode: case Page::Credit: deadline = modeStarted_ + ModeTime; break;
    case Page::Room: deadline = roomStarted_ + RoomTime; break;
    case Page::Exit: deadline = entered_ + ContinueTime; break;
    case Page::Result: deadline = entered_ + ResultTime; break;
    case Page::Summary:
        deadline = skillTitleAt_ < 0 ? entered_ + SummaryTime :
            skillTitleAt_ + render_.afpDuration("skill_effects/windows", "win_skill_change", "in") + SkillTitleTime;
        break;
    default: break;
    }
    if (practice) deadline = practiceUntil_;
    const int time = static_cast<int>(std::ceil(std::max(0., deadline - now_) / 1000));
    const auto digit=[](int n){return "cabinet_timer/num_time_"+std::to_string(n)+".png";};
    Bindings digits=practice ? Bindings{{"timer_1000_usr",digit(time/600%10)},{"timer_0100_usr",digit(time/60%10)},
        {"timer_0010_usr",digit(time%60/10)},{"timer_0001_usr",digit(time%10)}}
        : Bindings{{"timer_10_usr",digit(time/10%10)},{"timer_01_usr",digit(time%10)}};
    // cmn_sys/time_usr: center(540,1770), origin(540,150). The original
    // nested transforms retain the stagger, 10-degree tilt and drop shadow.
    ap("cabinet_timer",practice?"p_time":"time","loop_timer",0,1620,now_-entered,true,digits);
    field("footer-credit", "CREDIT: " + std::to_string(credits_));
    if (page != Page::Pin)
        field("footer-paseli", "PASELI: " + std::to_string(paseli_));
    if (result) ap("cabinet", "operation_result", "operation_01", 0, 1785, 0);
}

void Cabinet::drawRoom(double elapsed) {
    drawRoomArt();
    const auto movie = std::array{"room_0", "room_1", "room_2"}[roomFocus_];
    constexpr auto folder = "room_flow";
    const auto panel = std::array{"panel_02_usr", "panel_03_usr", "panel_04_usr"}[roomFocus_];
    const bool departing = roomDepartureAt_ >= 0;
    auto clip = "in";
    double time = elapsed;
    if (departing) {
        const auto age = now_ - roomDepartureAt_;
        const auto decide = render_.afpDuration(folder, "cursor", "decide");
        clip = age < decide ? "decide" : "out";
        time = age < decide ? age : age - decide;
    }
    render_.afp(folder, movie, clip, time, !departing);
    // The original starts both movies together and positions the cursor from
    // the current panel transform (180512213–1805122b0, 180512a58).
    const auto selected = render_.afpAnchor(folder, movie, clip, panel);
    if (!departing || time < render_.afpDuration(folder, "cursor", clip))
        ap(folder, "cursor", clip, selected.position.x - 300, selected.position.y - 300, time, !departing);
    const auto caption = render_.afpAnchor(folder, movie, clip, "setsumei_usr");
    if (caption.opacity > 0) {
        constexpr std::array descriptions{"アピールパーツを生成できます", "マイルームを終了してゲームを始めます",
                                          "カスタマイズアイテムを生成できます"};
        render_.text(descriptions[roomFocus_], caption.position.x, caption.position.y + 7, 22,
                     {1, 1, 1, caption.opacity}, TextAlign::Center, 500, TextFont::MaruGothicW4);
    }
    if (!departing && !lanePreview_) footer();
    else field("room-credit", "CREDIT: " + std::to_string(credits_));
    ap("subscreen", "operation_my_room", "operation_01", 0,1785,0);
    header("titletext_my_room");
}

void Cabinet::drawRoomArt() {
    art("bg_iseki", Screen);
    navigator_.draw(CrewScene::Room);
}

void Cabinet::drawCrew(double elapsed) {
    if (roomTab_ == 4) {
        drawAchievements(elapsed);
        drawSubscreenHeader(true);
        return;
    }
    if (roomTab_ == 2 || roomTab_ == 3) {
        drawCustomization(elapsed);
        drawSubscreenHeader(true);
        return;
    }
    if (roomTab_ != 1) {
        render_.afp("crew_selector/background", "bg", "0", now_ - roomStarted_, true, CrewPanel);
        render_.afp("crew_selector", "bg_sub_myroom", "0", 0, false, CrewPanel);
        const auto name = roomPageName();
        const auto intro = render_.afpDuration("subscreen", name, "in");
        const bool leaving = crewLeavingAt_ >= 0;
        render_.afp("subscreen", name, leaving ? "out" : elapsed < intro ? "in" : "in_end",
                    leaving ? now_ - crewLeavingAt_ : elapsed, false, CrewPanel);
        drawSubscreenHeader(true);
        return;
    }
    const auto intro = render_.afpDuration("crew_selector", "myroom_crew", "in");
    const bool leaving = crewLeavingAt_ >= 0;
    const auto phase = leaving ? "out" : elapsed < intro ? "in" : "in_end";
    const auto time = leaving ? now_ - crewLeavingAt_ : elapsed;
    const auto& crew = crewInfo(crewPreview_);
    const auto& families = crewFamilies();
    const int current = crewFamilyIndex();
    constexpr auto focus = "runtime/crew-focus";
    constexpr auto idleFocus = "runtime/crew-focus-idle";
    const double focusElapsed = std::max(0., now_ - crewChangedAt_);
    const double focusIntro = render_.afpDuration("crew_selector", "select_crew", "in");
    const auto drawFocus = [&](std::string_view target, std::string_view clip, double time) {
        render_.captureImage(target, 170, 170, [&] {
            render_.afp("crew_selector", "select_crew", clip, time, false, {0, 0, 170, 170});
        });
    };
    if (!render_.imageSize(idleFocus).x) drawFocus(idleFocus, "in_end", 0);
    if (focusElapsed < focusIntro) drawFocus(focus, "in", focusElapsed);
    render_.afp("crew_selector/background", "bg", "0", now_ - roomStarted_, true, CrewPanel);
    render_.afp("crew_selector", "bg_sub_myroom", "0", 0, false, CrewPanel);
    Bindings bindings{{"portrait", crew.preview}, {"name", ImageBinding{crew.nameImage,{.5f,.5f,279,39}}}};
    for (int slot = 0; slot < 25; ++slot) {
        const auto key = "icon_" + std::to_string(slot + 1);
        const int index = crewOffset_ + slot;
        bindings.emplace(key, index < int(families.size())
            ? ImageBinding{crewInfo(index == current ? crewPreview_ : families[index].styles.front()).icon,
                           {.5f,.5f,125,125}} : ImageBinding{});
        bindings.emplace("focus_" + key, index == current
            ? ImageBinding{focusElapsed < focusIntro ? focus : idleFocus, {0, 170, 170, -170}, {0, 0, 170, 170}}
            : ImageBinding{});
    }
    const auto& styles = families[current].styles;
    const int count = styles.size();
    const bool odd = count % 2 != 0;
    for (const bool oddSlots : {true, false}) {
        const int slots = oddSlots ? 5 : 6;
        for (int slot = 0; slot < slots; ++slot) {
            const int index = slot - (slots - count) / 2;
            const bool visible = oddSlots == odd && index >= 0 && index < count;
            const auto key = std::string(oddSlots ? "style_odd" : "style_even") + std::to_string(slot + 1);
            bindings.emplace(key, visible ? ImageBinding{crewInfo(styles[index]).icon, {.5f,.5f,125,125}}
                                          : ImageBinding{});
            bindings.emplace("focus_" + key, visible && styles[index] == crewPreview_
                ? ImageBinding{idleFocus, {0, 170, 170, -170}, {0, 0, 170, 170}} : ImageBinding{});
        }
    }
    const bool changed = crewPreview_ != selectedCrew_;
    for (int button = 0; button < 2; ++button) {
        const auto name = button == 0 ? "button_cancel" : "button_save";
        const auto bounds = Rect{button == 0 ? 526.f : 786.f,903,270,90};
        const bool enabled = changed && (button == 0 || crew.installed);
        const bool pressed = enabled && finger_ && touchStart_.x >= bounds.x && touchStart_.x < bounds.x + bounds.w
            && touchStart_.y >= bounds.y && touchStart_.y < bounds.y + bounds.h;
        const bool released = crewButton_ == button && crewButtonAt_ >= 0
            && now_ - crewButtonAt_ < render_.afpDuration("crew_selector", name, "release_disabled");
        const auto clip = leaving || elapsed < intro ? "disabled" : pressed ? "press"
            : released ? "release_disabled" : enabled ? "idle" : "disabled";
        const auto target = std::string("runtime/crew-") + name + "-" + clip;
        if (released || !render_.imageSize(target).x)
            render_.captureImage(target, 270, 90, [&] {
                render_.afp("crew_selector", name, clip, released ? now_ - crewButtonAt_ : 0,
                            false, {0, 0, 270, 90});
            });
        bindings.emplace(name, ImageBinding{target, {0, 90, 270, -90}, {0, 0, 270, 90}});
    }
    render_.afp("crew_selector", "myroom_crew", phase, leaving || elapsed < intro ? time : elapsed - intro,
                false, CrewPanel, bindings);
    if (elapsed >= intro && !leaving && crew.nameImage.empty())
        field("crew-name", crew.name);
    drawSubscreenHeader(true);
}

void Cabinet::drawSubscreenHeader(bool room) {
    render_.afp("subscreen", "common_frame", "0", 0, false, CrewPanel,
                {{"tenkey_mini_4b57c137ff", ""}});
    if (document_) useField(*document_->GetElementById("subscreen-back-panel"));
    field("subscreen-back-icon", "‹");
    field("subscreen-back-label", "返回");
    flushLabels();
    constexpr std::array roomTabs{"tab_home", "tab_crew", "tab_apicard", "tab_apiboard", "tab_achive"};
    constexpr std::array playTabs{"tab_home", "tab_option", "tab_search", "tab_achive"};
    for (int tab = 0; tab < (room ? int(roomTabs.size()) : int(playTabs.size())); ++tab)
        render_.afp("subscreen", room ? roomTabs[tab] : playTabs[tab],
                    tab == (room ? roomTab_ : playTab_) ? "selected" : "idle", 0, false,
                    {131.f + 254 * tab, 23, 250, 90});
    if (std::any_of(achievements_.begin(), achievements_.end(), [&](const auto& a) { return achievementReady(a); }))
        render_.afp("subscreen", "pop_new", "loop", now_ - entered_, true, {room ? 1339.f : 1085.f,7,70,50});
}

void Cabinet::drawAchievements(double elapsed) {
    render_.afp("crew_selector/background", "bg", "0", now_ - roomStarted_, true, CrewPanel);
    const double intro = render_.afpDuration("subscreen", "page_achievement", "in");
    render_.afp("subscreen", "page_achievement", elapsed < intro ? "in" : "in_end", elapsed, false, CrewPanel);
    if (elapsed < intro) return;
    render_.afp("subscreen", "folder_select", "in_end", 0, false, {0,271,560,760});
    std::array<int, 28> totals{}, completed{};
    for (const auto& achievement : achievements_) {
        ++totals[achievement.category];
        if (std::find(claimedAchievements_.begin(),claimedAchievements_.end(),achievement.id) != claimedAchievements_.end()) ++completed[achievement.category];
    }
    const auto percent = [&](int category) { return totals[category] ? completed[category]*100/totals[category] : 0; };
    field("achievement-name", achievementCategories_[achievementCategory_]);
    field("achievement-progress", "達成進捗：" + std::to_string(percent(achievementCategory_)) + "%");
    render_.afp("subscreen", "progress_bar", "min", percent(achievementCategory_)*1000./120, false, {781.4f,265.15f,894,26});
    flushLabels();
    render_.pushClip({0,379,530,650});
    for (int index = achievementCategoryOffset_; index < std::min(achievementCategoryOffset_+9, int(AchievementFolders.size())); ++index) {
        const int category = AchievementFolders[index];
        const float y = 379 + (index-achievementCategoryOffset_)*80.f;
        render_.afp("subscreen", "fuda_folder", "in_end", 0, false, {0,y,510,80});
        if (category == achievementCategory_) render_.afp("subscreen", "cursor_l", "in_end", 0, false, {0,y+8,470,74});
        rowField("achievement-categories", index - achievementCategoryOffset_, 0, achievementCategories_[category]);
        rowField("achievement-categories", index - achievementCategoryOffset_, 1, std::to_string(percent(category)) + "%");
    }
    flushLabels();
    render_.popClip();
    render_.pushClip({580,331,1310,690});
    int index = 0;
    for (const auto& achievement : achievements_) {
        if (achievement.category != achievementCategory_) continue;
        const int line = index++ - achievementOffset_;
        if (line < 0 || line >= 9) continue;
        const float y = 346 + line*80.f;
        render_.afp("subscreen", "fuda_ticket", "in_end", 0, false, {584,y,1500,70});
        rowField("achievement-rows", line, 0, achievement.title);
        if (achievement.rewardType != 1) rowField("achievement-rows", line, 1, std::to_string(achievement.reward));
        const bool claimed = std::find(claimedAchievements_.begin(),claimedAchievements_.end(),achievement.id) != claimedAchievements_.end();
        const bool ready = achievementReady(achievement);
        // The two original AFP wrapper names point at the opposite bitmap.
        render_.afp("subscreen", claimed ? "button_list_notyet" : ready ? "button_list_get" : "button_list_finish",
                    ready ? "release" : "0", 0, false, {1628,y+10,180,50});
    }
    flushLabels();
    render_.popClip();
}

void Cabinet::drawProfile(Rect bounds, bool draft) {
    const int style = draft ? draftStyle_ : boardStyle_;
    const auto& base = boardBases_[draft ? draftBase_ : boardBase_];
    const float scale = bounds.w / 960;
    render_.afp("result_profile", "nameplate", "0", 0, false, bounds,
                {{"nameplate_background", base.image}, {"nameplate_frame", ""}, {"stamp_canvas", ""}});
    for (const auto& sticker : draft ? draftStickers_ : placedStickers_) {
        const auto& art = boardStickers_[sticker.index];
        render_.image(art.image, {bounds.x + (330 + sticker.column * StickerCell) * scale,
            bounds.y + (30 + sticker.row * StickerCell) * scale,
            art.columns * StickerCell * scale, art.rows * StickerCell * scale});
    }
    drawPlayerCard({bounds.x+30*scale,bounds.y+30*scale,300*scale,300*scale}, style, draft);
}

void Cabinet::drawPlayerCard(Rect bounds, int style, bool draft) {
    auto card = playerCard(draft);
    card.style = style;
    drawProfileCard(render_, bounds, card);
}

PlayerCard Cabinet::playerCard(bool draft) const {
    return {playerName_, appealCards_[draft ? draftCard_ : appealCard_].image, selectedCrew_,
            draft ? draftStyle_ : boardStyle_, page_ == Page::Result
                ? (forceDone_ ? forceAfter_ : forceBefore_) : totalForce(),
            skillLevel_, skillTitle_};
}

static std::string skillPanel(int level, int name = 0) {
    if (level == 0 && name == 0) return "menu/art/skill_back_panel_none.png";
    char path[64];
    std::snprintf(path,sizeof(path),"skill_selector/skill_back_panel_%02d.png",level);
    return path;
}

static std::string_view skillName(int name) {
    // The skill-panel name IDs and authored strings are from 180135500.
    constexpr std::array<std::string_view, 15> names{
        "", "岳 翔", "流 星", "月 衝", "瞬 光", "天 極", "烈 風", "雷 電",
        "麗 華", "魔騎士", "剛力羅", "或帝滅斗", "暴龍天", "音戯探偵", "風 雲",
    };
    return names.at(name);
}

static void drawSkillName(Render& render, int name, Rect bounds) {
    if (name == 0) return;
    const float fontSize = 40 * bounds.h / 170;
    render.text(skillName(name), bounds.x + bounds.w * (169.f / 300), bounds.y + bounds.h * .5f + fontSize * .32f,
                fontSize, White, TextAlign::Center, bounds.w, TextFont::MaruGothicW4);
}

static void playProfileCard(Render& render, Rect bounds, const PlayerCard& card) {
    render.afp("result_profile", card.style ? "profile_card_02_gachi" : "profile_card_01_enjoy", "0", 0, false,
        bounds,
        {{"player_name",""},{"player_title",""},{"player_location",""},{"player_force",""},
         {"player_skill",ImageBinding{skillPanel(card.skillLevel, card.skillTitle),{}, {37,53,226,64}}},
         {"appeal_card",ImageBinding{card.appealImage,{}, {40,40,150,192}}},
         {"crew_portrait",crewInfo(card.crew).preview}});
}

void drawProfileCard(Render& render, Rect bounds, const PlayerCard& card) {
    const float scale = bounds.w / 300;
    playProfileCard(render, bounds, card);
    render.text("よろしくお願いします", bounds.x+10*scale, bounds.y+19*scale, 19*scale,
                White, TextAlign::Left, 280*scale, TextFont::MaruGothicW4);
    render.text(card.name,bounds.x+150*scale,bounds.y+53*scale,30*scale,White,TextAlign::Center,254*scale,TextFont::Display);
    drawSkillName(render, card.skillTitle, {bounds.x+37*scale, bounds.y+53*scale,226*scale,64*scale});
    const int force = card.force;
    const int band = forceBand(force);
    char emblem[64], value[24];
    std::snprintf(emblem,sizeof(emblem),"result_force/em6_s%02d_i_eab.png",band/4+1);
    const Rect widget{bounds.x+2*scale,bounds.y+206*scale,166*scale,70*scale};
    render.afp("result_force","force_playdate_s","0",0,false,widget);
    // force_icon_usr's child is a runtime-only empty AFP slot.
    render.image(emblem,{widget.x+2.98f*scale,widget.y+2.57f*scale,62.04f*scale,52.64f*scale});
    render.afp("result_force",std::string(band < 24 ? "star_silver_" : "star_gold_")+std::to_string(band%4+1),"0",0,false,
        {widget.x+3*scale,widget.y+51.5f*scale,62*scale,18.8f*scale});
    std::snprintf(value,sizeof(value),"%d.%03d",force/1000,force%1000);
    render.text(value,widget.x+116*scale,widget.y+58*scale,20*scale,White,TextAlign::Center,94*scale,TextFont::Display);
}

void Cabinet::drawResultProfile(const char* id) {
    if (!document_) return;
    auto& profile = *document_->GetElementById(id);
    auto& layout = *document_->GetElementById("result-profile");
    const auto card = playerCard();
    const auto band = forceBand(card.force);
    char emblem[64], value[24];
    std::snprintf(emblem, sizeof(emblem), "result_force/em6_s%02d_i_eab.png", band / 4 + 1);
    std::snprintf(value, sizeof(value), "%d.%03d", card.force / 1000, card.force % 1000);

    auto& name = *layout.QuerySelector(".profile-name");
    auto& title = *layout.QuerySelector(".profile-skill-name");
    auto& total = *layout.QuerySelector(".profile-force-value");
    auto& icon = *layout.QuerySelector(".profile-force-icon");
    setUiText(name, card.name);
    if (card.skillTitle) setUiText(title, skillName(card.skillTitle));
    setUiText(total, value);
    uiImage(icon, emblem);
    document_->GetContext()->Update();

    const auto destination = uiBounds(profile);
    const float scale = destination.w / uiBounds(layout).w;
    const auto project = [&](Rml::Element& element) {
        const auto local = uiBounds(element);
        return Rect{destination.x + local.x * scale, destination.y + local.y * scale, local.w * scale, local.h * scale};
    };

    render_.afp("result_profile", "nameplate", "0", 0, false, destination,
                {{"nameplate_background", boardBases_[boardBase_].image}, {"nameplate_frame", ""}, {"stamp_canvas", ""}});
    const auto stickers = uiBounds(*layout.QuerySelector(".profile-stickers"));
    for (const auto& sticker : placedStickers_) {
        const auto& image = boardStickers_[sticker.index];
        render_.image(image.image, {destination.x + (stickers.x + sticker.column * StickerCell) * scale,
            destination.y + (stickers.y + sticker.row * StickerCell) * scale,
            image.columns * StickerCell * scale, image.rows * StickerCell * scale});
    }
    playProfileCard(render_, project(*layout.QuerySelector(".profile-card")), card);
    drawUiElement(*layout.QuerySelector(".profile-greeting"), destination.x, destination.y, scale);
    drawUiElement(name, destination.x, destination.y, scale);
    if (card.skillTitle) drawUiElement(title, destination.x, destination.y, scale);
    render_.afp("result_force", "force_playdate_s", "0", 0, false, project(*layout.QuerySelector(".profile-force")));
    drawUiElement(icon, destination.x, destination.y, scale);
    render_.afp("result_force", std::string(band < 24 ? "star_silver_" : "star_gold_") + std::to_string(band % 4 + 1),
                "0", 0, false, project(*layout.QuerySelector(".profile-force-stars")));
    drawUiElement(total, destination.x, destination.y, scale);
}

void Cabinet::drawCustomization(double elapsed) {
    render_.afp("crew_selector/background", "bg", "0", now_ - roomStarted_, true, CrewPanel);
    render_.afp("crew_selector", "bg_sub_myroom", "0", 0, false, CrewPanel);
    const auto name = roomPageName();
    const double intro = render_.afpDuration("subscreen", name, "in");
    const bool leaving = crewLeavingAt_ >= 0;
    render_.afp("subscreen", name, leaving ? "out" : elapsed < intro ? "in" : "in_end",
                leaving ? now_ - crewLeavingAt_ : elapsed, false, CrewPanel,
                {{"profile_preview",ImageBinding{"runtime/profile-preview",{0,360,960,-360}}}});
    if (leaving || elapsed < intro) return;
    Bindings items;
    if (roomTab_ == 2) {
        render_.afp("subscreen","button_B_categoly01","0",0,false,{1510,180,180,50});
        render_.afp("subscreen","button_B_favorit",favoriteCardsOnly_?"on":"off",0,false,{1694,180,180,50});
        field("card-random", "ランダム");
        field("card-favorites", "お気に入り");
        flushLabels();
        for (int slot = 0; slot < 25; ++slot) {
            const int index = cardOffset_ + slot;
            ImageBinding thumbnail;
            if (index < int(visibleCards_.size())) {
                const auto& card = appealCards_[visibleCards_[index]];
                thumbnail = {card.thumbnail, card.thumbnailUv, {0,0,150,192}};
            }
            items.emplace("card_" + std::to_string(slot + 1), std::move(thumbnail));
        }
        render_.pushClip({1072,276,850,738});
        render_.afp("subscreen", "window_apica", "in_end", 0, false, {1072,246,850,774}, items);
        const auto current = std::find(visibleCards_.begin(), visibleCards_.end(), draftCard_);
        const int cell = int(current - visibleCards_.begin()) - cardOffset_;
        if (current != visibleCards_.end() && cell >= 0 && cell < 20)
            render_.afp("subscreen", "select_item", "in_end", 0, false,
                {1112.f + (cell%5)*144,269.f + (cell/5)*182,150,200});
        render_.popClip();
        render_.afp("subscreen", "date_apica", "0", 0, false, {86,640,960,250});
        const auto& card = appealCards_[draftCard_];
        field("card-name", card.name);
        field("card-illustrator", card.illustrator);
        for (int message = 0; message < 8; ++message)
            rowField("card-messages", message, 0, card.messages[message]);
    } else {
        constexpr std::array tabs{"button_A_style", "button_A_platebg", "button_A_sticker"};
        for (int tab = 0; tab < 3; ++tab)
            render_.afp("subscreen",tabs[tab],tab == decorationTab_ ? "press" : "release",0,false,
                        {953.f + tab*270,134,270,50});
        render_.pushClip({1072,276,850,738});
        if (decorationTab_ == 1) {
            // The 320x120 thumbnail replaces the texture in the original 960x360 AFP slot.
            constexpr Rect boardSlot{0, 0, 960, 360};
            for (int slot = 0; slot < 3; ++slot) {
                const int index = decorationOffset_ + slot;
                items["board_" + std::to_string(slot + 1)] = index < int(boardBases_.size())
                    ? ImageBinding{boardBases_[index].thumbnail, {}, boardSlot} : ImageBinding{};
            }
            render_.afp("subscreen", "window_board", "in_end", 0, false, {1072,280,850,774}, items);
            const int cell = draftBase_ - decorationOffset_;
            if (cell >= 0 && cell < 3) render_.afp("subscreen", "select_item_board", "in_end", 0, false,
                {1105,303.f+cell*280,740,296});
        } else if (decorationTab_ == 0) {
            for (int style = 0; style < 2; ++style) {
                drawPlayerCard({style ? 1440.f : 1128.f,320,300,300}, style, true);
            }
        } else {
            for (int cell = 0; cell < 25; ++cell) {
                const int index = decorationOffset_ + cell;
                items["sticker_" + std::to_string(cell + 1)] = index < int(boardStickers_.size()) ? boardStickers_[index].thumbnail : "";
            }
            render_.afp("subscreen", "window_item", "in_end", 0, false, StickerWindow, items);
            for (int cell = 0; cell < 20 && decorationOffset_ + cell < int(boardStickers_.size()); ++cell) {
                const int index = decorationOffset_ + cell;
                if (index == stickerFocus_ || std::any_of(draftStickers_.begin(), draftStickers_.end(),
                        [&](const auto& sticker) { return sticker.index == index; })) {
                    render_.afp("subscreen", "select_item", "in_end", 0, false,
                        {1112.f + (cell % 5) * 144, 269.f + (cell / 5) * 182, 150, 200});
                }
            }
        }
        flushLabels();
        render_.popClip();
        if (decorationTab_ == 2 && activeSticker_ >= 0) {
            const auto& sticker = draftStickers_[activeSticker_];
            const auto& art = boardStickers_[sticker.index];
            const float x = StickerCanvas.x + sticker.column * StickerCell;
            const float y = StickerCanvas.y + sticker.row * StickerCell;
            const float w = float(art.columns * StickerCell), h = float(art.rows * StickerCell);
            render_.rect({x, y, w, 2}, White);
            render_.rect({x, y + h - 2, w, 2}, White);
            render_.rect({x, y, 2, h}, White);
            render_.rect({x + w - 2, y, 2, h}, White);
            render_.afp("subscreen", "date_stiker", "0", 0, false, {86,640,960,250});
            field("sticker-name", art.name);
            flushLabels();
        }
    }
    const bool changed = roomTab_ == 2 ? draftCard_ != appealCard_ : draftStyle_ != boardStyle_
        || draftBase_ != boardBase_ || draftStickers_ != placedStickers_;
    for (int button = 0; button < 2; ++button)
        render_.afp("subscreen", button ? "button_save" : "button_cancel", changed ? "idle" : "disabled",0,false,
                    {526.f+260*button,903,270,90});
    if (cardFilterOpen_) {
        flushLabels();
        render_.afp("subscreen","filter_apica","0",0,false,CardFilterPanel);
        for (int index = 0; index < int(CardFilterOptions.size()); ++index) {
            const auto [bit, title] = CardFilterOptions[index];
            const auto bounds = cardFilterOptionBounds(index);
            render_.afp("subscreen","button_categoly",draftCardFilter_ & bit ? "0" : "off",0,false,bounds);
            rowField("card-filter-labels", index, 0, title);
        }
    }
}

void Cabinet::prepareModeArtwork() {
    for (int row = 0; row < int(ModeLetters.size()); ++row) {
        int low = 0, high = 0, creditPrice = 0;
        // FRIEND advertises the same cabinet rates as NORMAL.
        for (const auto& offer : modePayments(row == 4 ? NormalMode : row)) {
            if (offer.paseli) {
                low = low ? std::min(low, offer.price) : offer.price;
                high = std::max(high, offer.price);
            } else {
                creditPrice = offer.price;
            }
        }
        std::string amount = std::to_string(low);
        if (low != high) amount += "～" + std::to_string(high);
        render_.captureImage("runtime/mode-credit-" + std::to_string(row), ModePriceWidth * 2, ModePriceHeight * 2, [&] {
            // 180257610 uses font 12, at 25 px in choice_usr and 20 px in the list.
            for (int compact = 0; compact < 2; ++compact) {
                const float y = ModePriceHeight * float(compact);
                const float fontSize = compact ? 20.f : 25.f;
                const Color ink = compact ? Color{.788f,.788f,.788f,1} : White;
                if (low)
                    render_.text(amount + " 円", 0, y + 27, fontSize, ink,
                        TextAlign::Left, 0, TextFont::MaruGothicW4, 1, {0,0,0,1});
                if (creditPrice)
                    render_.text(std::to_string(creditPrice) + " CREDIT(S)", ModePriceWidth, y + 27, fontSize, ink,
                        TextAlign::Left, 0, TextFont::MaruGothicW4, 1, {0,0,0,1});
            }
        });
        render_.queueImage("menu/art/6_panel_s_title01" + std::string(1, ModeLetters[row]) + ".png");
        render_.queueImage("menu/art/6_panel_s_text01" + std::string(1, ModeLetters[row]) + ".png");
        render_.queueImage("menu/art/6_panel_title01" + std::string(1, ModeLetters[row]) + ".png");
        render_.queueImage("mode_flow/description_" + std::to_string(ModeTexts[row]) + ".png");
    }
    for (auto movie : {"mode_select", "mode_current"})
        render_.queueAfp("mode_flow", movie, "in");
    audio_.prepareSample("entry_flow/mode.wav");
    modeLabelsReady_ = true;
}

void Cabinet::drawMode(double) {
    drawRoomArt();
    // Captured framebuffer textures have their origin at the bottom.
    const auto price = [](int row, bool compact, bool credit) {
        return ImageBinding{"runtime/mode-credit-" + std::to_string(row),
            {credit ? float(ModePriceWidth) : 0, compact ? float(ModePriceHeight) : float(ModePriceHeight * 2),
             ModePriceWidth, -ModePriceHeight}, {0, 0, ModePriceWidth, ModePriceHeight}};
    };
    if (!modeLabelsReady_) prepareModeArtwork();
    Bindings modes;
    for (const int slot : {1, 2, 4, 5}) {
        const int row = wrap(modeIndex_ + slot - 3, ModeCount);
        const auto suffix = std::to_string(slot);
        modes["title_" + suffix] = "menu/art/6_panel_s_title01" + std::string(1, ModeLetters[row]) + ".png";
        modes["description_" + suffix] = "menu/art/6_panel_s_text01" + std::string(1, ModeLetters[row]) + ".png";
        modes["paseli_" + suffix] = price(row, true, false);
        modes["credit_" + suffix] = price(row, true, true);
    }
    modes["selected_title"] = "menu/art/6_panel_title01" + std::string(1, ModeLetters[modeIndex_]) + ".png";
    modes["selected_description"] = "mode_flow/description_" + std::to_string(ModeTexts[modeIndex_]) + ".png";
    modes["selected_paseli"] = price(modeIndex_, false, false);
    modes["selected_credit"] = price(modeIndex_, false, true);
    const auto outAt = page_ == Page::Credit ? paymentModeOutAt() : -1;
    const bool departing = outAt >= 0 && now_ >= outAt;
    const double intro = render_.afpDuration("mode_flow", "mode_select", "in");
    const double elapsed = now_ - modeStarted_;
    const bool entering = !departing && elapsed < intro;
    const auto phase = departing ? "out" : entering ? "in" : "loop";
    const double age = departing ? now_ - outAt : entering ? elapsed : elapsed - intro;
    render_.afp("mode_flow", "mode_select", phase, age, !departing && !entering, Screen, modes);
    render_.afp("mode_flow", "mode_current", phase, age, !departing && !entering, Screen, modes);
    if (page_ != Page::Credit) {
        footer();
        ap("payment", "operation_mode_credit", "operation_01", 0, 1792, 0);
        header(entering ? "titletext_title_entry" : "titletext_modesel");
    }
}

void Cabinet::drawCredit(double elapsed) {
    drawMode(elapsed);
    const double intro = render_.afpDuration("payment", "paseli_credit", "in");
    const bool leaving = paymentAcceptedAt_ >= 0;
    const double phase = leaving ? now_ - paymentAcceptedAt_ : elapsed;
    if (!leaving || phase < render_.afpDuration("payment", "paseli_credit", "out"))
        render_.afp("payment", "paseli_credit", leaving ? "out" : elapsed < intro ? "in" : "loop", phase);
    if (paymentReceiptAt_ >= 0) {
        constexpr auto folder = "entry_checkin";
        constexpr auto movie = "win_paseli_green";
        const double age = now_ - paymentReceiptAt_;
        const bool closing = paymentReceiptOutAt_ >= 0;
        const double closeAge = now_ - paymentReceiptOutAt_;
        const bool opening = age < render_.afpDuration(folder, movie, "in");
        if (!closing || closeAge < render_.afpDuration(folder, movie, "out"))
            render_.afp(folder, movie, closing ? "out" : opening ? "in" : "in_end", closing ? closeAge : age);
        if (!closing && !opening) {
            field("receipt-title", "PASELIを使用しました。");
            field("receipt-label-0", "使用");
            field("receipt-value-0", std::to_string(paymentReceiptPrice_));
            field("receipt-unit-0", "円");
            field("receipt-label-1", "残高");
            field("receipt-value-1", std::to_string(paseli_));
            field("receipt-unit-1", "円");
            flushLabels();
        }
    }
    footer();
    ap("payment", "operation_mode_credit", "operation_01", 0, 1792, 0);
    if (leaving || elapsed < intro) {
        header("titletext_modesel");
        return;
    }
    if (document_) {
        auto& list = *document_->GetElementById("payment-labels");
        const auto offers = modePayments(modeIndex_);
        const int count = static_cast<int>(offers.size());
        for (int row = 0; row < list.GetNumChildren() - 1; ++row) {
            auto& item = *list.GetChild(row);
            const auto image = [&](const char* selector, std::string_view name) {
                auto& element = *item.QuerySelector(selector);
                uiImage(element, "menu/art/" + std::string(name) + ".png");
                useField(element);
            };
            const auto text = [&](const char* selector, std::string_view value) {
                auto& element = *item.QuerySelector(selector);
                setUiText(element, value);
                useField(element);
            };
            if (row < count) {
                const auto& offer = offers[row];
                image(".payment-plate", offer.paseli ? "6_pay_fuda_paseli" : "6_pay_fuda_credit");
                image(".payment-title", offer.titleArt);
                if (modeIndex_ == PremiumMode) {
                    text(".payment-premium-one", "10分間レベル制限無く楽曲が遊べます。");
                    text(".payment-premium-two", "「演奏途中終了機能」や「リトライ機能」が使えます。");
                } else if (modeIndex_ == NormalMode || modeIndex_ == SkillMode) {
                    const bool standard = offer.titleArt == "6_pay_text_01b";
                    text(".payment-description", modeIndex_ == SkillMode ? "コースを選択してプレーできます"
                        : standard ? "必ず 3 曲プレーできます" : "1～3 曲プレーできます");
                    useField(*item.QuerySelector(".payment-detail-one"));
                    image(".payment-detail-two", standard ? "6_pay_text_08c" : "6_pay_text_08d");
                    image(".payment-detail-three", standard ? "6_pay_text_08e" : "6_pay_text_08f");
                }
                const int balance = offer.paseli ? paseli_ : credits_;
                const auto amount = offer.paseli ? std::to_string(offer.price)
                    : std::to_string(credits_) + "/" + std::to_string(offer.price);
                text(balance >= offer.price ? ".payment-amount" : ".payment-amount-disabled", amount);
                image(".payment-unit", offer.paseli ? "6_pay_text_03" : "6_pay_text_04");
            } else image(".payment-plate", "6_pay_fuda_non");
            if (creditFocus_ != count && row == creditFocus_) useField(*item.QuerySelector(".payment-focus"));
        }
        auto& back = *list.GetChild(list.GetNumChildren() - 1);
        useField(*back.QuerySelector(".payment-plate"));
        useField(*back.QuerySelector(".payment-back"));
        if (creditFocus_ == count) useField(*back.QuerySelector(".payment-focus"));
        drawUiLayers();
    }
    header("titletext_modesel");
}

void Cabinet::drawSkill(double elapsed) {
    render_.afp("skill_selector","bg_skill","normal",elapsed,true);
    if (elapsed < render_.afpDuration("skill_effects", "in_skill_sel", "in")) {
        render_.afp("skill_effects", "in_skill_sel", "in", elapsed);
        return;
    }
    const auto& selected=courses_[courseIndex_];
    field("course-set", selected.set);
    constexpr std::array<float,5> tops{326,548,775,1135,1357};
    for(int offset=-2;offset<=2;++offset) {
        const int index=(courseIndex_+offset+int(courses_.size()))%int(courses_.size());
        const auto& course=courses_[index];
        const bool current=offset==0;
        const float x=current?30.f:85.f, y=tops[offset+2];
        const auto& first=songs_[course.tracks.front().first].charts[course.tracks.front().second].track;
        Bindings bindings{{"jacket",first.asset(first.jacket)}};
        for(int i=0;i<4;++i) bindings["level_"+std::to_string(i+1)]=i<int(course.tracks.size())
            ? "song_row/level_small_"+lower(songs_[course.tracks[i].first].charts[course.tracks[i].second].track.label)+".png" : "";
        render_.afp("skill_selector",current?"big_frame":"small_frame","in_end",0,false,
            {x,y,current?1020.f:910.f,current?370.f:240.f},bindings);
        rowField("course-labels", offset + 2, 0, course.name);
        for(int track=0;track<int(course.tracks.size());++track) {
            const auto& data=songs_[course.tracks[track].first].charts[course.tracks[track].second].track;
            rowField("course-labels", offset + 2, track + 1, data.title);
            level(data.level,{x+(current?361.f:292.f),y+(current?91.f:58.f)+track*(current?37:30),55,22},true);
        }
    }
    footer();
    ap("menu","cmn_title","in_end",0,0,0);
    ap("skill_selector","titletext_skill_analyzer","in_end",0,HeaderTextY,0);
    ap("skill_selector","operation_skill_analyzer","operation_01",0,1785,0);
}

void Cabinet::radar(const Chart& selected, Vec2 center, float radius, bool labels) {
    constexpr std::array<Color, 6> colors{
        Color{.50f, 1, .81f, 1}, Color{1, .40f, .45f, 1}, Color{.95f, .40f, .81f, 1},
        Color{1, .88f, .32f, 1}, Color{.50f, .52f, 1, 1}, Color{.47f, 1, .64f, 1}
    };
    std::array<Vec2, 6> points{};
    const float scale = radius / 70;
    art(labels ? "radar_track_bg" : "radar_player_bg", {center.x - 121 * scale, center.y - 121 * scale, 242 * scale, 242 * scale});
    for (size_t i = 0; i < points.size(); ++i) {
        const float angle = (static_cast<float>(i) / 3 - .5f) * std::numbers::pi_v<float>;
        const float x = std::cos(angle) * radius, y = std::sin(angle) * radius;
        const float axis = std::clamp(selected.radar[i] / 200.f, 0.f, 1.f);
        points[i] = {center.x + x * axis, center.y + y * axis};
    }
    for (size_t i = 0; i < points.size(); ++i) {
        const size_t next = (i + 1) % points.size();
        const auto a = colors[i];
        const auto b = colors[next];
        const std::array<Vec2, 3> sector{center, points[i], points[next]};
        render_.polygon(sector, {(a.r + b.r) * .5f, (a.g + b.g) * .5f, (a.b + b.b) * .5f, .68f});
    }
    const float diameter = (labels ? 202.f : 242.f) * scale;
    art(labels ? "radar_track_top" : "radar_player_top_line",
        {center.x - diameter / 2, center.y - diameter / 2, diameter, diameter});
}

void Cabinet::level(int tenths, Rect bounds, bool selected) {
    const auto value = levelString(tenths);
    const float digitWidth = bounds.h * .52f;
    const float width = digitWidth * (tenths % 10 ? 2.74f : 2.f);
    float x = bounds.x + (bounds.w - width) / 2;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '.') {
            render_.rect({x, bounds.y + bounds.h * .85f, digitWidth * .23f, bounds.h * .13f}, White);
            x += digitWidth * .28f;
        } else {
            const bool small = i > 0 && value[i - 1] == '.';
            render_.image("menu/art/num_level_" + std::string(1, value[i]) + ".png",
                          {x, bounds.y + (small ? bounds.h * .27f : 0), digitWidth * (small ? .72f : 1),
                           bounds.h * (small ? .73f : 1)}, selected ? White : Color{1, 1, 1, .86f});
            x += digitWidth;
        }
    }
}

static void drawDifficultyDigits(Render& render, int tenths, Rect bounds, bool panel) {
    const auto digit=[](int n){return "next_area/num_level_"+std::to_string(n)+".png";};
    const bool decimal=tenths>=180 || tenths%10!=0;
    const Bindings digits=decimal ? Bindings{{"num_100_usr",digit(tenths/100)},
        {"num_010_usr",digit(tenths/10%10)},{"num_001_usr",digit(tenths%10)}}
        : Bindings{{"num_10_usr",digit(tenths/100)},{"num_01_usr",digit(tenths/10%10)}};
    render.afp("song_row",panel ? decimal?"panel_num_3":"panel_num_2" : decimal?"sel_num_3":"sel_num_2",
               "0",0,false,bounds,digits);
}

void drawLevelBadge(Render& render, std::string_view difficulty, int tenths, Rect bounds) {
    render.image("song_row/level_sel_"+lower(difficulty)+".png",bounds);
    drawDifficultyDigits(render, tenths, bounds, false);
}

void drawHorizontalLevel(Render& render, const Selection& track, Rect bounds, float numberScaleY) {
    const float scale = bounds.w / 200;
    const float baseline = bounds.y + 34 * scale;
    render.image("song_row/level_small_" + lower(track.label) + ".png", bounds);
    render.pushTransform({1, 0, 0, 0, numberScaleY, baseline * (1 - numberScaleY)});
    render.text(levelString(track.level), bounds.x+129*scale, baseline, 24*scale,
                White, TextAlign::Center, 96*scale, TextFont::Display);
    render.popTransform();
}

void Cabinet::drawNextArea(double elapsed, bool leaving) {
    const auto& track = chart().track;
    const auto bpm = bpmString(track);
    constexpr int atlasWidth = 660, atlasHeight = 292;
    constexpr auto atlas = "runtime/next-area";
    constexpr Rect difficulty{0, 182, 110, 110};
    struct Field {
        const char* slot;
        std::string_view text;
        Rect region;
        float baseline, size;
        TextFont font = TextFont::MaruGothicW4;
        float offsetY = 0;
    };
    const std::array fields{
        Field{"title", track.title, {0, 0, 660, 44}, 36, 36},
        Field{"artist", track.artist, {0, 46, 660, 36}, 28, 22},
        Field{"effect", track.charter, {0, 84, 520, 32}, 23, 16},
        Field{"illust", track.illustrator, {0, 118, 520, 32}, 23, 16},
        Field{"bpm", bpm, {0, 152, 80, 28}, 26, 22, TextFont::Display, -7}
    };
    if (!nextAreaLabelsReady_) {
        render_.captureImage(atlas, atlasWidth, atlasHeight, [&] {
            for (const auto& field : fields) {
                const auto& r = field.region;
                render_.text(field.text, r.w / 2, r.y + field.baseline, field.size,
                             White, TextAlign::Center, r.w, field.font);
            }
            drawLevelBadge(render_, track.label, track.level, difficulty);
        });
        nextAreaLabelsReady_ = true;
    }
    const bool finalTrack = activeCourse_ ? track_ == int(courses_[*activeCourse_].tracks.size()) : track_ >= 3;
    Bindings art{{"jacket_usr", track.asset(track.jacket)},
        {"track", sessionType_ == StartType::Premium ? "next_area/track_premium_i_eeb.png"
            : exTrack() ? "next_area/track_ex_i_eeb.png"
            : finalTrack ? "next_area/track_final_i_eeb.png"
            : "next_area/track_0" + std::to_string(track_) + "_i_eeb.png"}};
    // The original loading timeline animates the bound fields along with their panels.
    for (const auto& field : fields) {
        const auto& r = field.region;
        art[std::string(field.slot) + "_usr"] = ImageBinding{atlas, {r.x, atlasHeight - r.y, r.w, -r.h},
                                       {0, field.offsetY, r.w, r.h}};
    }
    art["difficulty_usr"] = ImageBinding{atlas, {0, atlasHeight - difficulty.y, difficulty.w, -difficulty.h}};
    if (!leaving) render_.image("menu/art/bg_iseki.png", Screen);
    const double intro = render_.afpDuration("next_area", "loading", "loading_in");
    const bool holding = !leaving && elapsed >= intro;
    render_.afp("next_area", "loading", leaving ? "loading_out" : holding ? "loading_loop" : "loading_in",
                holding ? elapsed - intro : elapsed, holding, Screen, art);
}

void Cabinet::drawBlasterGauge(int energy, double elapsed, bool result) {
    if (!document_) return;
    auto& slot = *document_->GetElementById(result ? "result-blaster-slot" : "songs-blaster-slot");
    auto& gauge = *document_->GetElementById("blaster-gauge");
    auto& fill = *gauge.QuerySelector(".blaster-fill");
    auto& gloss = *gauge.QuerySelector(".blaster-gloss");
    auto& caption = *gauge.QuerySelector(".blaster-caption");
    auto& shadow = *gauge.QuerySelector(".blaster-caption-shadow");
    const float amount = fill.GetAttribute<float>("data-fill-width", 300) * energy / 10000;
    uiPixels(fill, Rml::PropertyId::Width, amount);
    uiPixels(gloss, Rml::PropertyId::Width, amount);
    const int percent = energy / 10;
    const std::string value = percent == 1000 ? "100%" :
        std::to_string(percent / 10) + "." + std::to_string(percent % 10) + "%";
    const float captionTravel = percent == 1000 ? 0.f : std::min(amount, 241.f);
    uiPixels(caption, Rml::PropertyId::MarginLeft, captionTravel);
    uiPixels(shadow, Rml::PropertyId::MarginLeft, captionTravel);
    setUiText(caption, value);
    setUiText(shadow, value);
    document_->GetContext()->Update();

    const auto bounds = uiBounds(slot);
    const float scale = bounds.w / uiBounds(gauge).w;
    const bool pass = blasterPassActive();
    render_.afp("ex_track", "gauge_blaster", "0", 0, false, bounds);
    render_.pushTransform({scale, 0, bounds.x, 0, scale, bounds.y});
    drawUiElement(*gauge.QuerySelector(".blaster-rim"));
    drawUiElement(*gauge.QuerySelector(".blaster-interior"));
    if (amount > 0) {
        const auto clip = uiBounds(fill);
        render_.pushClip({bounds.x + clip.x * scale, bounds.y + clip.y * scale, clip.w * scale, clip.h * scale});
        for (int i = 0; i < fill.GetNumChildren(); ++i) drawUiElement(*fill.GetChild(i));
        render_.popClip();
    }
    if (pass) drawUiElement(*gauge.QuerySelector(result ? ".blaster-result-icon" : ".blaster-song-icon"));
    drawUiElement(shadow);
    drawUiElement(caption);
    render_.popTransform();
    if (pass) {
        const auto effect = uiBounds(*gauge.QuerySelector(".blaster-effect"));
        render_.afp(result ? "result_details" : "ex_track", "bpass_r_ef", "loop", elapsed, true,
                    {bounds.x + effect.x * scale, bounds.y + effect.y * scale, effect.w * scale, effect.h * scale});
    }
}

void Cabinet::drawSongs(double elapsed) {
    const bool songFocused=wheel_==Wheel::Songs;
    const auto& selected = chart();
    const auto& track = selected.track;
    render_.afp("menu", "bg_room", "60", elapsed, true);
    const auto best = record(track);
    Bindings digits{{"cursor_level_4c75cdfe4e", ""}};
    if (!songFocused || song().charts.size() <= 4) digits["tex_arrow_ultlevel_238f6acdae"] = "";
    if (!options_.exScore) {
        for (const auto* slot : {"box_score_max_ms_0e94613ae2", "box_score_ex_ms_f14192046d",
                                "text_maxper_dot_de17a059d3", "text_maxper_1b04e1179b"}) digits[slot] = "";
    }
    int score = best.score;
    for (int i = 0; i < 8; ++i) {
        auto& digit = digits["best_" + std::to_string(i)];
        digit.path = "hud/num_score_" + std::to_string(score % 10) + ".png";
        if (i > 0 && score == 0) digit.tint = ScoreLeadingZero;
        score /= 10;
    }
    if (songFocused) {
    render_.pushClip({0, 145, 541, 648});
    render_.image(track.asset(track.jacket), {-98, 72, 742, 742}, {.71f, .80f, .73f, .77f});
    render_.popClip();
    }
    if (!songFocused) for (int i=0;i<8;++i) digits["best_"+std::to_string(i)]="";
    render_.afp("menu", "ms_sel", "in_end", 0, false, Screen, digits);
    // ms_sel/gauge_blaster_usr is centered at (187,1589) in its 340x80 frame.
    drawBlasterGauge(blasterEnergy_, elapsed, false);
    if (sessionType_ == StartType::Premium)
        render_.image("ex_track/pt_playend.png", {0,1627,502,108});
    if (songFocused) {
    render_.image(track.asset(track.jacket), {95, 243, 350, 350});
    radar(selected, {431, 704}, 62);
    art("mark_" + outcomeAsset(best.outcome), {347, 822, 56, 56});
    art("grade_" + gradeAsset(best.grade), {410, 822, 56, 56});
    field("songs-title", track.title);
    field("songs-artist", track.artist);
    field("songs-bpm", bpmString(track));
    field("songs-charter", track.charter);
    field("songs-illustrator", track.illustrator);
    flushLabels();
    const auto [begin, end] = chartRange(song(), exChallengeFolder());
    for (int i = 0; i < int(song().charts.size()); ++i) {
        const auto& row = song().charts[i].track;
        const float x = 43 + 114.5f * i;
        const bool focused = i == chartIndex_;
        if (focused) render_.image("menu/cursor_level_4c75cdfe4e.png", {x - 16, 1046, 144, 122});
        const Rect badge{x, 1058, 110, 110};
        render_.image("song_row/level_panel_" + lower(row.label) + ".png", badge);
        drawDifficultyDigits(render_, row.level, badge, true);
        if (i < begin || i >= end) render_.rect({x+10,1058,90,96}, {0,0,0,.54f});
    }
    } else render_.image("selection_ui/sel_dmjk.png",{95,243,350,350});
    const int songs=activeFolder_<0?0:int(visibleSongs_.size());
    const auto [position, count] = songListPosition();
    constexpr Rect wheelClip{504,147,Width-504,1575};
    render_.pushClip(wheelClip);
    for (int offset=std::max(-6,-(count-1)/2);offset<=std::min(6,count/2);++offset) {
        const float y=RowTop+RowHeight*offset;
        if (y + RowHeight <= wheelClip.y || y >= wheelClip.y + wheelClip.h) continue;
        const int index=wrap(position+offset,count);
        if (activeFolder_>=0 && index>activeFolder_ && index<=activeFolder_+songs) {
            const int rowIndex=index-activeFolder_-1;
            const auto& item=songs_[visibleSongs_[rowIndex]];
            const auto& row=item.charts[visibleCharts_[rowIndex]<0?displayChart(item):visibleCharts_[rowIndex]].track;
            const auto entry=record(row);
            ap("song_row","panel_music","60",RowX,y,0,false,{{"jacket",row.asset(row.jacket)}});
            art("mark_"+outcomeAsset(entry.outcome),{RowX+264,y+20,114,114});
            art("grade_"+gradeAsset(entry.grade),{RowX+385,y+20,114,114});
            if (exChallengeFolder()) {
                render_.rect({RowX+10,y+5,523,164}, {1.f,.49f,.11f,.25f});
                render_.rect({RowX+186,y+7,336,19}, {.17f,.075f,.025f,.88f});
                render_.text("EXTRACK CHALLENGE対象楽曲",RowX+354,y+22,13,
                    {1.f,.48f,.16f,1},TextAlign::Center,326,TextFont::MaruGothicW4);
            }
            drawLevelBadge(render_,row.label,row.level,{RowX,y+75,99,99});
            rowField("wheel-labels", offset + 6, 0, row.title);
            if (std::find(favoriteSongs_.begin(),favoriteSongs_.end(),row.id)!=favoriteSongs_.end())
                render_.image("selection_ui/icon_favorite.png",{RowX+485,y+8,42,42});
        } else {
            const int folderIndex=index-(activeFolder_>=0 && index>activeFolder_?songs:0);
            const auto& folder=songFolders_[folderIndex];
            Bindings color;
            if (folder.kind == FolderKind::ExChallenge) {
                auto& panel = color["panel_mainfolder_71a29b0776"];
                panel.path = "selection_ui/panel_mainfolder_71a29b0776.png";
                panel.sourceRect = {1,1,540,174};
                panel.tint = {1,.25f,.44f,1};
            }
            render_.afp("selection_ui",folder.child?"panel_folder_sub":"panel_folder","in_end",0,false,{RowX,y,540,174},color);
            render_.image("selection_ui/panel_folder_hole.png",{RowX,y,540,174},
                folder.kind == FolderKind::ExChallenge ? Color{1,.28f,.32f,.5f} : Color{1,1,1,.5f});
            constexpr std::array marks{"", "crash", "comp", "comp_ex", "comp_ultra", "uc", "puc", "puc"};
            constexpr std::array grades{"", "d", "c", "b", "a", "a_plus", "aa", "aa_plus", "aaa", "aaa_plus", "s"};
            const auto [lamp, grade] = folder.badges[preferredChart_];
            // mark_usr and grade_usr are centered at (71,127) and (147,127) with .28 scale.
            if (lamp > 0) art(std::string("mark_") + marks[lamp], {RowX+29,y+85,84,84});
            if (grade > 0) art(std::string("grade_") + grades[grade], {RowX+105,y+85,84,84});
            rowField("wheel-labels", offset + 6, 1, folder.name);
            rowField("wheel-labels", offset + 6, 2, std::to_string(folder.count));
        }
    }
    flushLabels();
    render_.afp("menu","cursor","60",0);
    render_.popClip();
    if (document_) {
        const float frame = std::floor(songScrollFrame_ < 0.f ? 1000.f * position / count : songScrollFrame_);
        auto& rail = *document_->GetElementById("song-scroll-rail");
        auto& lamp = *document_->GetElementById("song-scroll-lamp");
        // ms_sel/bar advances 1044 px over 1000 authored frames.
        uiPixels(lamp, Rml::PropertyId::MarginTop, frame * 1.044f);
        useField(rail);
        if (const auto hint = songScrollLabel(); !hint.empty()) {
            auto& caption = *document_->GetElementById("song-scroll-caption");
            auto& text = *caption.QuerySelector(".scroll-caption-text");
            const auto size = text.GetComputedValues().font_size();
            const auto maxWidth = text.GetBox().GetSize().x;
            const float width = std::min(maxWidth, render_.textWidth(hint, size, TextFont::MaruGothicW4)) + 4.f;
            // 1802892d5 offsets captions whose padded width is at least 46 px.
            const float offset = width >= 46.f ? 18.f : 0.f;
            uiPixels(caption, Rml::PropertyId::Width, width);
            uiPixels(caption, Rml::PropertyId::MarginTop, frame - offset);
            uiPixels(caption, Rml::PropertyId::MarginRight, -offset);
            setUiText(text, hint);
            useField(caption);
        }
        flushLabels();
    }
    header(sessionType_ == StartType::Premium ? "titletext_track_premium"
           : exTrack() ? "titletext_track_ex"
           : track_ == 1 ? "titletext_track_1st" : track_ == 2 ? "titletext_track_2nd" : "titletext_track_final");
    art("box_category_sort", {9, 100, 338, 112});
    art("panel_main", {10, 99, 304, 50});
    art("panel_sort", {767, 99, 304, 50});
    art("text_sort", {836, 141, 218, 34});
    const auto& folder = songFolders_[songFocused ? activeFolder_ : wheelIndex_];
    const std::string_view mainFolder = folder.child
        ? songFolders_[folderIndex(folderGroup_, folderGroupValue_)].name : folder.name;
    const std::string_view subFolder = folder.child ? std::string_view(folder.name) : std::string_view{};
    field("songs-parent-folder", mainFolder);
    if (!subFolder.empty()) {
        art("panel_sub", {44, 143, 304, 50});
        field("songs-child-folder", subFolder);
    }
    field("songs-sort", SongSorts[sort_]);
    render_.image("entry_checkin/box_pc.png", {18,1745,190,40});
    render_.image("entry_checkin/box_bloc.png", {223,1745,190,40});
    field("songs-packets", std::to_string(packets_));
    field("songs-blocks", std::to_string(blocks_));
    if (!lanePreview_) footer();
    ap("menu", "operation_ms_sel", "operation_01", 0, 1794, 0);
    if (sortOpen_ || songConfirm_) { flushLabels(); drawSongOverlay(); }
}

void Cabinet::drawSongOverlay() {
    const auto movie=sortOpen_?"sort":"window_select";
    const double elapsed=now_-songOverlayAt_;
    const double intro=render_.afpDuration("selection_ui",movie,"in");
    const bool leaving=songOverlayOutAt_>=0;
    render_.afp("selection_ui",movie,leaving?"out":elapsed<intro?"in":"in_end",
        leaving?now_-songOverlayOutAt_:elapsed,false,Screen,
        sortOpen_?Bindings{}:Bindings{{"jacket",chart().track.asset(chart().track.jacket)}});
    if (leaving || elapsed<intro) return;
    if (sortOpen_) {
        if (document_) {
            auto* list = document_->GetElementById("sort-options");
            uiLayers_.push_back(list->GetChild(sortFocus_)->GetChild(1));
            flushLabels();
        }
        for (int row = 0; row < int(SongSorts.size()); ++row)
            rowField("sort-options", row, 0, SongSorts[row]);
        return;
    }
    const auto& track=chart().track;
    field("confirm-title", track.title);
    field("confirm-artist", track.artist);
    const bool light = sessionType_ == StartType::Light;
    const auto rate = options_.ultra && !light ? (options_.ars ? "panel_ultra_ars" : "panel_ultra")
        : options_.excessive ? (options_.ars && !light ? "panel_excessive_ars" : "panel_excessive") : "panel_effective";
    flushLabels();
    render_.image(std::string("selection_ui/")+rate+".png",{461,819,254,36});
    const auto [begin, end] = chartRange(song(), exChallengeFolder());
    for (int i=begin;i<end;++i) {
        const auto& item=song().charts[i].track;
        const Rect box{470+125.f*i,888,110,110};
        if (i==chartIndex_) render_.image("menu/cursor_level_4c75cdfe4e.png",{box.x-16,box.y-6,144,122});
        drawLevelBadge(render_,item.label,item.level,box);
    }
}

void Cabinet::drawMatching(double elapsed) {
    render_.afp("menu", "bg_room", "60", elapsed, true);
    render_.afp("matching", "ready", "matching_in", elapsed, true);
    const bool departing = matchingDeparture();
    render_.pushClip({0, 0, Width, characterView_ || departing ? Height : 1320.f});
    navigator_.draw(CrewScene::Matching);
    render_.popClip();
    if (departing) {
        const auto age = now_ - transition_->started;
        if (age < transition_->inMs) drawMatchingPanel(age, true);
        if (age < render_.afpDuration("matching", "hispeed", "hispeed_out"))
            render_.afp("matching", "hispeed", "hispeed_out", age);
        return;
    }
    if (characterView_) {
        ap("matching", "skip", "skip_loop", 440, 1309, elapsed, true);
        return;
    }
    drawMatchingPanel(elapsed, false);
    const auto speedIntro = render_.afpDuration("matching", "hispeed", "hispeed_in");
    render_.afp("matching", "hispeed", elapsed < speedIntro ? "hispeed_in" : "hispeed_loop",
                elapsed < speedIntro ? elapsed : elapsed - speedIntro, elapsed >= speedIntro);
    field("matching-bpm", bpmString(chart().track));
    field("matching-speed", optionLabel(Option::Speed));
    field("matching-credit", "CREDIT: " + std::to_string(credits_));
}

void Cabinet::drawMatchingPanel(double elapsed, bool departing) {
    const auto& track = chart().track;
    const bool finalTrack = activeCourse_ ? track_ == int(courses_[*activeCourse_].tracks.size()) : track_ >= 3;
    const std::string trackArt = exTrack() ? "hud/hud_track_05.png"
        : track_ == 3 && !finalTrack ? "hud/hud_track_03.png"
        : "menu/art/hud_track_0" + std::to_string(finalTrack ? 4 : track_) + ".png";
    constexpr auto atlas = "runtime/matching-card";
    constexpr int atlasHeight = 570;
    constexpr Rect difficulty{0, 362, 200, 50};
    struct SongField {
        const char* slot;
        std::string_view value;
        Rect region;
        float baseline;
    };
    const std::array fields{
        SongField{"song_title", track.title, {0, 414, 470, 32}, 26},
        SongField{"song_artist", track.artist, {0, 448, 470, 30}, 24},
        SongField{"song_effector", track.charter, {0, 480, 264, 24}, 22},
        SongField{"song_illustrator", track.illustrator, {0, 506, 264, 24}, 21}
    };
    constexpr Rect bpm{0, 532, 120, 28};
    if (!matchingCardReady_) {
        render_.captureImage(atlas, 960, atlasHeight, [&] {
            drawProfile({0, 0, 960, 360}, false);
            drawHorizontalLevel(render_, track, difficulty, .75f);
            // 1803660e0 uses Maru Gothic W4 at 22 for the four song fields.
            for (const auto& field : fields)
                render_.text(field.value, 0, field.region.y + field.baseline, 22,
                             White, TextAlign::Left, 0, TextFont::MaruGothicW4);
            constexpr float baseline = bpm.y + 24;
            render_.pushTransform({1, 0, 0, 0, .8f, baseline * .2f});
            render_.text(bpmString(track), bpm.w / 2, baseline, 22,
                         White, TextAlign::Center, bpm.w, TextFont::Display);
            render_.popTransform();
        });
        matchingCardReady_ = true;
    }
    const auto binding = [&](Rect region) {
        return ImageBinding{atlas, {region.x, atlasHeight - region.y, region.w, -region.h},
                            {0, 0, region.w, region.h}};
    };
    Bindings panel{{"song_jacket", track.asset(track.jacket)}, {"level_yoko", binding(difficulty)},
        {"song_bpm", binding(bpm)},
        {"track_label", sessionType_ == StartType::Premium ? "menu/art/track_premium_i_eeb.png" : trackArt}};
    for (const auto& field : fields) panel.emplace(field.slot, binding(field.region));
    if (departing) {
        // The song background includes SKIP. mat_comp fades the panel before mat_out.
        render_.afp("matching", "maching_ue", "mat_comp", elapsed, false, Screen, panel);
        return;
    }
    const auto matchingIn = render_.afpDuration("matching", "maching_ue", "mat_in");
    const bool matchingIntro = elapsed < matchingIn;
    render_.afp("matching", "maching_ue", matchingIntro ? "mat_in" : "mat_loop",
                matchingIntro ? elapsed : elapsed - matchingIn, !matchingIntro, Screen, panel);
    if (!matchingIntro) {
        const double intro = render_.afpDuration("matching", "big_2_ready", "in");
        // maching_ue places the local big_2_ready player window at (0, 901).
        render_.afp("matching", "big_2_ready", elapsed < intro ? "in" : "in_end", elapsed, false, {0, 901, 1080, 500},
            {{"nameplate", binding({0, 0, 960, 360})}, {"level", binding(difficulty)}});
    }
}

void Cabinet::drawResult(double elapsed) {
    const auto& track = sessionResults_.back().selection;
    uiLayer("result-background");
    uiLayer("result-grid");
    drawUiLayers();
    navigator_.draw(CrewScene::Result);
    if((characterView_ || (held_[FXL] && held_[FXR])) && (forceAt_ < 0 || forceDone_)) { footer(true); return; }
    Bindings data{{"rate_title",activeCourse_ ? "result_original/txt_permissive_rate.png" :
        result_.ultra ? "result_original/txt_ultra_rate.png" :
        result_.excessive ? "result_original/txt_excessive_rate.png" : "result_original/txt_effective_rate.png"},
        {"notes_option",options_.mirror?"result_original/txt_mirror.png":""},
        {"song_title",""},{"song_artist",""},{"level",""},{"jacket",track.asset(track.jacket)},
        {"grade","result_original/grade_"+gradeAsset(result_.grade)+".png"},{"grade_effect",""}};
    const std::array<std::pair<const char*,int>,13> counters{{
        {"crit_chip",result_.objectCounts[0][0]},{"near_chip",result_.objectCounts[0][1]},
        {"error_chip",result_.objectCounts[0][2]},{"crit_long",result_.objectCounts[1][0]},
        {"crit_vol",result_.objectCounts[2][0]},{"error_long",result_.objectCounts[1][2]},
        {"error_vol",result_.objectCounts[2][2]},{"maximum",result_.maxChain},
        {"early_error",result_.timingCounts[0]},{"early_near",result_.timingCounts[1]},
        {"crit",result_.timingCounts[2]},{"late_near",result_.timingCounts[3]},{"late_error",result_.timingCounts[4]}
    }};
    for(const auto& [name,count]:counters) {
        int value=count;
        for(int digit=0;digit<4;++digit) {
            data.emplace(std::string(name)+"_"+std::to_string(digit),digit>0 && value==0 ? ""
                : "result_original/num_score_small_"+std::to_string(value%10)+".png");
            value/=10;
        }
    }
    const auto intro=render_.afpDuration("result_original","result_all_default","in");
    render_.afp("result_original","result_all_default",elapsed<intro?"in":"loop",elapsed,false,Screen,data);
    if(elapsed<ResultGradeAt) { footer(true); return; }
    const bool newBest = !result_.autoPlay && result_.score > previousBest_;
    Bindings scores{{"best_sign",""},{"diff_sign",result_.score<previousBest_?"result_original/num_score_minus.png":"result_original/num_score_plus.png"}};
    for(const auto& [name,number]:std::array<std::pair<const char*,int>,3>{{{"score",result_.score},{"best",previousBest_},{"diff",std::abs(result_.score-previousBest_)}}}) {
        int value=number;
        for(int digit=0;digit<8;++digit) {
            auto& binding = scores[std::string(name)+"_"+std::to_string(digit)];
            binding.path = "result_original/num_score_" +
                std::string(std::string_view(name)=="score"?"":"small_")+std::to_string(value%10)+".png";
            if (digit > 0 && value == 0) binding.tint = ScoreLeadingZero;
            value/=10;
        }
    }
    // Preserve original depth order. normal_score_list includes the ring and
    // large backing plate, so it must precede grade and music_info.
    ap("result_score","normal_score_list","0",281,749,0,false,scores);
    if (newBest) uiLayer("result-record-flag");
    if (result_.autoPlay)
        field("result-auto", "AUTO");
    flushLabels();
    ap("result_score","clear_type_"+outcomeAsset(result_.outcome),"0",-1,839,elapsed-ResultGradeAt);
    ap("result_original","grade","loop",807,798,0,false,data);
    if (newBest && !cardId_.empty()) {
        // The reference local server derives both rankings from the same record set.
        constexpr auto atlas = "runtime/result-record";
        if (!resultRecordArtReady_) {
            char score[16];
            std::snprintf(score,sizeof(score),"%08d",previousBest_);
            render_.captureImage(atlas,532,28,[&] {
                uiLayer("result-record-banner");
                field("result-record-name", previousBest_ ? playerName_ : "SDVX");
                field("result-record-score", score);
                drawUiLayers();
            });
            resultRecordArtReady_ = true;
        }
        const ImageBinding text{atlas,{0,28,532,-28}};
        ap("result_details","score_update","in",539,718,elapsed-ResultGradeAt,false,
            {{"update_frame","result_details/box_update_world.png"},{"update_text",text}});
        ap("result_details","score_update","in",539,791,elapsed-ResultGradeAt,false,
            {{"update_frame","result_details/box_update_store.png"},{"update_text",text}});
    }
    if (sessionType_ == StartType::Premium) {
        const std::array targetIcons{"best","best","s","aaa_plus","aaa"};
        const auto challenge = stageOptions_.challenge == 1 ? targetIcons[stageOptions_.target]
            : stageOptions_.challenge == 2 ? "puc" : stageOptions_.challenge == 3 ? "uc" : "none";
        uiLayer("result-premium-panel");
        uiLayer("result-challenge", "result_details/pt_result_icon_" + std::string(challenge) + ".png");
        field("result-retries", std::to_string(std::min(quickRetries_,99)));
        flushLabels();
    }
    render_.afp("result_original","music_info",elapsed<intro?"in":"in_end",elapsed,false,Screen,data);
    if(elapsed>=intro) {
        uiLayer("result-difficulty-panel", "song_row/level_small_" + lower(track.label) + ".png");
        field("result-difficulty", levelString(track.level));
        drawUiLayers();
        field("result-title", track.title);
        field("result-artist", track.artist);
    }
    const float gauge = std::clamp(result_.gauge,0.f,1.f) * 100;
    ap("result_original","option","0",741,1033,0,false,data);
    render_.afp("result_gauge",activeCourse_ ? "gauge_permissive_result" :
        result_.ultra ? "gauge_ultra_result" : result_.excessive ? "gauge_excessive_result" : "gauge_effective_result",
        "0_per", std::lround(gauge * 5) * (1000. / 120), false, {741,1033,320,304});
    char gaugeText[16];
    std::snprintf(gaugeText,sizeof(gaugeText),"%.1f",gauge);
    field("result-gauge", gaugeText);
    flushLabels();
    // Awakening reference: 1165 CRITICAL out of 1590 judgments covers about 73% of the bar.
    const int judged = std::max(1, std::accumulate(result_.timingCounts.begin(),result_.timingCounts.end(),0));
    if (document_) {
        for (size_t i = 0; i < result_.timingCounts.size(); ++i) {
            if (result_.timingCounts[i] == 0) continue;
            auto& bar = *document_->GetElementById("result-timing-" + std::to_string(i));
            uiPixels(bar, Rml::PropertyId::Width, 194.11f * result_.timingCounts[i] / judged);
            useField(bar);
        }
        drawUiLayers();
    }
    // 1803ae0e0 advances the saved gauge by 40 hundredths per 120 Hz tick.
    const int addedEnergy = resultRewardsAt_ < 0 ? 0 : int((now_-resultRewardsAt_)*120/1000)*40;
    const int resultEnergy = std::min(blasterEnergy_,resultBlasterBefore_+addedEnergy);
    if (sessionPaidWithPaseli_ && !activeCourse_)
        drawBlasterGauge(resultEnergy, elapsed, true);
    flushLabels();
    drawResultProfile("result-profile-slot");
    if (elapsed >= intro) {
        // player_usr/blc_usr is a 96x22 slot at (788,1682), formatted with [f:12][sz:18].
        field("result-balance", std::to_string(blocks_-resultBlocks_.total()));
        flushLabels();
        if (document_ && resultRewardsAt_ >= 0 && resultEnergy >= blasterEnergy_) {
            auto& bonus = *document_->GetElementById("result-bonus-rows");
            int rows = 0;
            for (const auto& [name, value, paseli] : {std::tuple{"ステージボーナス", resultBlocks_.stage, false},
                    std::tuple{"PASELIボーナス", resultBlocks_.paseli, true},
                    std::tuple{"クリアボーナス", resultBlocks_.clear, false}}) {
                if (value == 0) continue;
                auto& item = *bonus.GetChild(rows++);
                auto& label = *item.QuerySelector(".result-bonus-name");
                label.SetAttribute("data-color", paseli ? "0.31 0.78 0.57 1" : "1 1 1 1");
                setUiText(label, name);
                setUiText(*item.QuerySelector(".result-bonus-value"), std::to_string(value));
            }
            document_->GetContext()->Update();
            for (int row = 0; row < rows; ++row) {
                auto& item = *bonus.GetChild(row);
                const auto bounds = uiBounds(item);
                ap("result_details", "bonus_fuda", "0", bounds.x, bounds.y, 0);
                drawUiElement(item);
            }
        }
    }
    footer(true);
    if (forceAt_ >= 0 && !forceDone_) {
        flushLabels();
        const double time = now_ - forceAt_;
        if (forceEffect_ == 3) {
            char before[64], after[64];
            std::snprintf(before,sizeof(before),"result_force/em6_%02d_i_eab.png",forceBand(forceBefore_)/4+1);
            std::snprintf(after,sizeof(after),"result_force/em6_%02d_i_eab.png",forceBand(forceAfter_)/4+1);
            render_.afp("result_force","class_up",time < 544000./120 ? "in" : "out",
                time < 544000./120 ? time : time-544000./120,false,Screen,
                {{"emblem_before",before},{"emblem_after",after}});
            if (time >= 500 && time < 544000./120) {
                const int value=forceBefore_+int((forceAfter_-forceBefore_)*std::min(time/1500.,1.));
                char number[24];std::snprintf(number,sizeof(number),"%d.%03d",value/1000,value%1000);
                // crass_up_scouter_usr/scouter_usr: (81,65), origin (75,15), scale 1.5.
                field("result-force-total", number);
                drawUiLayers();
            }
        } else {
            const std::string name=forceEffect_==2 ? "force_level_up" : "force_plus";
            render_.afp("result_force",name+"bg","in",time,false,Screen);
            // force_playdate_s/force_icon_usr on the result's 0.9-scale player card.
            constexpr float scale=.9f, x=101.4f-600.5f*scale, y=1598.9f-662*scale;
            render_.afp("result_force",name,"in",time,false,{x,y,1300*scale,1300*scale});
            if (time >= 200 && time < 1100) {
                char delta[24]; const int gain=forceAfter_-forceBefore_;
                std::snprintf(delta,sizeof(delta),"%d.%03d",gain/1000,gain%1000);
                field("result-force-delta", delta);
                drawUiLayers();
            }
        }
    }
}

void Cabinet::drawSummary(double elapsed) {
    uiLayer("result-background");
    drawUiLayers();
    if (activeCourse_) {
        const auto skillIntro = skillResultIntroDuration();
        if (elapsed < skillIntro) {
            render_.afp("skill_effects", skillResultIntro(), "in", elapsed);
            return;
        }
        elapsed -= skillIntro;
    }
    const auto intro = render_.afpDuration("total_result", "total_all", "in");
    const double fraction=summaryCountDone_ ? 1 : std::clamp((elapsed-intro)/1200.,0.,1.);
    Bindings currency;
    for (const auto& [kind,total,gain,bonus] : {std::tuple{"packet",packets_,sessionPackets_,skillBonusPackets_},
                                             std::tuple{"block",blocks_,sessionBlocks_,skillBonusBlocks_}}) {
        int added = int(gain * fraction);
        if (skillBonusPhase_ == SkillBonusPhase::Counting) added -= skillBonusRemaining(bonus);
        int count = total - gain + added;
        for (int i=0;i<8;++i) {
            auto& large=currency[std::string(kind)+"_total_"+std::to_string(i)];
            large="total_result/num_score_"+std::to_string(count%10)+".png";
            if (i>0 && count==0) large.tint={.25f,.25f,.25f,1};
            auto& small=currency[std::string(kind)+"_gain_"+std::to_string(i)];
            small="total_result/num_bonus_"+std::to_string(added%10)+".png";
            if (i>0 && added==0) small.tint={.25f,.25f,.25f,1};
            count/=10; added/=10;
        }
    }
    render_.afp("total_result", "total_all", elapsed < intro ? "in" : "loop",
                elapsed < intro ? elapsed : elapsed - intro, elapsed >= intro,Screen,currency);
    if (elapsed >= intro) {
        if (blasterPassActive()) {
            // total_all/play_data_usr scales big_1_wallet to .9 at (54,149).
            // The badge is the blaster_usr slot (68,34) inside that wallet.
            uiLayer("summary-pass-badge");
            const auto limit = static_cast<std::time_t>(blasterPassUntil_ / 1000);
            if (const auto* date = std::localtime(&limit)) {
                char text[64];
                std::strftime(text,sizeof(text),"%Y/%m/%d %H : %Mまで有効",date);
                field("summary-pass-expiry", text);
            }
            drawUiLayers();
        }
        drawResultProfile("summary-profile-slot");
    }
    header("titletext_title_total_result");
    const auto first = sessionResults_.size() > 5 ? sessionResults_.size() - 5 : 0;
    for (size_t i = first; i < sessionResults_.size(); ++i) {
        const auto& [selection, result] = sessionResults_[i];
        // total_all/fuda01..05_usr: (540,864+172*n), origin (540,80).
        const float y = 784 + (i - first) * 172.f;
        Bindings digits;
        int score = result.score;
        for (int digit = 0; digit < 8; ++digit) {
            digits.emplace("score_" + std::to_string(digit), "total_result/num_score_" + std::to_string(score % 10) + ".png");
            score /= 10;
        }
        const auto rowIn = render_.afpDuration("total_result", "result_data", "in");
        ap("total_result", "result_data", elapsed < rowIn ? "in" : "in_end", 0, y, elapsed, false, digits);
        if (elapsed < rowIn) continue;
        // result_data's authored runtime slots; fixed lettering remains AFP.
        const auto trackLabel = i == 0 ? "1st" : i == 1 ? "2nd" :
            activeCourse_ ? (i + 1 == courses_[*activeCourse_].tracks.size() ? "final" : "3rd") :
            i == 2 ? "final" : i == 3 && exTrack() ? "ex" : "over";
        ap("total_result", std::string("track_num_") + trackLabel, "0", 7, y + 3, 0);
        rowField("summary-rows", int(i - first), result.autoPlay ? 1 : 0, selection.title);
        if (result.autoPlay) rowField("summary-rows", int(i - first), 2, "AUTO");
        if (document_) {
            auto& item = *document_->GetElementById("summary-rows")->GetChild(int(i - first));
            auto& difficulty = *item.QuerySelector(".summary-difficulty-panel");
            uiImage(difficulty, "menu/art/level_small_" + lower(selection.label) + ".png");
            useField(difficulty);
            auto& grade = *item.QuerySelector(".summary-grade");
            uiImage(grade, "menu/art/grade_" + gradeAsset(result.grade) + ".png");
            auto& lamp = *item.QuerySelector(".summary-lamp");
            uiImage(lamp, "menu/art/mark_" + outcomeAsset(result.outcome) + ".png");
            drawUiLayers();

            // Keep the original bitmap glyphs and decimal dot. The row owns
            // the slot, including its dimensions, for both difficulty forms.
            level(selection.level, uiBounds(*item.QuerySelector(".summary-difficulty-digits")), true);
            drawUiElement(grade);
            drawUiElement(lamp);
        }
    }
    footer();
    ap("total_result", "operation_total_result", "operation_01", 0, 1785, 0);
    if (activeCourse_ && elapsed >= intro) {
        flushLabels();
        Bindings skillInfo{{"skill",skillPanel(courses_[*activeCourse_].level)}};
        int rate = skillAchievement();
        for (int digit = 2; digit >= 0; --digit) {
            skillInfo.emplace("rate_" + std::to_string(digit), digit < 2 && rate == 0 ? ""
                : "total_result/num_score_" + std::to_string(rate % 10) + ".png");
            rate /= 10;
        }
        render_.afp("skill_effects", "skill_total_info", "0", 0, false, Screen, skillInfo);
        render_.afp("skill_effects", skillPassed() ? "hantei_success" : "hantei_fail", "loop", 0,
                    false, {384,1523,644,84});
        const auto change = elapsed - SkillChangeAt;
        if (skillLevelChanged_ && change >= 0 && change < render_.afpDuration("skill_effects", "skill_change", "in"))
            render_.afp("skill_effects", "skill_change", "in", change, false, Screen,
                        {{"before",skillPanel(skillLevelBefore_, skillTitleBefore_)},
                         {"after",skillPanel(skillLevel_, courses_[*activeCourse_].nameId)}});
        if (skillTitleAt_ >= 0) drawSkillTitleChange();
        if (skillBonusPhase_ != SkillBonusPhase::Pending && skillBonusPhase_ != SkillBonusPhase::Done)
            drawSkillBonus();
    }
}

void Cabinet::drawSkillBonus() {
    const double age = now_ - skillBonusAt_;
    const double intro = render_.afpDuration("skill_effects/windows", "win_skill_bonus", "in");
    const bool opening = skillBonusPhase_ == SkillBonusPhase::Waiting && age < intro;
    const bool closing = skillBonusPhase_ == SkillBonusPhase::Closing;
    int amount = skillBonusPhase_ == SkillBonusPhase::Waiting ? skillBonus_ :
        skillBonusPhase_ == SkillBonusPhase::Counting ? skillBonusRemaining(skillBonus_) : 0;
    Bindings digits;
    for (int digit = 1; digit <= 5; ++digit) {
        digits.emplace("bonus_" + std::to_string(digit), digit > 1 && amount == 0 ? ""
            : "skill_effects/windows/num_stamp_bonus_" + std::to_string(amount % 10) + ".png");
        amount /= 10;
    }
    render_.afp("skill_effects/windows", "win_skill_bonus", opening ? "in" : closing ? "out" : "in_end", age, false,
                Screen, digits);
}

void Cabinet::drawSkillTitleChange() {
    const double age = now_ - skillTitleAt_;
    const double intro = render_.afpDuration("skill_effects/windows", "win_skill_change", "in");
    const double decision = now_ - skillTitleDecidedAt_;
    const bool deciding = skillTitleDecidedAt_ >= 0;
    const double decisionLength = render_.afpDuration("session_dialog", "yes_no", "decide_yes");
    const bool closing = deciding && decision >= decisionLength;
    render_.afp("skill_effects/windows", "win_skill_change", closing ? "out" : age < intro ? "in" : "in_end",
                closing ? decision - decisionLength : age);
    if (age < intro || closing) return;

    const int proposed = courses_[*activeCourse_].nameId;
    uiLayer("skill-panel-before", skillPanel(skillLevel_, skillTitleBefore_));
    uiLayer("skill-panel-after", skillPanel(skillLevel_, proposed));
    if (skillTitleBefore_) field("skill-name-before", skillName(skillTitleBefore_));
    if (proposed) field("skill-name-after", skillName(proposed));
    field("skill-question", "このスキルネームに変更しますか？");
    flushLabels();
    const auto focusClip = skillTitleFocus_ == 0 ? "no_yes" : "yes_no";
    const double focusAge = now_ - skillTitleFocusAt_;
    if (!deciding && skillTitleFocusAt_ >= 0 && focusAge < render_.afpDuration("skill_effects/windows", "yes_no", focusClip)) {
        ap("skill_effects/windows", "yes_no", focusClip, 0, 1027, focusAge);
    } else {
        const auto clip = deciding ? (skillTitleFocus_ == 0 ? "decide_yes" : "decide_no")
                                   : (skillTitleFocus_ == 0 ? "yes_loop" : "no_loop");
        ap("session_dialog", "yes_no", clip, 0, 1027, deciding ? decision : age - intro, !deciding);
    }
}

void Cabinet::drawSearch(double elapsed) {
    const auto intro = render_.afpDuration("subscreen", "page_music_search", "in");
    const Song* item = searchMatches_.empty() ? nullptr : &songs_[searchMatches_[searchFocus_]];
    const int chartCount = item ? chartRange(*item, false).second : 0;
    const Selection* selected = item ? &item->charts[std::min(chartIndex_, chartCount - 1)].track : nullptr;
    const auto jacket = selected ? selected->asset(selected->jacket) : "subscreen/jacket-empty.png";
    Bindings detail{{"jacket", jacket}};
    for (int i = 0; i < 5; ++i) {
        detail["level_" + std::to_string(i + 1)] = i < chartCount
            ? "song_row/level_small_" + lower(item->charts[i].track.label) + ".png" : "";
    }
    render_.afp("subscreen", "page_music_search", elapsed < intro ? "in" : "in_end", elapsed, false, CrewPanel,
                detail);
    if (elapsed < intro) return;
    if (selected) {
        field("search-title", selected->title);
        field("search-artist", selected->artist);
        field("search-charter", selected->charter);
        field("search-illustrator", selected->illustrator);
        for (int i = 0; i < chartCount; ++i)
            level(item->charts[i].track.level, {424.f+(i%4)*119,508.f+(i/4)*32,42,17}, true);
    }
    flushLabels();
    render_.pushClip({879,216,1000,787.5f});
    const int first = searchFocus_ / 5 * 5;
    for (int index = first; index < std::min(first + 5, int(searchMatches_.size())); ++index) {
        const auto& item = songs_[searchMatches_[index]];
        const int count = chartRange(item, false).second;
        const auto& track = item.charts[std::min(chartIndex_, count - 1)].track;
        const float y = 216 + (index - first) * 157.5f;
        Bindings row{{"jacket", track.asset(track.jacket)}};
        for (int i = 0; i < 5; ++i) row["level_" + std::to_string(i + 1)] = i < count
            ? "song_row/level_small_" + lower(item.charts[i].track.label) + ".png" : "";
        render_.afp("subscreen", "search_box_fuda", index == searchFocus_ ? "touch" : "release", 0, false,
                    {879,y,1080,150}, row);
        for (int i = 0; i < count; ++i)
            level(item.charts[i].track.level, {1627.f+(i%2)*142,y+37+(i/2)*33,48,19}, true);
        rowField("search-rows", index - first, 0, track.title);
        rowField("search-rows", index - first, 1, track.artist);
    }
    flushLabels();
    render_.popClip();
    if (searchEditing_ || !searchQuery_.empty()) {
        const float shift=searchEditing_?keyboardBarTop_-588:0;
        render_.afp("subscreen", "keyboard_group", searchEditing_?"disp_loop":"hide_loop", 0, false, {0,shift,1920,1080});
        if (document_) {
            auto* bar = document_->GetElementById("search-bar");
            const float top = searchEditing_ ? std::nearbyint(keyboardBarTop_) : 1008.f;
            const auto* position = bar->GetLocalProperty(Rml::PropertyId::Top);
            if (!position || position->Get<float>() != top) {
                bar->SetProperty(Rml::PropertyId::Top, {top, Rml::Unit::PX});
                uiDirty_ = true;
            }
        }
        field("search-query", searchQuery_ + searchComposition_);
        field("search-count", std::to_string(searchMatches_.size()) + " 件");
    }
}

void Cabinet::drawSettings(double elapsed) {
    render_.afp("crew_selector/background", "bg", "0", now_ - roomStarted_, true, CrewPanel);
    if (playTab_ != 1) {
        if (playTab_ == 0) render_.afp("subscreen", "page_home", "0", 0, false, CrewPanel);
        else if (playTab_ == 2) drawSearch(elapsed);
        else drawAchievements(elapsed);
        drawSubscreenHeader(false);
        return;
    }
    const bool leaving = settingsLeavingAt_ >= 0;
    if (controllerSettings()) {
        drawSubscreenHeader(false);
        render_.afp("settings", "option_top", "release", 0, false, GeneralSettingsTab);
        if (bindingFocus_ == BindingControlCount) render_.image("settings/focus.png", GeneralSettingsTab);
        render_.afp("settings", "option_top", "press", 0, false, ControllerSettingsTab,
                    {{"option_tab_label", ""}});
        field("controller-caption", "控制器");
        const bool joystick = controllerMode_ == ControllerMode::Joystick;
        const bool leftUsesX = joystick ? !controllerLeftUsesY_ : leftUsesX_;
        const auto& keys = joystick ? controllerMap_ : keyMap_;
        const auto& reverse = joystick ? controllerReverse_ : reverse_;
        field("controller-assignments", "ボタン割り当て");
        const auto connection = joystick ? "NEMSYS HID" : controllerMode_ == ControllerMode::KeyboardMouse
            ? "KEYBOARD / MOUSE" : "未接続";
        field("controller-type", connection);
        if (document_) document_->GetElementById("controller-type")->SetClass("binding-prompt", joystick);
        constexpr std::array otherLabels{"VOL-L / VOL-R", "VOL-L", "VOL-R", "初期設定に戻す"};
        for (int control = 0; control < BindingControlCount; ++control) {
            const auto index = std::to_string(control);
            const bool binding = bindTarget_ == control;
            if (document_) {
                useField(*document_->GetElementById("binding-bg-" + index));
                if (binding || bindingFocus_ == control)
                    useField(*document_->GetElementById("binding-focus-" + index));
            }
            const auto name = control < ButtonCount ? Buttons[control] : otherLabels[control - ButtonCount];
            field(("binding-label-" + index).c_str(), name);
            std::string value;
            if (binding) value = "ボタンを押してください";
            else if (control < ButtonCount) {
                if (keys[control] == 0) value = "未割り当て";
                else if (joystick) value = "BUTTON " + std::to_string(keys[control]);
                else value = SDL_GetScancodeName(static_cast<SDL_Scancode>(keys[control]));
            } else if (control == Axes) value = leftUsesX ? "X / Y" : "Y / X";
            else if (control != ResetBindings) {
                const int side = control - ReverseLeft;
                value = reverse[side] ? "反転" : "通常";
                const float dial = 220.f * std::tanh(calibration_[side] / 180.f);
                if (document_ && dial != 0) {
                    auto& meter = *document_->GetElementById("binding-dial-" + index);
                    meter.SetProperty(Rml::PropertyId::Width, {std::abs(dial), Rml::Unit::PX});
                    meter.SetProperty(Rml::PropertyId::MarginLeft, {std::min(0.f, dial), Rml::Unit::PX});
                    useField(meter);
                }
            }
            if (!value.empty())
                field(((binding ? "binding-prompt-" : "binding-value-") + index).c_str(), value);
        }
        return;
    }
    auto movie=[&](std::string_view name,std::string_view clip,Rect bounds,double time=0,bool focused=false) {
        const Bindings bindings=focused?Bindings{{"focus_frame",ImageBinding{"settings/focus.png",{.5f,.5f,197,47}}}}:Bindings{};
        render_.afp("settings",name,clip,time,false,bounds,bindings);
    };
    const double intro=render_.afpDuration("settings","page_option","in");
    const bool premium = settingsReturn_ == Page::Room || sessionType_ == StartType::Premium;
    const bool light = !premium && sessionType_ == StartType::Light;
    constexpr auto tabLabel = "runtime/controller-tab-label";
    if (!controllerTabLabelReady_) {
        render_.captureImage(tabLabel, 198, 30, [&] {
            render_.text("控制器", 99, 23, 23, White, TextAlign::Center, 198, TextFont::MaruGothicW4);
        });
        controllerTabLabelReady_ = true;
    }
    render_.afp("settings",premium?"page_option_premium":light?"page_option_light":"page_option_normal",leaving?"out":elapsed<intro?"in":"in_end",
                leaving?now_-settingsLeavingAt_:elapsed,false,CrewPanel,
                {{"option_lighting", ImageBinding{tabLabel, {0,30,198,-30}, {0,-3,198,30}}}});
    drawSubscreenHeader(false);
    if(elapsed<intro || leaving) return;
    movie("option_top","press",GeneralSettingsTab);
    if (optionFocus_ == ControllerTabFocus) render_.image("settings/focus.png", ControllerSettingsTab);
    for(const auto& slot:OptionSlots) {
        if (!premium && (slot.option == Option::Challenge || slot.option == Option::Average)) continue;
        if (slot.option == Option::AutoPlay) {
            render_.rect({slot.bounds.x-18,slot.bounds.y-75,232,136}, {0,0,0,.72f});
            render_.text("AUTO", slot.bounds.x+slot.bounds.w/2, slot.bounds.y-32, 20,
                         White, TextAlign::Center, slot.bounds.w, TextFont::Display);
        }
        const auto value=optionChoice(slot.option);
        const bool focus=optionFocus_==static_cast<int>(slot.option);
        std::string clip=slot.option==Option::Score ? (value?"score_ex":"score_nomal")
            : optionArtwork(slot.option,value)=="option_off" ? "no_select" : "select";
        if (light && (slot.option == Option::Score || slot.option == Option::SCritical || slot.option == Option::Ars))
            clip += "_disabled";
        movie(optionArtwork(slot.option,value),clip,slot.bounds,0,focus);
    }
    if (optionFocus_ == static_cast<int>(Option::Ars)) render_.image("settings/focus.png", ArsOption);
    render_.image(options_.ars && !light ? "settings/bt_rate_arson.png" : "settings/bt_rate_arsoff.png",
                  ArsOption, light ? Color{.5f,.5f,.5f,1} : White);
    movie("lane_preview", "no_select", LanePreviewOption, 0, optionFocus_ == static_cast<int>(Option::LanePreview));
    movie("fix","no_select",{1601,949,168,46});
    if (document_) {
        useField(*document_->GetElementById("settings-nemsys-bg"));
        if (optionFocus_ == static_cast<int>(Option::Nemsys))
            useField(*document_->GetElementById("settings-nemsys-focus"));
        auto& preview = *document_->GetElementById("settings-nemsys-preview");
        preview.SetAttribute("src", "../" + std::string(nemsysSkin(options_.nemsys).image));
        useField(preview);
        field("settings-nemsys-title", "NEMSYS");
        char skinNumber[8];
        std::snprintf(skinNumber, sizeof(skinNumber), "%04d", options_.nemsys);
        field("settings-nemsys-number", skinNumber);
        field("settings-nemsys-left", "<");
        field("settings-nemsys-right", ">");
        field("settings-volume", std::to_string(volume_));
        for (int column = 0; column < 3; ++column) {
            const auto option = std::array{Option::Judgment, Option::Display, Option::Speed}[column];
            const auto index = std::to_string(column);
            useField(*document_->GetElementById("gauge-track-" + index));
            const auto value = optionValue(option);
            const int limit = column < 2 ? options_.timingLimit(column) : 0;
            const float fraction = option == Option::Speed ? (value - 100) / 900.f : (value + limit) / (2.f * limit);
            auto& thumb = *document_->GetElementById("gauge-thumb-" + index);
            thumb.SetProperty(Rml::PropertyId::MarginTop, {-544 * fraction, Rml::Unit::PX});
            useField(thumb);
            if (optionFocus_ == static_cast<int>(option))
                useField(*document_->GetElementById("gauge-focus-" + index));
            field(((column < 2 && options_.fineTiming[column] ? "gauge-fine-" : "gauge-value-") + index).c_str(),
                  optionLabel(option));
        }
        flushLabels();
    }
    if(optionPopup_>=0) {
        const auto& slot=OptionSlots[optionPopup_];
        const float spacing=optionChoices(slot.option)==5?51.f:67.5f;
        const bool color=slot.option==Option::LeftColor || slot.option==Option::RightColor;
        const Rect popup{color?909.f:slot.bounds.x-18,slot.bounds.y-73,232,slot.option==Option::Rate?286.f:340.f};
        const auto phase=now_-optionPopupAt_;
        const auto popupIn=render_.afpDuration("settings",slot.popup,"hide_to_disp");
        if(!color) movie(slot.popup,phase<popupIn?"hide_to_disp":"disp_loop",popup,phase);
        if(!color && phase<popupIn) return;
        for(int choice=0;choice<optionChoices(slot.option);++choice) {
            const Rect value{slot.bounds.x,slot.bounds.y+spacing*choice,slot.bounds.w,slot.bounds.h};
            movie(optionArtwork(slot.option,choice),choice==optionChoice(slot.option)?"select":"no_select",value);
        }
    }
}

void Cabinet::drawTransition(const Transition& transition, double elapsed) {
    if (transition.kind == TransitionKind::MatchingOut) return;
    const double hold = transition.holdMs;
    if (transition.kind == TransitionKind::NextArea) {
        const bool leaving = elapsed >= transition.inMs + hold;
        drawNextArea(leaving ? elapsed - transition.inMs - hold : elapsed, leaving);
        return;
    }
    const bool announcement = transition.kind == TransitionKind::TrackIntro;
    if (announcement && transition.phase == TransitionPhase::Opening) {
        const double phase = elapsed - transition.inMs - hold;
        const bool leaving = transition.announcementOutOffset >= 0;
        render_.afp("track_intro", trackIntroName(), leaving ? "out" : "in",
            leaving ? phase - transition.announcementOutOffset : phase);
    }
    if (transition.kind == TransitionKind::Shutter || announcement) {
        const bool closing = elapsed < transition.inMs;
        const bool closed = transition.phase == TransitionPhase::WaitingForStage || elapsed < transition.inMs + hold
            || (announcement && transition.phase == TransitionPhase::Covering);
        const auto clip = closing ? "in" : closed ? "loop" : "out";
        const double phase = closing ? elapsed : closed ? elapsed - transition.inMs : elapsed - transition.inMs - hold;
        if (!announcement || closing || closed || phase < render_.afpDuration("shutter_nabla", "shutter_default", "out"))
            render_.afp("shutter_nabla", "shutter_default", clip, phase, closed);
        return;
    }
    if (elapsed < transition.inMs) render_.afp("track_entry", "loading", "loading_in", elapsed);
    else if (elapsed < transition.inMs + hold) render_.afp("track_entry", "loading", "loading_loop", elapsed - transition.inMs, true);
    else render_.afp("track_entry", "loading", "loading_out", elapsed - transition.inMs - hold);
    ap("track_entry", sessionType_ == StartType::Premium ? "track_premium"
       : exTrack() ? "track_ex"
       : track_ == 1 ? "track_1st" : track_ == 2 ? "track_2nd" : "track_final", "loop", 405, 946, elapsed, true);
}

void Cabinet::drawStageShutter(double nowMs) {
    if (matchingDeparture()) {
        navigator_.draw(CrewScene::Matching);
        const auto elapsed = nowMs - transition_->started;
        const auto duration = render_.afpDuration("matching", "hispeed", "hispeed_out");
        if (elapsed < duration) render_.afp("matching", "hispeed", "hispeed_out", elapsed);
    } else if (stageTransition()) drawTransition(*transition_, nowMs - transition_->started);
}
}
