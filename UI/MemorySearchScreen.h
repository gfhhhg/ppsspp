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

#pragma once

#include <memory>
#include <thread>
#include <atomic>
#include "Common/UI/View.h"
#include "Common/UI/UIScreen.h"
#include "Common/UI/Context.h"
#include "UI/BaseScreens.h"
#include "Core/MemorySearch.h"

class MemorySearchScreen : public UIBaseDialogScreen {
public:
	MemorySearchScreen(const Path &gamePath);
	~MemorySearchScreen();

	void CreateViews() override;
	void update() override;

	const char* tag() const override { return "MemorySearchScreen"; }

protected:
	void dialogFinished(const Screen *dialog, DialogResult result) override;

private:
	void OnSearchType(UI::EventParams &e);
	void OnCompareType(UI::EventParams &e);
	void OnRegion(UI::EventParams &e);
	void OnAlignedSearch(UI::EventParams &e);
	void OnSearchValue(UI::EventParams &e);
	void OnSearchValue2(UI::EventParams &e);
	void OnFirstSearch(UI::EventParams &e);
	void OnNextSearch(UI::EventParams &e);
	void OnClearResults(UI::EventParams &e);
	void OnReset(UI::EventParams &e);
	void OnUnfreezeAll(UI::EventParams &e);
	void OnRefreshResults(UI::EventParams &e);
	void OnResultClick(int index);
	void OnAddCheat(int index);

	void UpdateResultsList();
	void UpdateControlLabels();
	void RefreshViews();

	MemorySearchEngine *searchEngine_ = nullptr;

	UI::PopupMultiChoice *searchTypeChoice_ = nullptr;
	UI::PopupMultiChoice *compareTypeChoice_ = nullptr;
	UI::PopupMultiChoice *regionChoice_ = nullptr;
	UI::Choice *alignedChoice_ = nullptr;
	UI::ItemHeader *searchValueHeader_ = nullptr;
	UI::Choice *searchValueChoice_ = nullptr;
	UI::Choice *searchValue2Choice_ = nullptr;
	UI::Choice *firstSearchButton_ = nullptr;
	UI::Choice *nextSearchButton_ = nullptr;
	UI::TextView *resultCountText_ = nullptr;
	UI::ScrollView *resultsScroll_ = nullptr;
	UI::LinearLayout *resultsList_ = nullptr;
	UI::TextView *searchingText_ = nullptr;

	int pendingDialogType_ = 0;
	int pendingResultIndex_ = -1;

	std::thread searchThread_;
	std::atomic<bool> searchCanceled_{false};
	std::atomic<bool> searchInProgress_{false};
	std::atomic<size_t> pendingResultCount_{0};
	bool pendingIsFirstSearch_ = false;

	static std::unique_ptr<MemorySearchEngine> s_sharedEngine;
	static Path s_lastGamePath;
	static int s_searchTypeIndex;
	static int s_compareTypeIndex;
	static int s_regionIndex;
	static bool s_alignedSearch;
	static int64_t s_searchValue;
	static int64_t s_searchValue2;
};
