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
#include "Core/MemorySearch.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Common/Log.h"
#include "Common/File/FileUtil.h"
#include "Core/System.h"
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <cerrno>
#include <map>

#include "Core/Util/PathUtil.h"

static std::string GetTempDirectory() {
	Path cacheDir = GetSysDirectory(DIRECTORY_APP_CACHE);
	File::CreateFullPath(cacheDir);
	return cacheDir.ToString();
}

static std::mutex g_logMutex;

void MemorySearchEngine::Log(const char* format, ...) {
	std::lock_guard<std::mutex> lock(g_logMutex);

	Path logDir = GetSysDirectory(DIRECTORY_DUMP);
	File::CreateFullPath(logDir);
	Path logPath = logDir / "memory_search_log.txt";

	File::IOFile logFile(logPath, "a");
	if (!logFile.IsOpen()) {
		return;
	}

	time_t now = time(nullptr);
	struct tm tmNow;
	localtime_r(&now, &tmNow);
	char timeBuf[64];
	snprintf(timeBuf, sizeof(timeBuf), "[%04d-%02d-%02d %02d:%02d:%02d] ",
		tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
		tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);
	logFile.WriteBytes(timeBuf, strlen(timeBuf));

	va_list args;
	va_start(args, format);
	char buf[1024];
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	logFile.WriteBytes(buf, strlen(buf));

	const char* newline = "\n";
	logFile.WriteBytes(newline, 1);
	logFile.Flush();
	logFile.Close();
}

MemorySearchEngine::MemorySearchEngine()
	: dataType_(SearchDataType::TYPE_16BIT),
	  compareType_(SearchCompareType::EXACT_VALUE),
	  currentRegion_(MemoryRegion::ALL),
	  searchIntValue_(0),
	  searchIntValue2_(0),
	  searchFloatValue_(0.0),
	  searchFloatValue2_(0.0),
	  searchedBytes_(0) {
}

MemorySearchEngine::~MemorySearchEngine() {
	if (fileSnapshot_) {
		fclose(fileSnapshot_);
		fileSnapshot_ = nullptr;
	}
	ClearResults();
	UnfreezeAll();
}

void MemorySearchEngine::SetDataType(SearchDataType type) {
	dataType_ = type;
	ClearResults();
}

void MemorySearchEngine::SetMemoryRegion(MemoryRegion region) {
	currentRegion_ = region;
	ClearResults();
}

void MemorySearchEngine::SetCompareType(SearchCompareType compareType) {
	compareType_ = compareType;
}

void MemorySearchEngine::SetSearchValue(int64_t value) {
	searchIntValue_ = value;
}

void MemorySearchEngine::SetSearchValue(double value) {
	searchFloatValue_ = value;
}

void MemorySearchEngine::SetSearchValue(const std::string& value) {
	searchStringValue_ = value;
}

void MemorySearchEngine::SetRange(int64_t minVal, int64_t maxVal) {
	searchIntValue_ = minVal;
	searchIntValue2_ = maxVal;
}

void MemorySearchEngine::SetRange(double minVal, double maxVal) {
	searchFloatValue_ = minVal;
	searchFloatValue2_ = maxVal;
}

bool MemorySearchEngine::IsValidAddress(uint32_t address) const {
	size_t dataSize = GetDataTypeSize();
	uint32_t endAddr = address + (uint32_t)dataSize;
	// Prevent integer overflow
	if (endAddr < address) {
		return false;
	}

	if (address >= 0x08000000 && endAddr <= 0x08000000 + Memory::g_MemorySize) {
		return true;
	}
	if (address >= 0x04000000 && endAddr <= 0x04600000) {
		return true;
	}
	if (address >= 0x00010000 && endAddr <= 0x00014000) {
		return true;
	}
	return false;
}

uint32_t MemorySearchEngine::GetRegionStart() const {
	switch (currentRegion_) {
	case MemoryRegion::MAIN_RAM:
		return 0x08000000;
	case MemoryRegion::VRAM:
		return 0x04000000;
	case MemoryRegion::SCRATCHPAD:
		return 0x00010000;
	case MemoryRegion::ALL:
	default:
		return 0x00010000;
	}
}

uint32_t MemorySearchEngine::GetRegionEnd() const {
	switch (currentRegion_) {
	case MemoryRegion::MAIN_RAM:
		return 0x08000000 + Memory::g_MemorySize;
	case MemoryRegion::VRAM:
		return 0x04600000;
	case MemoryRegion::SCRATCHPAD:
		return 0x00014000;
	case MemoryRegion::ALL:
	default:
		return 0x08000000 + Memory::g_MemorySize;
	}
}

bool MemorySearchEngine::IsFuzzySearch() const {
	switch (compareType_) {
	case SearchCompareType::CHANGED:
	case SearchCompareType::UNCHANGED:
	case SearchCompareType::INCREASED:
	case SearchCompareType::DECREASED:
	case SearchCompareType::UNKNOWN_VALUE:
		return true;
	default:
		return false;
	}
}

int64_t MemorySearchEngine::ReadIntValue(uint32_t address) {
	if (!IsValidAddress(address)) {
		return 0;
	}

	switch (dataType_) {
	case SearchDataType::TYPE_8BIT:
		return Memory::ReadUnchecked_U8(address);
	case SearchDataType::TYPE_16BIT:
		return Memory::ReadUnchecked_U16(address);
	case SearchDataType::TYPE_32BIT:
		return Memory::ReadUnchecked_U32(address);
	case SearchDataType::TYPE_64BIT: {
		const uint8_t* ptr = Memory::GetPointerUnchecked(address);
		uint64_t val;
		memcpy(&val, ptr, sizeof(uint64_t));
		return val;
	}
	default:
		return 0;
	}
}

double MemorySearchEngine::ReadFloatValue(uint32_t address) {
	if (!IsValidAddress(address)) {
		return 0.0;
	}

	switch (dataType_) {
	case SearchDataType::TYPE_FLOAT:
		return Memory::ReadUnchecked_Float(address);
	case SearchDataType::TYPE_DOUBLE: {
		const uint8_t* ptr = Memory::GetPointerUnchecked(address);
		uint64_t u64val;
		memcpy(&u64val, ptr, sizeof(uint64_t));
		double dval;
		memcpy(&dval, &u64val, sizeof(dval));
		return dval;
	}
	default:
		return 0.0;
	}
}

std::string MemorySearchEngine::ReadStringValue(uint32_t address, size_t maxLen) {
	if (!IsValidAddress(address)) {
		return "";
	}

	const uint8_t* ptr = Memory::GetPointer(address);
	if (!ptr) {
		return "";
	}

	std::string result;
	for (size_t i = 0; i < maxLen; ++i) {
		uint8_t ch = ptr[i];
		if (ch == 0) break;
		if (ch >= 32 && ch < 127) {
			result += static_cast<char>(ch);
		} else {
			break;
		}
	}
	return result;
}

bool MemorySearchEngine::ApplyCompare(int64_t currentVal, int64_t prevVal) {
	switch (compareType_) {
	case SearchCompareType::EXACT_VALUE:
		return currentVal == searchIntValue_;
	case SearchCompareType::GREATER_THAN:
		return currentVal > searchIntValue_;
	case SearchCompareType::LESS_THAN:
		return currentVal < searchIntValue_;
	case SearchCompareType::GREATER_OR_EQUAL:
		return currentVal >= searchIntValue_;
	case SearchCompareType::LESS_OR_EQUAL:
		return currentVal <= searchIntValue_;
	case SearchCompareType::BETWEEN:
		return currentVal >= searchIntValue_ && currentVal <= searchIntValue2_;
	case SearchCompareType::CHANGED:
		return currentVal != prevVal;
	case SearchCompareType::UNCHANGED:
		return currentVal == prevVal;
	case SearchCompareType::INCREASED:
		return currentVal > prevVal;
	case SearchCompareType::DECREASED:
		return currentVal < prevVal;
	case SearchCompareType::UNKNOWN_VALUE:
		return true;
	default:
		return false;
	}
}

bool MemorySearchEngine::ApplyCompare(double currentVal, double prevVal) {
	switch (compareType_) {
	case SearchCompareType::EXACT_VALUE:
		return currentVal == searchFloatValue_;
	case SearchCompareType::GREATER_THAN:
		return currentVal > searchFloatValue_;
	case SearchCompareType::LESS_THAN:
		return currentVal < searchFloatValue_;
	case SearchCompareType::GREATER_OR_EQUAL:
		return currentVal >= searchFloatValue_;
	case SearchCompareType::LESS_OR_EQUAL:
		return currentVal <= searchFloatValue_;
	case SearchCompareType::BETWEEN:
		return currentVal >= searchFloatValue_ && currentVal <= searchFloatValue2_;
	case SearchCompareType::CHANGED:
		return currentVal != prevVal;
	case SearchCompareType::UNCHANGED:
		return currentVal == prevVal;
	case SearchCompareType::INCREASED:
		return currentVal > prevVal;
	case SearchCompareType::DECREASED:
		return currentVal < prevVal;
	case SearchCompareType::UNKNOWN_VALUE:
		return true;
	default:
		return false;
	}
}

size_t MemorySearchEngine::GetDataTypeAlignment() const {
	switch (dataType_) {
	case SearchDataType::TYPE_8BIT:
		return 1;
	case SearchDataType::TYPE_16BIT:
		return 2;
	case SearchDataType::TYPE_32BIT:
	case SearchDataType::TYPE_FLOAT:
		return 4;
	case SearchDataType::TYPE_64BIT:
	case SearchDataType::TYPE_DOUBLE:
		return 8;
	case SearchDataType::TYPE_STRING:
		return 1;
	default:
		return 4;
	}
}

size_t MemorySearchEngine::GetDataTypeSize() const {
	switch (dataType_) {
	case SearchDataType::TYPE_8BIT:
		return 1;
	case SearchDataType::TYPE_16BIT:
		return 2;
	case SearchDataType::TYPE_32BIT:
	case SearchDataType::TYPE_FLOAT:
		return 4;
	case SearchDataType::TYPE_64BIT:
	case SearchDataType::TYPE_DOUBLE:
		return 8;
	case SearchDataType::TYPE_STRING:
		return 1;
	default:
		return 4;
	}
}

size_t MemorySearchEngine::GetSearchStep() const {
	if (alignedSearch_) {
		return GetDataTypeAlignment();
	}
	return 1;
}

size_t MemorySearchEngine::SearchMainRAM() {
	uint32_t start = 0x08000000;
	uint32_t end = 0x08000000 + Memory::g_MemorySize;
	size_t count = 0;

	bool isFloatType = (dataType_ == SearchDataType::TYPE_FLOAT || dataType_ == SearchDataType::TYPE_DOUBLE);
	bool hasPrevious = !previousResults_.empty();
	size_t step = GetSearchStep();

	if (!hasPrevious) {
		Log("SearchMainRAM: scanning full memory from 0x%08X to 0x%08X, step=%zu, aligned=%s", start, end, step, alignedSearch_ ? "true" : "false");
		bool fuzzySearch = IsFuzzySearch();
		for (uint32_t addr = start; addr < end; addr = (uint32_t)(addr + step)) {
			if (!IsValidAddress(addr)) continue;

			bool matched = false;
			SearchResult result;
			result.address = addr;
			result.type = dataType_;

			if (fuzzySearch) {
				// Fuzzy search on first scan: match all valid addresses
				if (isFloatType) {
					result.value.floatValue = ReadFloatValue(addr);
					result.prevValue.floatPrevValue = 0.0;
				} else {
					result.value.intValue = ReadIntValue(addr);
					result.prevValue.intPrevValue = 0;
				}
				matched = true;
			} else if (isFloatType) {
				double currentVal = ReadFloatValue(addr);
				if (ApplyCompare(currentVal, 0.0)) {
					result.value.floatValue = currentVal;
					result.prevValue.floatPrevValue = 0.0;
					matched = true;
				}
			} else {
				int64_t currentVal = ReadIntValue(addr);
				if (ApplyCompare(currentVal, 0)) {
					result.value.intValue = currentVal;
					result.prevValue.intPrevValue = 0;
					matched = true;
				}
			}

			if (matched) {
				results_.push_back(result);
				count++;
			}
		}
		Log("SearchMainRAM: full scan complete, %zu matches found", count);
	} else {
		size_t checked = 0;
		size_t inRange = 0;
		for (const auto& prev : previousResults_) {
			checked++;
			if (prev.address < start || prev.address >= end) continue;
			inRange++;
			if (!IsValidAddress(prev.address)) continue;

			bool matched = false;
			SearchResult result = prev;

			if (isFloatType) {
				double currentVal = ReadFloatValue(prev.address);
				if (ApplyCompare(currentVal, prev.value.floatValue)) {
					result.prevValue.floatPrevValue = prev.value.floatValue;
					result.value.floatValue = currentVal;
					matched = true;
				}
			} else {
				int64_t currentVal = ReadIntValue(prev.address);
				if (ApplyCompare(currentVal, prev.value.intValue)) {
					result.prevValue.intPrevValue = prev.value.intValue;
					result.value.intValue = currentVal;
					matched = true;
				}
			}

			if (matched) {
				results_.push_back(result);
				count++;
			}
		}
		Log("SearchMainRAM: filtered from %zu previous results, %zu in range, %zu matches", checked, inRange, count);
	}

	searchedBytes_ += (end - start);
	return count;
}

size_t MemorySearchEngine::SearchVRAM() {
	uint32_t start = 0x04000000;
	uint32_t end = 0x04600000;
	size_t count = 0;

	bool isFloatType = (dataType_ == SearchDataType::TYPE_FLOAT || dataType_ == SearchDataType::TYPE_DOUBLE);
	bool hasPrevious = !previousResults_.empty();
	size_t step = GetSearchStep();

	if (!hasPrevious) {
		Log("SearchVRAM: scanning full memory from 0x%08X to 0x%08X, step=%zu, aligned=%s", start, end, step, alignedSearch_ ? "true" : "false");
		bool fuzzySearch = IsFuzzySearch();
		for (uint32_t addr = start; addr < end; addr = (uint32_t)(addr + step)) {
			if (!IsValidAddress(addr)) continue;

			bool matched = false;
			SearchResult result;
			result.address = addr;
			result.type = dataType_;

			if (fuzzySearch) {
				// Fuzzy search on first scan: match all valid addresses
				if (isFloatType) {
					result.value.floatValue = ReadFloatValue(addr);
					result.prevValue.floatPrevValue = 0.0;
				} else {
					result.value.intValue = ReadIntValue(addr);
					result.prevValue.intPrevValue = 0;
				}
				matched = true;
			} else if (isFloatType) {
				double currentVal = ReadFloatValue(addr);
				if (ApplyCompare(currentVal, 0.0)) {
					result.value.floatValue = currentVal;
					result.prevValue.floatPrevValue = 0.0;
					matched = true;
				}
			} else {
				int64_t currentVal = ReadIntValue(addr);
				if (ApplyCompare(currentVal, 0)) {
					result.value.intValue = currentVal;
					result.prevValue.intPrevValue = 0;
					matched = true;
				}
			}

			if (matched) {
				results_.push_back(result);
				count++;
			}
		}
		Log("SearchVRAM: full scan complete, %zu matches found", count);
	} else {
		size_t checked = 0;
		size_t inRange = 0;
		for (const auto& prev : previousResults_) {
			checked++;
			if (prev.address < start || prev.address >= end) continue;
			inRange++;
			if (!IsValidAddress(prev.address)) continue;

			bool matched = false;
			SearchResult result = prev;

			if (isFloatType) {
				double currentVal = ReadFloatValue(prev.address);
				if (ApplyCompare(currentVal, prev.value.floatValue)) {
					result.prevValue.floatPrevValue = prev.value.floatValue;
					result.value.floatValue = currentVal;
					matched = true;
				}
			} else {
				int64_t currentVal = ReadIntValue(prev.address);
				if (ApplyCompare(currentVal, prev.value.intValue)) {
					result.prevValue.intPrevValue = prev.value.intValue;
					result.value.intValue = currentVal;
					matched = true;
				}
			}

			if (matched) {
				results_.push_back(result);
				count++;
			}
		}
		Log("SearchVRAM: filtered from %zu previous results, %zu in range, %zu matches", checked, inRange, count);
	}

	searchedBytes_ += (end - start);
	return count;
}

size_t MemorySearchEngine::SearchScratchPad() {
	uint32_t start = 0x00010000;
	uint32_t end = 0x00014000;
	size_t count = 0;

	bool isFloatType = (dataType_ == SearchDataType::TYPE_FLOAT || dataType_ == SearchDataType::TYPE_DOUBLE);
	bool hasPrevious = !previousResults_.empty();
	size_t step = GetSearchStep();

	if (!hasPrevious) {
		Log("SearchScratchPad: scanning full memory from 0x%08X to 0x%08X, step=%zu, aligned=%s", start, end, step, alignedSearch_ ? "true" : "false");
		bool fuzzySearch = IsFuzzySearch();
		for (uint32_t addr = start; addr < end; addr = (uint32_t)(addr + step)) {
			if (!IsValidAddress(addr)) continue;

			bool matched = false;
			SearchResult result;
			result.address = addr;
			result.type = dataType_;

			if (fuzzySearch) {
				// Fuzzy search on first scan: match all valid addresses
				if (isFloatType) {
					result.value.floatValue = ReadFloatValue(addr);
					result.prevValue.floatPrevValue = 0.0;
				} else {
					result.value.intValue = ReadIntValue(addr);
					result.prevValue.intPrevValue = 0;
				}
				matched = true;
			} else if (isFloatType) {
				double currentVal = ReadFloatValue(addr);
				if (ApplyCompare(currentVal, 0.0)) {
					result.value.floatValue = currentVal;
					result.prevValue.floatPrevValue = 0.0;
					matched = true;
				}
			} else {
				int64_t currentVal = ReadIntValue(addr);
				if (ApplyCompare(currentVal, 0)) {
					result.value.intValue = currentVal;
					result.prevValue.intPrevValue = 0;
					matched = true;
				}
			}

			if (matched) {
				results_.push_back(result);
				count++;
			}
		}
		Log("SearchScratchPad: full scan complete, %zu matches found", count);
	} else {
		size_t checked = 0;
		size_t inRange = 0;
		for (const auto& prev : previousResults_) {
			checked++;
			if (prev.address < start || prev.address >= end) continue;
			inRange++;
			if (!IsValidAddress(prev.address)) continue;

			bool matched = false;
			SearchResult result = prev;

			if (isFloatType) {
				double currentVal = ReadFloatValue(prev.address);
				if (ApplyCompare(currentVal, prev.value.floatValue)) {
					result.prevValue.floatPrevValue = prev.value.floatValue;
					result.value.floatValue = currentVal;
					matched = true;
				}
			} else {
				int64_t currentVal = ReadIntValue(prev.address);
				if (ApplyCompare(currentVal, prev.value.intValue)) {
					result.prevValue.intPrevValue = prev.value.intValue;
					result.value.intValue = currentVal;
					matched = true;
				}
			}

			if (matched) {
				results_.push_back(result);
				count++;
			}
		}
		Log("SearchScratchPad: filtered from %zu previous results, %zu in range, %zu matches", checked, inRange, count);
	}

	searchedBytes_ += (end - start);
	return count;
}

size_t MemorySearchEngine::FirstSearch() {
	ClearResults();
	previousResults_.clear();
	useBitmapMode_ = false;
	resultBitmap_.clear();
	previousBitmap_.clear();
	memorySnapshot_.clear();

	Log("=== FirstSearch ===");
	Log("DataType: %d, CompareType: %d, Region: %d", (int)dataType_, (int)compareType_, (int)currentRegion_);
	Log("SearchValue (int): %lld, SearchValue2: %lld", (long long)searchIntValue_, (long long)searchIntValue2_);
	Log("SearchValue (float): %f, SearchValue2: %f", searchFloatValue_, searchFloatValue2_);

	size_t totalCount = 0;
	bool fuzzySearch = IsFuzzySearch();
	size_t step = GetSearchStep();

	if (fuzzySearch) {
		uint32_t minAddr = 0xFFFFFFFF;
		uint32_t maxAddr = 0;

		switch (currentRegion_) {
		case MemoryRegion::MAIN_RAM:
			minAddr = 0x08000000;
			maxAddr = 0x08000000 + Memory::g_MemorySize;
			break;
		case MemoryRegion::VRAM:
			minAddr = 0x04000000;
			maxAddr = 0x04200000;
			break;
		case MemoryRegion::SCRATCHPAD:
			minAddr = 0x00010000;
			maxAddr = 0x00014000;
			break;
		case MemoryRegion::ALL:
			minAddr = 0x00010000;
			maxAddr = 0x08000000 + Memory::g_MemorySize;
			break;
		}

		size_t estimatedCount = (maxAddr - minAddr + step - 1) / step;
		Log("Fuzzy first search: estimated %zu results, threshold=%zu", estimatedCount, MAX_BITMAP_RESULT_COUNT);

		if (estimatedCount > MAX_BITMAP_RESULT_COUNT) {
			Log("Using bitmap mode for fuzzy first search");
			InitBitmap(minAddr, maxAddr, step);

			if (currentRegion_ == MemoryRegion::ALL) {
				uint32_t mainRamStart = 0x08000000;
				uint32_t mainRamEnd = 0x08000000 + Memory::g_MemorySize;
				uint32_t vramStart = 0x04000000;
				uint32_t vramEnd = 0x04200000;
				uint32_t scratchStart = 0x00010000;
				uint32_t scratchEnd = 0x00014000;

				for (uint32_t addr = minAddr; addr < maxAddr; addr = (uint32_t)(addr + step)) {
					bool inRange = false;
					if (addr >= mainRamStart && addr < mainRamEnd) inRange = true;
					else if (addr >= vramStart && addr < vramEnd) inRange = true;
					else if (addr >= scratchStart && addr < scratchEnd) inRange = true;

					if (!inRange) {
						BitmapClear(addr);
					}
				}
			}

			SaveBitmapSnapshot();
			totalCount = bitmapResultCount_;
			Log("Bitmap mode first search complete: %zu results", totalCount);
			return totalCount;
		}
	}

	switch (currentRegion_) {
	case MemoryRegion::MAIN_RAM:
		totalCount = SearchMainRAM();
		break;
	case MemoryRegion::VRAM:
		totalCount = SearchVRAM();
		break;
	case MemoryRegion::SCRATCHPAD:
		totalCount = SearchScratchPad();
		break;
	case MemoryRegion::ALL:
		totalCount += SearchMainRAM();
		totalCount += SearchVRAM();
		totalCount += SearchScratchPad();
		break;
	}

	SaveCurrentValues();

	Log("FirstSearch complete: %zu results", totalCount);

	if (totalCount > 0) {
		size_t kernelCount = 0;
		size_t userCount = 0;
		size_t vramCount = 0;
		size_t scratchCount = 0;
		uint32_t kernelEnd = 0x08800000;
		uint32_t userEnd = 0x08000000 + Memory::g_MemorySize;
		for (size_t i = 0; i < totalCount; i++) {
			uint32_t addr = results_[i].address;
			if (addr >= 0x08000000 && addr < kernelEnd) {
				kernelCount++;
			} else if (addr >= kernelEnd && addr < userEnd) {
				userCount++;
			} else if (addr >= 0x04000000 && addr < 0x04600000) {
				vramCount++;
			} else if (addr >= 0x00010000 && addr < 0x00014000) {
				scratchCount++;
			}
		}
		Log("Memory distribution: Kernel(0x08000000-0x087FFFFF)=%zu, User(0x08800000-0x%08X)=%zu, VRAM=%zu, ScratchPad=%zu",
			kernelCount, userEnd - 1, userCount, vramCount, scratchCount);
	}

	if (totalCount > 0 && totalCount <= 10) {
		Log("Results:");
		for (size_t i = 0; i < totalCount; i++) {
			const SearchResult &r = results_[i];
			Log("  [%zu] 0x%08X = %lld (int) / %f (float)", i, r.address, (long long)r.value.intValue, r.value.floatValue);
		}
	} else if (totalCount > 10) {
		Log("First 5 results:");
		for (size_t i = 0; i < 5; i++) {
			const SearchResult &r = results_[i];
			Log("  [%zu] 0x%08X = %lld (int) / %f (float)", i, r.address, (long long)r.value.intValue, r.value.floatValue);
		}
		size_t userStart = 0;
		for (size_t i = 0; i < totalCount; i++) {
			if (results_[i].address >= 0x08800000) {
				userStart = i;
				break;
			}
		}
		if (userStart > 0 && userStart < totalCount) {
			Log("First 5 user memory results (starting at index %zu):", userStart);
			for (size_t i = 0; i < 5 && userStart + i < totalCount; i++) {
				const SearchResult &r = results_[userStart + i];
				Log("  [%zu] 0x%08X = %lld (int) / %f (float)", userStart + i, r.address, (long long)r.value.intValue, r.value.floatValue);
			}
		}
	}

	return totalCount;
}

size_t MemorySearchEngine::NextSearch() {
	Log("=== NextSearch ===");
	Log("DataType: %d, CompareType: %d, Region: %d", (int)dataType_, (int)compareType_, (int)currentRegion_);
	Log("SearchValue (int): %lld, SearchValue2: %lld", (long long)searchIntValue_, (long long)searchIntValue2_);
	Log("SearchValue (float): %f, SearchValue2: %f", searchFloatValue_, searchFloatValue2_);
	Log("Previous results: %zu, bitmapMode: %s", previousResults_.size(), useBitmapMode_ ? "true" : "false");

	if (useBitmapMode_) {
		Log("Bitmap mode: filtering %zu previous results", bitmapResultCount_);

		resultBitmap_.assign(previousBitmap_.size(), 0);

		FilterBitmapWithCompare();

		size_t totalCount = bitmapResultCount_;
		Log("Bitmap mode next search: %zu results remaining", totalCount);

		if (totalCount <= MAX_BITMAP_RESULT_COUNT) {
			Log("Result count below threshold (%zu), expanding to full results", MAX_BITMAP_RESULT_COUNT);
			ExpandBitmapToResults();
			SaveCurrentValues();
			totalCount = results_.size();
			Log("Expanded to %zu full results", totalCount);
		} else {
			SaveBitmapSnapshot();
		}

		return totalCount;
	}

	bool isFloatType = (dataType_ == SearchDataType::TYPE_FLOAT || dataType_ == SearchDataType::TYPE_DOUBLE);
	size_t changedCount = 0;
	size_t unchangedCount = 0;
	if (!previousResults_.empty() && previousResults_.size() <= 100) {
		for (size_t i = 0; i < previousResults_.size(); i++) {
			const SearchResult &r = previousResults_[i];
			if (isFloatType) {
				double currentFloat = ReadFloatValue(r.address);
				if (currentFloat != r.value.floatValue) {
					changedCount++;
				} else {
					unchangedCount++;
				}
			} else {
				int64_t currentInt = ReadIntValue(r.address);
				if (currentInt != r.value.intValue) {
					changedCount++;
				} else {
					unchangedCount++;
				}
			}
		}
		Log("Value change check (from %zu results): %zu changed, %zu unchanged", previousResults_.size(), changedCount, unchangedCount);
	}

	if (!previousResults_.empty() && previousResults_.size() <= 10) {
		Log("Previous results (re-checking values):");
		for (size_t i = 0; i < previousResults_.size(); i++) {
			const SearchResult &r = previousResults_[i];
			int64_t currentInt = ReadIntValue(r.address);
			double currentFloat = ReadFloatValue(r.address);
			Log("  [%zu] 0x%08X: saved=%lld, current=%lld (int) | saved=%f, current=%f (float)",
				i, r.address, (long long)r.value.intValue, (long long)currentInt, r.value.floatValue, currentFloat);
		}
	} else if (!previousResults_.empty()) {
		Log("First 5 previous results (re-checking values):");
		for (size_t i = 0; i < 5; i++) {
			const SearchResult &r = previousResults_[i];
			int64_t currentInt = ReadIntValue(r.address);
			double currentFloat = ReadFloatValue(r.address);
			Log("  [%zu] 0x%08X: saved=%lld, current=%lld (int) | saved=%f, current=%f (float)",
				i, r.address, (long long)r.value.intValue, (long long)currentInt, r.value.floatValue, currentFloat);
		}
	}

	Log("--- Debug: Full scan for current search value (for comparison) ---");
	std::vector<SearchResult> savedResults = results_;
	std::vector<SearchResult> savedPrev = previousResults_;
	results_.clear();
	previousResults_.clear();
	size_t debugFullCount = 0;
	switch (currentRegion_) {
	case MemoryRegion::MAIN_RAM:
		debugFullCount = SearchMainRAM();
		break;
	case MemoryRegion::VRAM:
		debugFullCount = SearchVRAM();
		break;
	case MemoryRegion::SCRATCHPAD:
		debugFullCount = SearchScratchPad();
		break;
	case MemoryRegion::ALL:
		debugFullCount += SearchMainRAM();
		debugFullCount += SearchVRAM();
		debugFullCount += SearchScratchPad();
		break;
	}
	Log("Debug full scan result: %zu matches for current search value", debugFullCount);
	if (debugFullCount > 0) {
		size_t dKernelCount = 0;
		size_t dUserCount = 0;
		size_t dVramCount = 0;
		size_t dScratchCount = 0;
		uint32_t kernelEnd = 0x08800000;
		uint32_t userEnd = 0x08000000 + Memory::g_MemorySize;
		for (size_t i = 0; i < debugFullCount; i++) {
			uint32_t addr = results_[i].address;
			if (addr >= 0x08000000 && addr < kernelEnd) {
				dKernelCount++;
			} else if (addr >= kernelEnd && addr < userEnd) {
				dUserCount++;
			} else if (addr >= 0x04000000 && addr < 0x04600000) {
				dVramCount++;
			} else if (addr >= 0x00010000 && addr < 0x00014000) {
				dScratchCount++;
			}
		}
		Log("Debug full scan distribution: Kernel=%zu, User=%zu, VRAM=%zu, ScratchPad=%zu",
			dKernelCount, dUserCount, dVramCount, dScratchCount);
	}
	if (debugFullCount > 0 && debugFullCount <= 20) {
		Log("Debug full scan results:");
		for (size_t i = 0; i < debugFullCount; i++) {
			const SearchResult &r = results_[i];
			Log("  [%zu] 0x%08X = %lld (int) / %f (float)", i, r.address, (long long)r.value.intValue, r.value.floatValue);
		}
	}
	results_ = savedResults;
	previousResults_ = savedPrev;
	Log("--- End debug full scan ---");

	Log("--- Debug: What did previous results change to? ---");
	if (!savedPrev.empty() && savedPrev.size() <= 200) {
		std::map<int64_t, size_t> valueCounts;
		size_t stillSame = 0;
		for (size_t i = 0; i < savedPrev.size(); i++) {
			const SearchResult &r = savedPrev[i];
			if (isFloatType) {
				// Skip float for now
			} else {
				int64_t currentVal = ReadIntValue(r.address);
				if (currentVal == r.value.intValue) {
					stillSame++;
				} else {
					valueCounts[currentVal]++;
				}
			}
		}
		Log("Out of %zu previous results: %zu still same, %zu changed", savedPrev.size(), stillSame, savedPrev.size() - stillSame);
		if (!valueCounts.empty()) {
			Log("Changed values distribution (top 10):");
			size_t count = 0;
			for (auto it = valueCounts.begin(); it != valueCounts.end() && count < 10; ++it, ++count) {
				Log("  value=%lld: %zu addresses", (long long)it->first, it->second);
			}
		}
	}
	Log("--- End debug previous changes ---");

	results_.clear();

	size_t totalCount = 0;

	switch (currentRegion_) {
	case MemoryRegion::MAIN_RAM:
		totalCount = SearchMainRAM();
		break;
	case MemoryRegion::VRAM:
		totalCount = SearchVRAM();
		break;
	case MemoryRegion::SCRATCHPAD:
		totalCount = SearchScratchPad();
		break;
	case MemoryRegion::ALL:
		totalCount += SearchMainRAM();
		totalCount += SearchVRAM();
		totalCount += SearchScratchPad();
		break;
	}

	SaveCurrentValues();

	Log("NextSearch complete: %zu results", totalCount);

	return totalCount;
}

const SearchResult* MemorySearchEngine::GetResultAt(size_t index) const {
	if (index < results_.size()) {
		return &results_[index];
	}
	return nullptr;
}

bool MemorySearchEngine::ModifyValue(uint32_t address, int64_t newValue) {
	if (!IsValidAddress(address)) {
		Log("ModifyValue failed: invalid address 0x%08X", address);
		return false;
	}

	Log("ModifyValue: address=0x%08X, dataType=%d, newValue=%lld", address, (int)dataType_, (long long)newValue);

	switch (dataType_) {
	case SearchDataType::TYPE_8BIT:
		Memory::WriteUnchecked_U8(static_cast<uint8_t>(newValue), address);
		break;
	case SearchDataType::TYPE_16BIT:
		Memory::WriteUnchecked_U16(static_cast<uint16_t>(newValue), address);
		break;
	case SearchDataType::TYPE_32BIT:
		Memory::WriteUnchecked_U32(static_cast<uint32_t>(newValue), address);
		break;
	case SearchDataType::TYPE_64BIT: {
		uint8_t* ptr = Memory::GetPointerWriteUnchecked(address);
		uint64_t val = static_cast<uint64_t>(newValue);
		memcpy(ptr, &val, sizeof(uint64_t));
		break;
	}
	default:
		Log("ModifyValue failed: unsupported data type %d", (int)dataType_);
		return false;
	}

	int64_t verifyVal = ReadIntValue(address);
	Log("ModifyValue verify: address=0x%08X, value after write=%lld", address, (long long)verifyVal);

	return true;
}

bool MemorySearchEngine::ModifyValue(uint32_t address, double newValue) {
	if (!IsValidAddress(address)) {
		Log("ModifyValue(float) failed: invalid address 0x%08X", address);
		return false;
	}

	Log("ModifyValue(float): address=0x%08X, dataType=%d, newValue=%f", address, (int)dataType_, newValue);

	switch (dataType_) {
	case SearchDataType::TYPE_FLOAT:
		Memory::WriteUnchecked_Float(static_cast<float>(newValue), address);
		break;
	case SearchDataType::TYPE_DOUBLE: {
		uint8_t* ptr = Memory::GetPointerWriteUnchecked(address);
		uint64_t u64val;
		memcpy(&u64val, &newValue, sizeof(uint64_t));
		memcpy(ptr, &u64val, sizeof(uint64_t));
		break;
	}
	default:
		Log("ModifyValue(float) failed: unsupported data type %d", (int)dataType_);
		return false;
	}

	double verifyVal = ReadFloatValue(address);
	Log("ModifyValue(float) verify: address=0x%08X, value after write=%f", address, verifyVal);

	return true;
}

bool MemorySearchEngine::ModifyValue(uint32_t address, const std::string& newValue) {
	if (!IsValidAddress(address)) {
		return false;
	}

	uint8_t* ptr = Memory::GetPointerWrite(address);
	if (!ptr) {
		return false;
	}

	size_t len = std::min(newValue.length() + 1, static_cast<size_t>(256));
	memcpy(ptr, newValue.c_str(), len);
	ptr[len - 1] = 0;

	return true;
}

void MemorySearchEngine::FreezeAddress(uint32_t address, int64_t value) {
	UnfreezeAddress(address);

	SearchResult frozen;
	frozen.address = address;
	frozen.type = dataType_;
	frozen.value.intValue = value;
	frozenValues_.push_back(frozen);
}

void MemorySearchEngine::UnfreezeAddress(uint32_t address) {
	for (auto it = frozenValues_.begin(); it != frozenValues_.end(); ) {
		if (it->address == address) {
			it = frozenValues_.erase(it);
		} else {
			++it;
		}
	}
}

void MemorySearchEngine::UnfreezeAll() {
	frozenValues_.clear();
}

bool MemorySearchEngine::IsAddressFrozen(uint32_t address) const {
	for (const auto& frozen : frozenValues_) {
		if (frozen.address == address) {
			return true;
		}
	}
	return false;
}

int64_t MemorySearchEngine::GetFrozenValue(uint32_t address) const {
	for (const auto& frozen : frozenValues_) {
		if (frozen.address == address) {
			return frozen.value.intValue;
		}
	}
	return 0;
}

void MemorySearchEngine::ClearResults() {
	results_.clear();
	useBitmapMode_ = false;
	resultBitmap_.clear();
	previousBitmap_.clear();
	memorySnapshot_.clear();
	if (fileSnapshot_) {
		fclose(fileSnapshot_);
		fileSnapshot_ = nullptr;
	}
	bitmapResultCount_ = 0;
}

void MemorySearchEngine::SaveCurrentValues() {
	previousResults_.clear();
	for (const auto& result : results_) {
		previousResults_.push_back(result);
	}
}

void MemorySearchEngine::LoadPreviousValues() {
	results_.clear();
	for (const auto& prev : previousResults_) {
		results_.push_back(prev);
	}
}

void MemorySearchEngine::RefreshResults() {
	bool isFloatType = (dataType_ == SearchDataType::TYPE_FLOAT || dataType_ == SearchDataType::TYPE_DOUBLE);
	size_t changedCount = 0;
	size_t loggedCount = 0;

	for (auto& result : results_) {
		if (isFloatType) {
			double oldVal = result.value.floatValue;
			double newVal = ReadFloatValue(result.address);
			result.prevValue.floatPrevValue = oldVal;
			result.value.floatValue = newVal;
			if (oldVal != newVal) {
				changedCount++;
				if (loggedCount < 10) {
					Log("  Changed: 0x%08X: %f -> %f", result.address, oldVal, newVal);
					loggedCount++;
				}
			}
		} else {
			int64_t oldVal = result.value.intValue;
			int64_t newVal = ReadIntValue(result.address);
			result.prevValue.intPrevValue = oldVal;
			result.value.intValue = newVal;
			if (oldVal != newVal) {
				changedCount++;
				if (loggedCount < 10) {
					Log("  Changed: 0x%08X: %lld -> %lld", result.address, (long long)oldVal, (long long)newVal);
					loggedCount++;
				}
			}
		}
	}

	Log("RefreshResults: %zu results, %zu values changed", results_.size(), changedCount);
}

void MemorySearchEngine::InitBitmap(uint32_t baseAddr, uint32_t endAddr, size_t step) {
	bitmapBaseAddr_ = baseAddr;
	bitmapEndAddr_ = endAddr;
	bitmapStep_ = step;
	size_t totalSlots = (endAddr - baseAddr + step - 1) / step;
	size_t bitmapBytes = (totalSlots + 7) / 8;
	resultBitmap_.assign(bitmapBytes, 0xFF);
	previousBitmap_.clear();
	bitmapResultCount_ = totalSlots;
	useBitmapMode_ = true;

	size_t lastByteBits = totalSlots & 7;
	if (lastByteBits != 0) {
		uint8_t lastByteMask = (1 << lastByteBits) - 1;
		resultBitmap_.back() &= lastByteMask;
		size_t extraBits = 8 - lastByteBits;
		bitmapResultCount_ -= extraBits;
	}

	Log("InitBitmap: base=0x%08X, end=0x%08X, step=%zu, slots=%zu, bitmapSize=%zu bytes",
		baseAddr, endAddr, step, totalSlots, bitmapBytes);
}

void MemorySearchEngine::BitmapSet(uint32_t addr) {
	if (addr < bitmapBaseAddr_ || addr >= bitmapEndAddr_) return;
	size_t slot = (addr - bitmapBaseAddr_) / bitmapStep_;
	size_t byteIdx = slot >> 3;
	uint8_t bitMask = 1 << (slot & 7);
	if (!(resultBitmap_[byteIdx] & bitMask)) {
		resultBitmap_[byteIdx] |= bitMask;
		bitmapResultCount_++;
	}
}

void MemorySearchEngine::BitmapClear(uint32_t addr) {
	if (addr < bitmapBaseAddr_ || addr >= bitmapEndAddr_) return;
	size_t slot = (addr - bitmapBaseAddr_) / bitmapStep_;
	size_t byteIdx = slot >> 3;
	uint8_t bitMask = 1 << (slot & 7);
	if (resultBitmap_[byteIdx] & bitMask) {
		resultBitmap_[byteIdx] &= ~bitMask;
		bitmapResultCount_--;
	}
}

bool MemorySearchEngine::BitmapTest(uint32_t addr) const {
	if (addr < bitmapBaseAddr_ || addr >= bitmapEndAddr_) return false;
	size_t slot = (addr - bitmapBaseAddr_) / bitmapStep_;
	size_t byteIdx = slot >> 3;
	uint8_t bitMask = 1 << (slot & 7);
	return (resultBitmap_[byteIdx] & bitMask) != 0;
}

size_t MemorySearchEngine::BitmapCount() const {
	return bitmapResultCount_;
}

void MemorySearchEngine::SaveBitmapSnapshot() {
	uint32_t snapshotSize = bitmapEndAddr_ - bitmapBaseAddr_;

	if (fileSnapshot_) {
		fclose(fileSnapshot_);
		fileSnapshot_ = nullptr;
	}
	memorySnapshot_.clear();
	memorySnapshot_.shrink_to_fit();
	
	if (snapshotSize <= 64 * 1024 * 1024) {
		memorySnapshot_.resize(snapshotSize, 0);
		Log("SaveBitmapSnapshot: allocated %u bytes for memory snapshot", snapshotSize);
	} else {
		std::string tempPath = GetTempDirectory() + "/ppsspp_bitmap_snapshot.bin";
		FILE* f = fopen(tempPath.c_str(), "wb");
		if (f) {
			fseek(f, snapshotSize - 1, SEEK_SET);
			fputc(0, f);
			fflush(f);
			fclose(f);
			f = fopen(tempPath.c_str(), "rb+");
			if (f) {
				fileSnapshot_ = f;
				Log("SaveBitmapSnapshot: created file snapshot for %u bytes", snapshotSize);
			} else {
				Log("SaveBitmapSnapshot: failed to open file for writing, error=%d", errno);
			}
		} else {
			Log("SaveBitmapSnapshot: failed to create file, error=%d", errno);
		}
	}

	Log("SaveBitmapSnapshot: copying valid memory regions...");

	uint32_t mainRamStart = 0x08000000;
	uint32_t mainRamEnd = 0x08000000 + Memory::g_MemorySize;
	uint32_t vramStart = 0x04000000;
	uint32_t vramEnd = 0x04200000;
	uint32_t scratchStart = 0x00010000;
	uint32_t scratchEnd = 0x00014000;

	struct Region { uint32_t start; uint32_t end; const char* name; };
	Region regions[] = {
		{scratchStart, scratchEnd, "ScratchPad"},
		{vramStart, vramEnd, "VRAM"},
		{mainRamStart, mainRamEnd, "MainRAM"}
	};

	for (const auto& region : regions) {
		if (region.start >= bitmapBaseAddr_ && region.start < bitmapEndAddr_) {
			uint32_t offset = region.start - bitmapBaseAddr_;
			uint32_t size = std::min(region.end, bitmapEndAddr_) - region.start;
			if (size == 0) continue;
			
			const uint8_t* src = Memory::GetPointerUnchecked(region.start);
			
			uint8_t* dst = nullptr;
			FILE* f = nullptr;
			if (fileSnapshot_)
				f = fileSnapshot_;
			else if (!memorySnapshot_.empty())
				dst = memorySnapshot_.data() + offset;

			if (dst && src) {
				memcpy(dst, src, size);
				Log("SaveBitmapSnapshot: copied %s (%u bytes at offset %u)", region.name, size, offset);
			} else if (f && src) {
				fseek(f, offset, SEEK_SET);
				fwrite(src, 1, size, f);
				fflush(f);
				Log("SaveBitmapSnapshot: wrote %s to file (%u bytes at offset %u)", region.name, size, offset);
			} else if (dst) {
				memset(dst, 0, size);
				Log("SaveBitmapSnapshot: zeroed %s (%u bytes at offset %u)", region.name, size, offset);
			}
		}
	}

	previousBitmap_ = std::move(resultBitmap_);
	resultBitmap_.clear();
	resultBitmap_.shrink_to_fit();
	Log("SaveBitmapSnapshot: moved bitmap to previous (size %zu bytes), bitmap has %zu results",
		previousBitmap_.size(), bitmapResultCount_);
}

void MemorySearchEngine::ExpandBitmapToResults() {
	results_.clear();
	bool isFloatType = (dataType_ == SearchDataType::TYPE_FLOAT || dataType_ == SearchDataType::TYPE_DOUBLE);
	size_t step = bitmapStep_;

	for (uint32_t addr = bitmapBaseAddr_; addr < bitmapEndAddr_; addr = (uint32_t)(addr + step)) {
		if (BitmapTest(addr)) {
			SearchResult result;
			result.address = addr;
			result.type = dataType_;
			if (isFloatType) {
				result.value.floatValue = ReadFloatValue(addr);
				result.prevValue.floatPrevValue = 0.0;
			} else {
				result.value.intValue = ReadIntValue(addr);
				result.prevValue.intPrevValue = 0;
			}
			results_.push_back(result);
		}
	}
	useBitmapMode_ = false;
	resultBitmap_.clear();
	previousBitmap_.clear();
	memorySnapshot_.clear();
	Log("ExpandBitmapToResults: expanded %zu results from bitmap", results_.size());
}

size_t MemorySearchEngine::FilterBitmapWithCompare() {
	bool hasSnapshot = !memorySnapshot_.empty() || (fileSnapshot_ != nullptr);
	bool hasPreviousBitmap = !previousBitmap_.empty();

	if (!hasSnapshot && !hasPreviousBitmap) {
		Log("FilterBitmapWithCompare: no previous data, keeping all bitmap results");
		return bitmapResultCount_;
	}

	bool isFloatType = (dataType_ == SearchDataType::TYPE_FLOAT || dataType_ == SearchDataType::TYPE_DOUBLE);
	size_t step = bitmapStep_;
	size_t totalChecked = 0;
	size_t matched = 0;
	uint32_t snapshotSize = bitmapEndAddr_ - bitmapBaseAddr_;

	const uint8_t* snapData = nullptr;
	FILE* f = nullptr;
	std::vector<uint8_t> fileBuffer;
	uint32_t bufferStart = 0;
	uint32_t bufferSize = 0;
	const uint32_t kBufferSize = 1 * 1024 * 1024;

	if (fileSnapshot_) {
		f = fileSnapshot_;
		fileBuffer.resize(kBufferSize);
		bufferStart = 0xFFFFFFFF;
		bufferSize = 0;
	} else {
		snapData = memorySnapshot_.data();
	}

	auto EnsureBuffer = [&](uint32_t offset, uint32_t needSize) -> const uint8_t* {
		if (!f) return snapData + offset;
		if (offset >= bufferStart && offset + needSize <= bufferStart + bufferSize) {
			return fileBuffer.data() + (offset - bufferStart);
		}
		bufferStart = offset & ~(kBufferSize - 1);
		uint32_t readSize = (uint32_t)kBufferSize;
		if (bufferStart + readSize > snapshotSize) {
			readSize = snapshotSize - bufferStart;
		}
		fseek(f, bufferStart, SEEK_SET);
		fread(fileBuffer.data(), 1, readSize, f);
		bufferSize = readSize;
		return fileBuffer.data() + (offset - bufferStart);
	};

	for (uint32_t addr = bitmapBaseAddr_; addr < bitmapEndAddr_; addr = (uint32_t)(addr + step)) {
		size_t slot = (addr - bitmapBaseAddr_) / step;
		size_t byteIdx = slot >> 3;
		uint8_t bitMask = 1 << (slot & 7);

		if (hasPreviousBitmap) {
			if (!(previousBitmap_[byteIdx] & bitMask)) {
				if (resultBitmap_[byteIdx] & bitMask) {
					resultBitmap_[byteIdx] &= ~bitMask;
				}
				continue;
			}
		}

		totalChecked++;
		bool matches = false;
		uint32_t offset = addr - bitmapBaseAddr_;

		if (isFloatType) {
			double prevVal = 0.0;
			if (hasSnapshot && offset + sizeof(double) <= snapshotSize) {
				if (dataType_ == SearchDataType::TYPE_FLOAT) {
					const uint8_t* p = EnsureBuffer(offset, sizeof(float));
					float fval;
					memcpy(&fval, p, sizeof(float));
					prevVal = fval;
				} else if (dataType_ == SearchDataType::TYPE_DOUBLE) {
					const uint8_t* p = EnsureBuffer(offset, sizeof(double));
					memcpy(&prevVal, p, sizeof(double));
				}
			}
			double currentVal = ReadFloatValue(addr);
			matches = ApplyCompare(currentVal, prevVal);
		} else {
			int64_t prevVal = 0;
			if (hasSnapshot && offset < snapshotSize) {
				switch (dataType_) {
				case SearchDataType::TYPE_8BIT:
					if (offset + 1 <= snapshotSize) {
						const uint8_t* p = EnsureBuffer(offset, 1);
						prevVal = p[0];
					}
					break;
				case SearchDataType::TYPE_16BIT:
					if (offset + 2 <= snapshotSize) {
						const uint8_t* p = EnsureBuffer(offset, 2);
						uint16_t val;
						memcpy(&val, p, sizeof(uint16_t));
						prevVal = val;
					}
					break;
				case SearchDataType::TYPE_32BIT:
					if (offset + 4 <= snapshotSize) {
						const uint8_t* p = EnsureBuffer(offset, 4);
						uint32_t val;
						memcpy(&val, p, sizeof(uint32_t));
						prevVal = val;
					}
					break;
				case SearchDataType::TYPE_64BIT:
					if (offset + 8 <= snapshotSize) {
						const uint8_t* p = EnsureBuffer(offset, 8);
						uint64_t val;
						memcpy(&val, p, sizeof(uint64_t));
						prevVal = val;
					}
					break;
				default:
					break;
				}
			}
			int64_t currentVal = ReadIntValue(addr);
			matches = ApplyCompare(currentVal, prevVal);
		}

		if (matches) {
			if (!(resultBitmap_[byteIdx] & bitMask)) {
				resultBitmap_[byteIdx] |= bitMask;
			}
			matched++;
		} else {
			if (resultBitmap_[byteIdx] & bitMask) {
				resultBitmap_[byteIdx] &= ~bitMask;
			}
		}
	}

	bitmapResultCount_ = matched;
	Log("FilterBitmapWithCompare: checked %zu addresses, %zu matched (hasSnapshot=%d, hasFile=%d, hasPrevBitmap=%d)",
		totalChecked, matched, hasSnapshot,
		fileSnapshot_ != nullptr ? 1 : 0, hasPreviousBitmap);
	return matched;
}
