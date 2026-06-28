// Copyright (c) 2024 PPSSPP Project

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "ppsspp_config.h"
#include "Common/UI/UI.h"
#include "Common/UI/PopupScreens.h"
#include "Common/UI/ViewGroup.h"
#include "Common/Data/Text/I18n.h"
#include "Common/System/Request.h"
#include "Common/StringUtils.h"
#include "Core/System.h"
#include "Core/Config.h"
#include "Core/CwCheat.h"
#include "Core/MemorySearch.h"
#include "UI/MemorySearchScreen.h"
#include "UI/MiscViews.h"
#include "thread"
#include "atomic"

#ifdef _WIN32
#undef min
#undef max
#endif
#include "UI/GameInfoCache.h"

static const char *searchTypeKeys[] = {
	"8Bit", "16Bit", "32Bit", "64Bit", "Float", "Double"
};

static const char *compareTypeKeys[] = {
	"ExactValue", "GreaterThan", "LessThan", "GreaterOrEqual",
	"LessOrEqual", "Between", "Changed", "Unchanged", "Increased", "Decreased", "UnknownValue"
};

static const char *regionKeys[] = {
	"MainRAM", "VRAM", "Scratchpad", "AllMemory"
};

std::unique_ptr<MemorySearchEngine> MemorySearchScreen::s_sharedEngine;
Path MemorySearchScreen::s_lastGamePath;
int MemorySearchScreen::s_searchTypeIndex = 2;
int MemorySearchScreen::s_compareTypeIndex = 0;
int MemorySearchScreen::s_regionIndex = 3;
bool MemorySearchScreen::s_alignedSearch = false;
int64_t MemorySearchScreen::s_searchValue = 0;
int64_t MemorySearchScreen::s_searchValue2 = 0;

MemorySearchScreen::MemorySearchScreen(const Path &gamePath)
	: UIBaseDialogScreen(gamePath) {
	if (!s_sharedEngine || s_lastGamePath != gamePath) {
		s_sharedEngine = std::make_unique<MemorySearchEngine>();
		s_lastGamePath = gamePath;
		s_searchTypeIndex = 1;
		s_compareTypeIndex = 0;
		s_regionIndex = 3;
		s_alignedSearch = false;
		s_searchValue = 0;
		s_searchValue2 = 0;

		SearchDataType types[] = {
			SearchDataType::TYPE_8BIT,
			SearchDataType::TYPE_16BIT,
			SearchDataType::TYPE_32BIT,
			SearchDataType::TYPE_64BIT,
			SearchDataType::TYPE_FLOAT,
			SearchDataType::TYPE_DOUBLE
		};
		MemoryRegion regions[] = {
			MemoryRegion::MAIN_RAM,
			MemoryRegion::VRAM,
			MemoryRegion::SCRATCHPAD,
			MemoryRegion::ALL
		};

		s_sharedEngine->SetDataType(types[s_searchTypeIndex]);
		s_sharedEngine->SetMemoryRegion(regions[s_regionIndex]);
		s_sharedEngine->SetCompareType(SearchCompareType::EXACT_VALUE);
		s_sharedEngine->SetAlignedSearch(s_alignedSearch);
	}
	searchEngine_ = s_sharedEngine.get();
}

MemorySearchScreen::~MemorySearchScreen() {
	if (searchThread_.joinable()) {
		searchCanceled_ = true;
		searchThread_.join();
	}
}

void MemorySearchScreen::CreateViews() {
	using namespace UI;

	auto di = GetI18NCategory(I18NCat::DIALOG);
	auto ms = GetI18NCategory(I18NCat::MEMORY_SEARCH);

	root_ = new AnchorLayout(new LayoutParams(FILL_PARENT, FILL_PARENT));

	LinearLayout *vertical = new LinearLayout(ORIENT_VERTICAL, new LayoutParams(FILL_PARENT, FILL_PARENT));
	vertical->SetSpacing(0.0f);
	root_->Add(vertical);

	// Top bar
	LinearLayout *topbar = new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
	topbar->SetSpacing(10.0f);
	vertical->Add(topbar);

	Choice *back = topbar->Add(new Choice(di->T("Back"), ImageID("I_NAVIGATE_BACK")));
	back->OnClick.Handle<UIScreen>(this, &UIScreen::OnBack);
	root_->SetDefaultFocusView(back);

	TextView *title = topbar->Add(new TextView(ms->T("MemorySearch", "Memory Search")));
	title->SetBig(true);
	title->SetShadow(true);

	// Main content area
	LinearLayout *content = new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, FILL_PARENT, 1.0f));
	content->SetSpacing(16.0f);
	vertical->Add(content);

	// Left column - Controls
	ScrollView *leftScroll = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(300, FILL_PARENT, 0.0f));
	LinearLayout *leftColumn = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
	leftColumn->SetSpacing(4.0f);
	leftScroll->Add(leftColumn);
	content->Add(leftScroll);

	leftColumn->Add(new ItemHeader(ms->T("DataType", "Data Type")));
	static const char *searchTypeChoices[] = { "8Bit", "16Bit", "32Bit", "64Bit", "Float", "Double" };
	searchTypeChoice_ = leftColumn->Add(new PopupMultiChoice(&s_searchTypeIndex, ms->T("DataType", "Data Type"), searchTypeChoices, 0, 6, I18NCat::MEMORY_SEARCH, screenManager()));
	searchTypeChoice_->OnChoice.Handle(this, &MemorySearchScreen::OnSearchType);

	leftColumn->Add(new ItemHeader(ms->T("SearchType", "Search Type")));
	static const char *compareTypeChoices[] = {
		"ExactValue", "GreaterThan", "LessThan", "GreaterOrEqual",
		"LessOrEqual", "Between", "Changed", "Unchanged", "Increased", "Decreased", "UnknownValue"
	};
	compareTypeChoice_ = leftColumn->Add(new PopupMultiChoice(&s_compareTypeIndex, ms->T("SearchType", "Search Type"), compareTypeChoices, 0, 11, I18NCat::MEMORY_SEARCH, screenManager()));
	compareTypeChoice_->OnChoice.Handle(this, &MemorySearchScreen::OnCompareType);

	searchValueHeader_ = leftColumn->Add(new ItemHeader(ms->T("SearchValue", "Search Value")));
	searchValueChoice_ = leftColumn->Add(new Choice("0"));
	searchValueChoice_->OnClick.Handle(this, &MemorySearchScreen::OnSearchValue);

	searchValue2Choice_ = leftColumn->Add(new Choice("0"));
	searchValue2Choice_->OnClick.Handle(this, &MemorySearchScreen::OnSearchValue2);
	searchValue2Choice_->SetVisibility(V_GONE);

	leftColumn->Add(new ItemHeader(ms->T("MemoryRegion", "Memory Region")));
	static const char *regionChoices[] = { "MainRAM", "VRAM", "Scratchpad", "AllMemory" };
	regionChoice_ = leftColumn->Add(new PopupMultiChoice(&s_regionIndex, ms->T("MemoryRegion", "Memory Region"), regionChoices, 0, 4, I18NCat::MEMORY_SEARCH, screenManager()));
	regionChoice_->OnChoice.Handle(this, &MemorySearchScreen::OnRegion);

	leftColumn->Add(new ItemHeader(ms->T("AlignedSearch", "Aligned Search")));
	alignedChoice_ = leftColumn->Add(new Choice(ms->T("AlignedOn", "Aligned (On)")));
	alignedChoice_->OnClick.Handle(this, &MemorySearchScreen::OnAlignedSearch);

	leftColumn->Add(new Spacer(16.0f));

	firstSearchButton_ = leftColumn->Add(new Choice(ms->T("FirstSearch", "First Search"), ImageID("I_SEARCH")));
	firstSearchButton_->OnClick.Handle(this, &MemorySearchScreen::OnFirstSearch);

	nextSearchButton_ = leftColumn->Add(new Choice(ms->T("NextSearch", "Next Search"), ImageID("I_SEARCH")));
	nextSearchButton_->OnClick.Handle(this, &MemorySearchScreen::OnNextSearch);

	Choice *refreshBtn = leftColumn->Add(new Choice(ms->T("RefreshValues", "Refresh Values")));
	refreshBtn->OnClick.Handle(this, &MemorySearchScreen::OnRefreshResults);

	leftColumn->Add(new Spacer(16.0f));

	Choice *unfreezeBtn = leftColumn->Add(new Choice(ms->T("UnfreezeAll", "Unfreeze All")));
	unfreezeBtn->OnClick.Handle(this, &MemorySearchScreen::OnUnfreezeAll);

	Choice *resetBtn = leftColumn->Add(new Choice(ms->T("ResetAll", "Reset All")));
	resetBtn->OnClick.Handle(this, &MemorySearchScreen::OnReset);

	Choice *clearBtn = leftColumn->Add(new Choice(ms->T("ClearResults", "Clear Results")));
	clearBtn->OnClick.Handle(this, &MemorySearchScreen::OnClearResults);

	// Right column - Results
	LinearLayout *rightColumn = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, FILL_PARENT, 1.0f));
	rightColumn->SetSpacing(4.0f);
	content->Add(rightColumn);

	resultCountText_ = rightColumn->Add(new TextView(ms->T("ResultsCount", "0 results")));
	resultCountText_->SetSmall(true);

	searchingText_ = rightColumn->Add(new TextView(""));
	searchingText_->SetVisibility(UI::V_GONE);

	resultsScroll_ = rightColumn->Add(new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, FILL_PARENT, 1.0f)));
	resultsList_ = new LinearLayoutList(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
	resultsScroll_->Add(resultsList_);

	UpdateControlLabels();
	if (!searchEngine_->GetResults().empty() || searchEngine_->IsBitmapMode()) {
		if (searchEngine_->GetResultCount() > 0) {
			nextSearchButton_->SetEnabled(true);
		}
	}
	UpdateResultsList();
}

void MemorySearchScreen::update() {
	UIBaseDialogScreen::update();
	
	if (!searchInProgress_.load() && pendingResultCount_.load() > 0) {
		pendingResultCount_.store(0);
		nextSearchButton_->SetEnabled(true);
		firstSearchButton_->SetEnabled(true);
		searchingText_->SetVisibility(UI::V_GONE);
		UpdateResultsList();
	}
}

void MemorySearchScreen::dialogFinished(const Screen *dialog, DialogResult result) {
	UIBaseDialogScreen::dialogFinished(dialog, result);
}

void MemorySearchScreen::OnSearchType(UI::EventParams &e) {
	SearchDataType types[] = {
		SearchDataType::TYPE_8BIT,
		SearchDataType::TYPE_16BIT,
		SearchDataType::TYPE_32BIT,
		SearchDataType::TYPE_64BIT,
		SearchDataType::TYPE_FLOAT,
		SearchDataType::TYPE_DOUBLE
	};
	searchEngine_->SetDataType(types[s_searchTypeIndex]);
	nextSearchButton_->SetEnabled(false);
}

void MemorySearchScreen::OnCompareType(UI::EventParams &e) {
	SearchCompareType compares[] = {
		SearchCompareType::EXACT_VALUE,
		SearchCompareType::GREATER_THAN,
		SearchCompareType::LESS_THAN,
		SearchCompareType::GREATER_OR_EQUAL,
		SearchCompareType::LESS_OR_EQUAL,
		SearchCompareType::BETWEEN,
		SearchCompareType::CHANGED,
		SearchCompareType::UNCHANGED,
		SearchCompareType::INCREASED,
		SearchCompareType::DECREASED,
		SearchCompareType::UNKNOWN_VALUE
	};
	searchEngine_->SetCompareType(compares[s_compareTypeIndex]);
	searchValue2Choice_->SetVisibility(s_compareTypeIndex == 5 ? UI::V_VISIBLE : UI::V_GONE);
	bool hideSearchValue = s_compareTypeIndex >= 6;
	searchValueChoice_->SetVisibility(hideSearchValue ? UI::V_GONE : UI::V_VISIBLE);
	searchValueHeader_->SetVisibility(hideSearchValue ? UI::V_GONE : UI::V_VISIBLE);
}

void MemorySearchScreen::OnRegion(UI::EventParams &e) {
	MemoryRegion regions[] = {
		MemoryRegion::MAIN_RAM,
		MemoryRegion::VRAM,
		MemoryRegion::SCRATCHPAD,
		MemoryRegion::ALL
	};
	searchEngine_->SetMemoryRegion(regions[s_regionIndex]);
	nextSearchButton_->SetEnabled(false);
}

void MemorySearchScreen::OnAlignedSearch(UI::EventParams &e) {
	s_alignedSearch = !s_alignedSearch;
	searchEngine_->SetAlignedSearch(s_alignedSearch);

	auto ms = GetI18NCategory(I18NCat::MEMORY_SEARCH);
	alignedChoice_->SetText(s_alignedSearch ?
		ms->T("AlignedOn", "Aligned (On)") :
		ms->T("AlignedOff", "Unaligned (Off)"));
	nextSearchButton_->SetEnabled(false);
}

void MemorySearchScreen::OnSearchValue(UI::EventParams &e) {
	auto ms = GetI18NCategory(I18NCat::MEMORY_SEARCH);
	UI::AskForInput(screenManager(), GetRequesterToken(), e.v, ms->T("EnterSearchValue", "Enter Search Value"),
		[this](const std::string &text, bool success) {
			if (success && !text.empty()) {
				s_searchValue = std::strtoll(text.c_str(), nullptr, 10);
				searchEngine_->SetSearchValue(s_searchValue);
				searchValueChoice_->SetText(StringFromFormat("%lld", (long long)s_searchValue));
			}
		});
}

void MemorySearchScreen::OnSearchValue2(UI::EventParams &e) {
	auto ms = GetI18NCategory(I18NCat::MEMORY_SEARCH);
	UI::AskForInput(screenManager(), GetRequesterToken(), e.v, ms->T("EnterSecondValue", "Enter Second Value"),
		[this](const std::string &text, bool success) {
			if (success && !text.empty()) {
				s_searchValue2 = std::strtoll(text.c_str(), nullptr, 10);
				searchEngine_->SetRange(s_searchValue, s_searchValue2);
				searchValue2Choice_->SetText(StringFromFormat("%lld", (long long)s_searchValue2));
			}
		});
}

void MemorySearchScreen::OnFirstSearch(UI::EventParams &e) {
	if (searchInProgress_.load()) return;
	
	if (searchThread_.joinable())
		searchThread_.join();
	
	searchCanceled_ = false;
	searchInProgress_.store(true);
	firstSearchButton_->SetEnabled(false);
	nextSearchButton_->SetEnabled(false);
	searchingText_->SetText("Searching...");
	searchingText_->SetVisibility(UI::V_VISIBLE);
	pendingIsFirstSearch_ = true;
	
	searchThread_ = std::thread([this]() {
		size_t count = searchEngine_->FirstSearch();
		pendingResultCount_.store(count);
		searchInProgress_.store(false);
	});
}

void MemorySearchScreen::OnNextSearch(UI::EventParams &e) {
	if (searchInProgress_.load()) return;
	
	if (searchThread_.joinable())
		searchThread_.join();
	
	searchCanceled_ = false;
	searchInProgress_.store(true);
	firstSearchButton_->SetEnabled(false);
	nextSearchButton_->SetEnabled(false);
	searchingText_->SetText("Searching...");
	searchingText_->SetVisibility(UI::V_VISIBLE);
	pendingIsFirstSearch_ = false;
	
	searchThread_ = std::thread([this]() {
		size_t count = searchEngine_->NextSearch();
		pendingResultCount_.store(count);
		searchInProgress_.store(false);
	});
}

void MemorySearchScreen::OnClearResults(UI::EventParams &e) {
	searchEngine_->ClearResults();
	nextSearchButton_->SetEnabled(false);
	UpdateResultsList();
}

void MemorySearchScreen::OnReset(UI::EventParams &e) {
	s_searchTypeIndex = 1;
	s_compareTypeIndex = 0;
	s_regionIndex = 3;
	s_alignedSearch = false;
	s_searchValue = 0;
	s_searchValue2 = 0;

	SearchDataType types[] = {
		SearchDataType::TYPE_8BIT,
		SearchDataType::TYPE_16BIT,
		SearchDataType::TYPE_32BIT,
		SearchDataType::TYPE_64BIT,
		SearchDataType::TYPE_FLOAT,
		SearchDataType::TYPE_DOUBLE
	};
	SearchCompareType compares[] = {
		SearchCompareType::EXACT_VALUE,
		SearchCompareType::GREATER_THAN,
		SearchCompareType::LESS_THAN,
		SearchCompareType::GREATER_OR_EQUAL,
		SearchCompareType::LESS_OR_EQUAL,
		SearchCompareType::BETWEEN,
		SearchCompareType::CHANGED,
		SearchCompareType::UNCHANGED,
		SearchCompareType::INCREASED,
		SearchCompareType::DECREASED,
		SearchCompareType::UNKNOWN_VALUE
	};
	MemoryRegion regions[] = {
		MemoryRegion::MAIN_RAM,
		MemoryRegion::VRAM,
		MemoryRegion::SCRATCHPAD,
		MemoryRegion::ALL
	};

	searchEngine_->SetDataType(types[s_searchTypeIndex]);
	searchEngine_->SetCompareType(compares[s_compareTypeIndex]);
	searchEngine_->SetMemoryRegion(regions[s_regionIndex]);
	searchEngine_->SetAlignedSearch(s_alignedSearch);
	searchEngine_->SetSearchValue(s_searchValue);
	searchEngine_->SetRange(s_searchValue, s_searchValue2);
	searchEngine_->ClearResults();
	searchEngine_->UnfreezeAll();

	nextSearchButton_->SetEnabled(false);
	UpdateControlLabels();
	UpdateResultsList();
}

void MemorySearchScreen::OnUnfreezeAll(UI::EventParams &e) {
	searchEngine_->UnfreezeAll();
}

void MemorySearchScreen::OnRefreshResults(UI::EventParams &e) {
	searchEngine_->RefreshResults();
	UpdateResultsList();
}

void MemorySearchScreen::OnResultClick(int index) {
	const SearchResult *r = searchEngine_->GetResultAt(index);
	if (!r) return;

	auto ms = GetI18NCategory(I18NCat::MEMORY_SEARCH);
	std::string title = StringFromFormat(std::string(ms->T("ModifyAddress", "Modify 0x%08X")).c_str(), r->address);
	UI::AskForInput(screenManager(), GetRequesterToken(), nullptr, title,
		[this, index](const std::string &text, bool success) {
			if (success && !text.empty()) {
				int64_t newValue = std::strtoll(text.c_str(), nullptr, 10);
				searchEngine_->ModifyValue(searchEngine_->GetResultAt(index)->address, newValue);
				UpdateResultsList();
			}
		});
}

void MemorySearchScreen::UpdateControlLabels() {
	auto ms = GetI18NCategory(I18NCat::MEMORY_SEARCH);

	alignedChoice_->SetText(s_alignedSearch ?
		ms->T("AlignedOn", "Aligned (On)") :
		ms->T("AlignedOff", "Unaligned (Off)"));
	searchValueChoice_->SetText(StringFromFormat("%lld", (long long)s_searchValue));
	searchValue2Choice_->SetText(StringFromFormat("%lld", (long long)s_searchValue2));
	searchValue2Choice_->SetVisibility(s_compareTypeIndex == 5 ? UI::V_VISIBLE : UI::V_GONE);
	bool hideSearchValue = s_compareTypeIndex >= 6;
	searchValueChoice_->SetVisibility(hideSearchValue ? UI::V_GONE : UI::V_VISIBLE);
	searchValueHeader_->SetVisibility(hideSearchValue ? UI::V_GONE : UI::V_VISIBLE);
}

void MemorySearchScreen::UpdateResultsList() {
	using namespace UI;

	if (!resultsList_) return;

	resultsList_->Clear();

	auto ms = GetI18NCategory(I18NCat::MEMORY_SEARCH);

	size_t resultCount = searchEngine_->GetResultCount();
	resultCountText_->SetText(StringFromFormat(std::string(ms->T("ResultsCount", "%zu results")).c_str(), resultCount));

	if (searchEngine_->IsBitmapMode()) {
		std::string msg = StringFromFormat("Too many results to display (%zu). Continue narrowing down your search to see individual results.", resultCount);
		resultsList_->Add(new TextView(msg));
		return;
	}

	const auto &results = searchEngine_->GetResults();

	if (results.empty()) {
		resultsList_->Add(new TextView(std::string(ms->T("NoResults", "No results found. Start a search!"))));
		return;
	}

	size_t maxResults = std::min(results.size(), static_cast<size_t>(500));

	for (size_t i = 0; i < maxResults; ++i) {
		const auto &result = results[i];

		LinearLayout *row = new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
		row->SetSpacing(8.0f);

		std::string addrStr = StringFromFormat("0x%08X", result.address);
		TextView *addrText = row->Add(new TextView(addrStr, new LinearLayoutParams(140, WRAP_CONTENT)));
		addrText->SetSmall(true);

		std::string valueStr;
		std::string prevValueStr;
		std::string typeStr;
		bool valueChanged = false;

		switch (result.type) {
		case SearchDataType::TYPE_8BIT: {
			uint8_t cur = static_cast<uint8_t>(result.value.intValue);
			uint8_t prev = static_cast<uint8_t>(result.prevValue.intPrevValue);
			valueStr = StringFromFormat("%u", cur);
			prevValueStr = StringFromFormat("%u", prev);
			typeStr = "U8";
			valueChanged = (cur != prev);
			break;
		}
		case SearchDataType::TYPE_16BIT: {
			uint16_t cur = static_cast<uint16_t>(result.value.intValue);
			uint16_t prev = static_cast<uint16_t>(result.prevValue.intPrevValue);
			valueStr = StringFromFormat("%u", cur);
			prevValueStr = StringFromFormat("%u", prev);
			typeStr = "U16";
			valueChanged = (cur != prev);
			break;
		}
		case SearchDataType::TYPE_32BIT: {
			uint32_t cur = static_cast<uint32_t>(result.value.intValue);
			uint32_t prev = static_cast<uint32_t>(result.prevValue.intPrevValue);
			valueStr = StringFromFormat("%u", cur);
			prevValueStr = StringFromFormat("%u", prev);
			typeStr = "U32";
			valueChanged = (cur != prev);
			break;
		}
		case SearchDataType::TYPE_64BIT: {
			uint64_t cur = static_cast<uint64_t>(result.value.intValue);
			uint64_t prev = static_cast<uint64_t>(result.prevValue.intPrevValue);
			valueStr = StringFromFormat("%llu", (unsigned long long)cur);
			prevValueStr = StringFromFormat("%llu", (unsigned long long)prev);
			typeStr = "U64";
			valueChanged = (cur != prev);
			break;
		}
		case SearchDataType::TYPE_FLOAT:
			valueStr = StringFromFormat("%.4f", result.value.floatValue);
			prevValueStr = StringFromFormat("%.4f", result.prevValue.floatPrevValue);
			typeStr = "F32";
			valueChanged = (result.value.floatValue != result.prevValue.floatPrevValue);
			break;
		case SearchDataType::TYPE_DOUBLE:
			valueStr = StringFromFormat("%.6f", result.value.floatValue);
			prevValueStr = StringFromFormat("%.6f", result.prevValue.floatPrevValue);
			typeStr = "F64";
			valueChanged = (result.value.floatValue != result.prevValue.floatPrevValue);
			break;
		default:
			valueStr = "???";
			prevValueStr = "???";
			typeStr = "???";
			break;
		}

		std::string displayValue = valueStr;
		if (valueChanged) {
			displayValue = StringFromFormat("%s <- %s", valueStr.c_str(), prevValueStr.c_str());
		}

		TextView *valueText = row->Add(new TextView(displayValue, new LinearLayoutParams(200, WRAP_CONTENT)));
		valueText->SetSmall(true);

		TextView *typeText = row->Add(new TextView(typeStr, new LinearLayoutParams(60, WRAP_CONTENT)));
		typeText->SetSmall(true);

		row->Add(new Spacer(new LinearLayoutParams(1.0f)));

		Choice *modifyBtn = row->Add(new Choice(std::string(ms->T("Modify", "Modify")), new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT)));
		modifyBtn->OnClick.Add([this, i](UI::EventParams &e) {
			OnResultClick((int)i);
		});

		Choice *addCheatBtn = row->Add(new Choice(std::string(ms->T("AddCheat", "Add Cheat")), new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT)));
		addCheatBtn->OnClick.Add([this, i](UI::EventParams &e) {
			OnAddCheat((int)i);
		});

		resultsList_->Add(row);
	}

	if (results.size() > 500) {
		resultsList_->Add(new TextView(StringFromFormat(std::string(ms->T("MoreResults", "... and %zu more results")).c_str(), results.size() - 500)));
	}
}

void MemorySearchScreen::OnAddCheat(int index) {
	const SearchResult *r = searchEngine_->GetResultAt(index);
	if (!r) return;

	auto ms = GetI18NCategory(I18NCat::MEMORY_SEARCH);
	std::string title = StringFromFormat(std::string(ms->T("AddCheatTitle", "Add Cheat at 0x%08X")).c_str(), r->address);

	pendingResultIndex_ = index;
	pendingDialogType_ = 2;

	UI::AskForInput(screenManager(), GetRequesterToken(), nullptr, title,
		[this](const std::string &text, bool success) {
			if (success && !text.empty() && pendingResultIndex_ >= 0) {
				const SearchResult *r = searchEngine_->GetResultAt(pendingResultIndex_);
				if (r) {
					std::string gameID;
					auto info = g_gameInfoCache->GetInfo(nullptr, gamePath_, GameInfoFlags::PARAM_SFO);
					if (info && info->Ready(GameInfoFlags::PARAM_SFO)) {
						gameID = info->GetParamSFO().GetValueString("DISC_ID");
					}
					if (gameID.empty()) {
						gameID = g_paramSFO.GenerateFakeID(gamePath_);
					}

					CWCheatEngine engine(gameID);
					if (!engine.HasCheats()) {
						engine.CreateCheatFile();
					}

					uint32_t cheatAddr = r->address - 0x08800000;
					uint32_t cheatType = 0;
					uint32_t cheatValue = 0;

					switch (r->type) {
					case SearchDataType::TYPE_8BIT:
						cheatType = 0x00000000;
						cheatValue = (uint32_t)(uint8_t)r->value.intValue;
						break;
					case SearchDataType::TYPE_16BIT:
						cheatType = 0x10000000;
						cheatValue = (uint32_t)(uint16_t)r->value.intValue;
						break;
					case SearchDataType::TYPE_32BIT:
						cheatType = 0x20000000;
						cheatValue = (uint32_t)r->value.intValue;
						break;
					default:
						return;
					}

					uint32_t cheatCode = cheatType | (cheatAddr & 0x0FFFFFFF);

					FILE *fp = File::OpenCFile(engine.CheatFilename(), "at");
					if (fp) {
						fprintf(fp, "_C0 %s\n", text.c_str());
						fprintf(fp, "_L 0x%08X 0x%08X\n", cheatCode, cheatValue);
						fclose(fp);

						g_Config.bReloadCheats = true;

						auto ms = GetI18NCategory(I18NCat::MEMORY_SEARCH);
						auto di = GetI18NCategory(I18NCat::DIALOG);
						screenManager()->push(new UI::MessagePopupScreen(
							di->T("Success"),
							StringFromFormat(std::string(ms->T("CheatAdded", "Cheat '%s' added successfully!")).c_str(), text.c_str()),
							di->T("OK"), "", nullptr));
					}
				}
			}
			pendingResultIndex_ = -1;
			pendingDialogType_ = 0;
		});
}

void MemorySearchScreen::RefreshViews() {
	RecreateViews();
}
