#include "cabinet.h"
#include "cabinet_crew_catalog.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL_stdinc.h>
#include <cmath>

namespace sdvx {
namespace {
constexpr std::string_view SubscreenTabSound = "menu/audio/cursor/se_04_cursor004_submonitor1.wav";
constexpr std::string_view SubscreenChangeSound = "menu/audio/cursor/se_04_cursor005_submonitor2.wav";
}

void Cabinet::updateMenuInput() {
    constexpr std::array views{
        "title", "entry", "room", "generator", "generator", "crew", "caution", "mode", "credit",
        "skill", "songs", "matching", "", "result", "summary", "exit", "", "game-end", "settings"
    };
    const auto view = lanePreview_ ? "lane" : page_ == Page::Songs && sortOpen_ ? "sort"
        : page_ == Page::Songs && songConfirm_ ? "difficulty" : views[static_cast<size_t>(page_)];
    auto* next = inputDocument_->GetElementById(view);
    if (inputView_ != next) {
        if (inputView_) inputView_->SetClass("shown", false);
        inputView_ = next;
        if (inputView_) inputView_->SetClass("shown", true);
    }
    if (!inputView_) return;

    auto& page = *inputView_;
    switch (page_) {
    case Page::Pin:
        page.SetClass("entry-pin", entryStep_ == EntryStep::Pin);
        page.SetClass("entry-registration", entryStep_ == EntryStep::Registration);
        page.SetClass("entry-terms", entryStep_ == EntryStep::Terms);
        page.SetClass("entry-name", entryStep_ == EntryStep::Name);
        page.SetClass("entry-continue", entryStep_ != EntryStep::Pin && entryStep_ != EntryStep::Name
            && entryStep_ != EntryStep::Loading && entryStep_ != EntryStep::Registration && entryStep_ != EntryStep::Terms);
        break;
    case Page::Generator: case Page::Valkyrie:
        page.SetClass("generator-main", page_ == Page::Valkyrie || generatorPhase_ == 0);
        page.SetClass("generator-count", page_ == Page::Generator && generatorPhase_ == 0);
        page.SetClass("generator-volume", page_ == Page::Valkyrie);
        page.SetClass("generator-confirm", page_ == Page::Generator && generatorPhase_ == 1);
        break;
    case Page::Credit:
        page.SetClass("payment-menu", paymentAcceptedAt_ < 0);
        page.SetClass("payment-receipt", paymentAcceptedAt_ >= 0);
        break;
    case Page::Songs:
        page.SetClass("premium-end", canEndPremium());
        break;
    case Page::Result: {
        const bool celebrating = navigator_.celebratingResult();
        page.SetClass("result-character", !celebrating);
        page.SetClass("result-celebrating", celebrating);
        break;
    }
    case Page::Summary:
        page.SetClass("summary-main", skillTitleAt_ < 0);
        page.SetClass("summary-bonus", skillTitleAt_ < 0 &&
            (skillBonusPhase_ == SkillBonusPhase::Waiting || skillBonusPhase_ == SkillBonusPhase::Ready));
        page.SetClass("summary-title", skillTitleAt_ >= 0 && skillTitleDecidedAt_ < 0
            && now_ - skillTitleAt_ >= render_.afpDuration("skill_effects/windows", "win_skill_change", "in"));
        break;
    case Page::Crew: {
        page.SetClass("achievements", roomTab_ == 4);
        page.SetClass("crew-home", roomTab_ == 0);
        page.SetClass("crew-models", roomTab_ == 1);
        page.SetClass("crew-save", roomTab_ >= 1 && roomTab_ <= 3);
        const size_t styles = roomTab_ == 1 ? crewFamilies()[crewFamilyIndex()].styles.size() : 0;
        for (size_t count = 1; count <= 6; ++count)
            page.SetClass("crew-styles-" + std::to_string(count), styles == count);
        page.SetClass("crew-cards", roomTab_ == 2);
        page.SetClass("crew-board", roomTab_ == 3);
        page.SetClass("crew-board-style", roomTab_ == 3 && decorationTab_ == 0);
        page.SetClass("crew-board-base", roomTab_ == 3 && decorationTab_ == 1);
        page.SetClass("crew-stickers", roomTab_ == 3 && decorationTab_ == 2);
        page.SetClass("filter", cardFilterOpen_);
        break;
    }
    case Page::Settings: {
        if (lanePreview_) break;
        const bool general = playTab_ == 1 && settingsTab_ == SettingsTab::General;
        const bool premium = settingsReturn_ == Page::Room || sessionType_ == StartType::Premium;
        page.SetClass("achievements", playTab_ == 3);
        page.SetClass("settings-home", playTab_ == 0);
        page.SetClass("settings-search", playTab_ == 2);
        page.SetClass("search-has-query", playTab_ == 2 && (searchEditing_ || !searchQuery_.empty()));
        page.SetClass("settings-options", playTab_ == 1);
        page.SetClass("controller", controllerSettings());
        page.SetClass("general", general);
        page.SetClass("premium-options", general && premium);
        page.SetClass("score-options", general && (premium || sessionType_ != StartType::Light));
        page.SetClass("option-popup", general && optionPopup_ >= 0);
        const float searchTop = searchEditing_ ? keyboardBarTop_ : 1008.f;
        inputView_->GetElementById("search-edit-target")->SetProperty(Rml::PropertyId::Top, {searchTop, Rml::Unit::PX});
        inputView_->GetElementById("search-toggle-target")->SetProperty(Rml::PropertyId::Top, {searchTop - 23, Rml::Unit::PX});
        if (general && optionPopup_ >= 0) {
            const auto& slot = OptionSlots[optionPopup_];
            const int count = optionChoices(slot.option);
            const float spacing = count == 5 ? 51.f : 67.5f;
            for (int choice = 0; choice < 5; ++choice) {
                auto* target = inputView_->GetElementById("option-choice-" + std::to_string(choice));
                target->SetClass("option-hidden", choice >= count);
                if (choice >= count) continue;
                target->SetProperty(Rml::PropertyId::Left, {slot.bounds.x, Rml::Unit::PX});
                target->SetProperty(Rml::PropertyId::Top, {slot.bounds.y + spacing * choice, Rml::Unit::PX});
                target->SetProperty(Rml::PropertyId::Width, {slot.bounds.w, Rml::Unit::PX});
                target->SetProperty(Rml::PropertyId::Height, {slot.bounds.h, Rml::Unit::PX});
            }
        }
        break;
    }
    default: break;
    }
}

Rml::Element* Cabinet::menuTarget(float x, float y) {
    if (!inputDocument_) return nullptr;
    updateMenuInput();
    auto* context = inputDocument_->GetContext();
    context->Update();
    if (!inputView_) return nullptr;
    auto* target = context->GetElementAtPoint({x, y}, nullptr, inputView_);
    if (!target || !target->HasAttribute("data-action")) return nullptr;
    if (target->IsClassSet("round")) {
        const auto size = target->GetBox().GetSize();
        const auto offset = Rml::Vector2f{x, y} - target->GetAbsoluteOffset() - size * .5f;
        if (offset.SquaredMagnitude() > size.x * size.x * .25f) return nullptr;
    }
    return target;
}

void Cabinet::setGauge(Rml::Element& gauge, float y) {
    const int column = gauge.GetAttribute<int>("data-value", 0);
    const auto option = std::array{Option::Judgment, Option::Display, Option::Speed}[column];
    const float fraction = std::clamp(1.f - (y - gauge.GetAbsoluteOffset().y) / gauge.GetBox().GetSize().y, 0.f, 1.f);
    const int limit = column < 2 ? options_.timingLimit(column) : 0;
    const int goal = option == Option::Speed ? 100 + int(std::lround(fraction * 900))
        : int(std::lround(fraction * 2 * limit)) - limit;
    adjust(option, goal - optionValue(option));
    optionFocus_ = static_cast<int>(option);
}

void Cabinet::menuAction(std::string_view action, int value) {
    if (action == "confirm") confirm();
    else if (action == "back") back();
    else if (action == "credit") credit();
    else if (action == "entry-confirm") confirmEntry();
    else if (action == "pin-key") { pinFocus_ = value; confirmEntry(); }
    else if (action == "entry-choice") { entryFocus_ = value; confirmEntry(); }
    else if (action == "entry-name-step" || action == "generator-step" || action == "skill-step") rotate(0, value);
    else if (action == "mode-step" || action == "valkyrie-step" || action == "skill-category") rotate(1, value);
    else if (action == "entry-name-erase") { if (!draftName_.empty()) eraseNameCharacter(); }
    else if (action == "room-tile") { setRoomFocus(value); confirm(); }
    else if (action == "room-subscreen") enter(Page::Crew);
    else if (action == "generator-choice") { generatorFocus_ = value; confirm(); }
    else if (action == "exit-choice") { exitFocus_ = value; confirm(); }
    else if (action == "payment-choice") {
        const int count = int(modePayments(modeIndex_).size());
        if (value < count || value == 4) { creditFocus_ = value == 4 ? count : value; confirm(); }
    } else if (action == "songs-folder") toggleWheel(Wheel::Folder);
    else if (action == "songs-sort") toggleWheel(Wheel::Sort);
    else if (action == "songs-end") requestPremiumExit();
    else if (action == "settings") openSettings();
    else if (action == "song-wheel") { if (value == 0) confirm(); else rotate(1, value); }
    else if (action == "songs-difficulty") {
        if (wheel_ == Wheel::Songs || songConfirm_)
            rotate(0, std::min(value, int(song().charts.size()) - 1) - chartIndex_);
    } else if (action == "songs-jacket" && wheel_ == Wheel::Songs) {
        if (now_ - touchDownAt_ < 600) { confirm(); return; }
        songPromptPending_ = false;
        const int id = chart().track.id;
        const auto favorite = std::find(favoriteSongs_.begin(), favoriteSongs_.end(), id);
        if (favorite == favoriteSongs_.end()) favoriteSongs_.push_back(id);
        else favoriteSongs_.erase(favorite);
        saveProfile();
        rebuildFolders(folderGroup_, folderGroupValue_);
        rebuildSongs();
        play(Decide);
    } else if (action == "sort-item") { sortFocus_ = value; confirm(); }
    else if (action == "sort-close") closeSongOverlay(true);
    else if (action == "matching-speed") { adjust(Option::Speed, value); saveProfile(); }
    else if (action == "character") characterView_ = !characterView_;
    else if (action == "summary-title") { skillTitleFocus_ = value; confirm(); }
    else if (action == "submonitor-back") { if (page_ == Page::Crew) play(Decide); back(); }
    else if (action == "room-tab") switchRoomTab(value);
    else if (action == "crew-start") { startAfterSubscreen_ = true; back(); }
    else if (action == "crew-cancel") cancelCrew();
    else if (action == "crew-family") {
        const int index = crewOffset_ + value;
        const auto& families = crewFamilies();
        if (index < int(families.size()) && index != crewFamilyIndex()) chooseCrew(families[index].styles.front());
    } else if (action == "crew-style") chooseCrew(crewFamilies()[crewFamilyIndex()].styles[value]);
    else if (action == "card-item") {
        const int index = cardOffset_ + value;
        if (index >= int(visibleCards_.size())) return;
        draftCard_ = visibleCards_[index];
        if (now_ - touchDownAt_ >= 600) {
            const auto id = appealCards_[draftCard_].id;
            const auto favorite = std::find(favoriteCards_.begin(), favoriteCards_.end(), id);
            if (favorite == favoriteCards_.end()) favoriteCards_.push_back(id);
            else favoriteCards_.erase(favorite);
            saveProfile();
            if (favoriteCardsOnly_) rebuildCards();
        }
        play(Cursor);
    } else if (action == "card-random") {
        if (!visibleCards_.empty()) {
            const int index = SDL_rand(int(visibleCards_.size()));
            draftCard_ = visibleCards_[index];
            cardOffset_ = index / 20 * 20;
        }
        play(Cursor);
    } else if (action == "card-favorites") {
        favoriteCardsOnly_ = !favoriteCardsOnly_;
        rebuildCards();
        play(Cursor);
    } else if (action == "card-filter") {
        draftCardFilter_ = cardFilter_;
        cardFilterOpen_ = true;
        play(Cursor);
    } else if (action == "decoration-tab") {
        decorationTab_ = value;
        decorationOffset_ = 0;
        stickerFocus_ = -1;
        play(Cursor);
    } else if (action == "board-style") { draftStyle_ = value; play(Cursor); }
    else if (action == "board-base") {
        draftBase_ = std::min(decorationOffset_ + value, int(boardBases_.size()) - 1);
        play(Cursor);
    } else if (action == "sticker-remove") removeSticker();
    else if (action == "sticker-item") {
        const int index = decorationOffset_ + value;
        if (index < int(boardStickers_.size())) placeSticker(index);
    } else if (action.starts_with("filter-")) {
        if (action == "filter-all") draftCardFilter_ = value == 0 ? CardGenreMask | CardSeriesMask : 0;
        else if (action == "filter-toggle") draftCardFilter_ ^= CardFilterOptions[value].first;
        else if (action == "filter-confirm") {
            if (value == 0) cardFilterOpen_ = false;
            else confirm();
        }
        play(Cursor);
    } else if (action == "claim-all") claimAchievement(-1);
    else if (action == "achievement-category") {
        const int index = achievementCategoryOffset_ + value;
        if (index < int(AchievementFolders.size())) {
            achievementCategory_ = AchievementFolders[index];
            achievementOffset_ = 0;
            play(Cursor);
        }
    } else if (action == "achievement-item") {
        int offset = achievementOffset_ + value;
        for (int index = 0; index < int(achievements_.size()); ++index)
            if (achievements_[index].category == achievementCategory_ && offset-- == 0) { claimAchievement(index); break; }
    } else if (action == "play-tab" || action == "play-home") {
        if (action == "play-tab" && value == playTab_) {
            if (value == 1 && settingsTab_ != SettingsTab::General) switchSettingsTab(SettingsTab::General);
            return;
        }
        playTab_ = value;
        settingsTab_ = SettingsTab::General;
        optionFocus_ = 0;
        bindTarget_ = -1;
        searchEditing_ = false;
        searchComposition_.clear();
        optionPopup_ = -1;
        entered_ = now_;
        play(SubscreenTabSound);
    } else if (action == "search-edit" || action == "search-toggle") {
        searchEditing_ = action == "search-edit" || !searchEditing_;
        ++keyboardRequest_;
    } else if (action == "search-item") {
        const int selected = searchFocus_ / 5 * 5 + value;
        if (selected < int(searchMatches_.size())) {
            searchFocus_ = selected;
            confirm();
        }
    } else if (action == "settings-tab") {
        const auto tab = static_cast<SettingsTab>(value);
        if (tab != settingsTab_) switchSettingsTab(tab);
    } else if (action == "binding") activateBinding(value);
    else if (action == "option-dismiss" || action == "option-choice") {
        if (action == "option-choice") chooseOption(OptionSlots[optionPopup_].option, value);
        optionPopup_ = -1;
    } else if (action == "lane-preview") {
        optionFocus_ = static_cast<int>(Option::LanePreview);
        adjust(Option::LanePreview, 1);
    } else if (action == "ars") {
        optionFocus_ = static_cast<int>(Option::Ars);
        chooseOption(Option::Ars, !options_.ars);
    } else if (action == "nemsys") {
        optionFocus_ = static_cast<int>(Option::Nemsys);
        if (value) rotate(1, value);
    } else if (action == "volume") {
        volume_ = std::clamp(volume_ + value, 0, 10);
        audio_.setGain(volume_ / 10.f);
        play(SubscreenChangeSound);
    } else if (action == "option") {
        const auto& slot = OptionSlots[value];
        optionFocus_ = static_cast<int>(slot.option);
        if (*slot.popup) { optionPopup_ = value; optionPopupAt_ = now_; }
        else chooseOption(slot.option, 1 - optionChoice(slot.option));
    } else if (action == "precision" || action == "gauge-step") {
        const int column = action == "precision" ? value : std::abs(value) - 1;
        const auto option = std::array{Option::Judgment, Option::Display, Option::Speed}[column];
        optionFocus_ = static_cast<int>(option);
        if (action == "precision") { options_.toggleTimingPrecision(column); play(Decide); }
        else { adjust(option, value > 0 ? 1 : -1); play(SubscreenChangeSound); }
    }
}

void Cabinet::touch(float x, float y) {
    if (auto* target = menuTarget(x, y)) {
        const auto action = target->GetAttribute<std::string>("data-action", "");
        if (action == "gauge") setGauge(*target, y);
        else menuAction(action, target->GetAttribute<int>("data-value", 0));
    }
}
}
