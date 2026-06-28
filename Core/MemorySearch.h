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

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <cstdio>

enum class SearchDataType {
	TYPE_8BIT,
	TYPE_16BIT,
	TYPE_32BIT,
	TYPE_64BIT,
	TYPE_FLOAT,
	TYPE_DOUBLE,
	TYPE_STRING,
};

enum class SearchCompareType {
	EXACT_VALUE,
	GREATER_THAN,
	LESS_THAN,
	GREATER_OR_EQUAL,
	LESS_OR_EQUAL,
	BETWEEN,
	CHANGED,
	UNCHANGED,
	INCREASED,
	DECREASED,
	UNKNOWN_VALUE,
};

enum class MemoryRegion {
	MAIN_RAM,
	VRAM,
	SCRATCHPAD,
	ALL,
};

struct SearchResult {
	uint32_t address;
	SearchDataType type;
	union {
		int64_t intValue;
		double floatValue;
		std::string* stringValue;
	} value;

	union {
		int64_t intPrevValue;
		double floatPrevValue;
	} prevValue;

	SearchResult() : address(0), type(SearchDataType::TYPE_32BIT), value({0}), prevValue({0}) {}
};

class MemorySearchEngine {
public:
	MemorySearchEngine();
	~MemorySearchEngine();

	void SetDataType(SearchDataType type);
	void SetMemoryRegion(MemoryRegion region);
	void SetCompareType(SearchCompareType compareType);
	void SetAlignedSearch(bool aligned) { alignedSearch_ = aligned; }
	bool GetAlignedSearch() const { return alignedSearch_; }

	void SetSearchValue(int64_t value);
	void SetSearchValue(double value);
	void SetSearchValue(const std::string& value);

	void SetRange(int64_t minVal, int64_t maxVal);
	void SetRange(double minVal, double maxVal);

	size_t FirstSearch();
	size_t NextSearch();

	const std::vector<SearchResult>& GetResults() const { return results_; }
	size_t GetResultCount() const { return useBitmapMode_ ? bitmapResultCount_ : results_.size(); }
	bool IsBitmapMode() const { return useBitmapMode_; }
	const SearchResult* GetResultAt(size_t index) const;

	bool ModifyValue(uint32_t address, int64_t newValue);
	bool ModifyValue(uint32_t address, double newValue);
	bool ModifyValue(uint32_t address, const std::string& newValue);

	void FreezeAddress(uint32_t address, int64_t value);
	void UnfreezeAddress(uint32_t address);
	void UnfreezeAll();
	bool IsAddressFrozen(uint32_t address) const;
	int64_t GetFrozenValue(uint32_t address) const;

	void ClearResults();
	void SaveCurrentValues();
	void LoadPreviousValues();
	void RefreshResults();

	size_t GetSearchedByteCount() const { return searchedBytes_; }
	MemoryRegion GetCurrentRegion() const { return currentRegion_; }

	static void Log(const char* format, ...);

private:
	size_t SearchMainRAM();
	size_t SearchVRAM();
	size_t SearchScratchPad();
	size_t GetDataTypeAlignment() const;
	size_t GetDataTypeSize() const;
	size_t GetSearchStep() const;

	bool ApplyCompare(int64_t currentVal, int64_t prevVal);
	bool ApplyCompare(double currentVal, double prevVal);

	int64_t ReadIntValue(uint32_t address);
	double ReadFloatValue(uint32_t address);
	std::string ReadStringValue(uint32_t address, size_t maxLen = 256);

	bool IsValidAddress(uint32_t address) const;
	uint32_t GetRegionStart() const;
	uint32_t GetRegionEnd() const;
	bool IsFuzzySearch() const;

	SearchDataType dataType_;
	SearchCompareType compareType_;
	MemoryRegion currentRegion_;
	bool alignedSearch_ = false;

	int64_t searchIntValue_;
	int64_t searchIntValue2_;
	double searchFloatValue_;
	double searchFloatValue2_;
	std::string searchStringValue_;

	std::vector<SearchResult> results_;
	std::vector<SearchResult> previousResults_;
	std::vector<SearchResult> frozenValues_;

	std::vector<uint8_t> memorySnapshot_;
	FILE* fileSnapshot_ = nullptr;

	bool useBitmapMode_ = false;
	std::vector<uint8_t> resultBitmap_;
	std::vector<uint8_t> previousBitmap_;
	uint32_t bitmapBaseAddr_ = 0;
	uint32_t bitmapEndAddr_ = 0;
	size_t bitmapStep_ = 1;
	size_t bitmapResultCount_ = 0;

	static const size_t MAX_BITMAP_RESULT_COUNT = 100000;

	size_t searchedBytes_;

	void BitmapSet(uint32_t addr);
	void BitmapClear(uint32_t addr);
	bool BitmapTest(uint32_t addr) const;
	size_t BitmapCount() const;
	void InitBitmap(uint32_t baseAddr, uint32_t endAddr, size_t step);
	void SaveBitmapSnapshot();
	void ExpandBitmapToResults();
	size_t FilterBitmapWithCompare();
};
