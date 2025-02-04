//
// Created by kat on 5/19/23.
//

#include "binaryninjaapi.h"

/* ---
 * This is the primary image loader logic for Shared Caches
 *
 * It is standalone code that operates on a DSCView.
 *
 * This has to recreate _all_ of the Mach-O View logic, but slightly differently, as everything is spicy and weird and
 * 		different enough that it's not worth trying to make a shared base class.
 *
 * The SharedCache api object is a 'Controller' that serializes its own state in view metadata.
 *
 * It is multithreading capable (multiple SharedCache objects can exist and do things on different threads, it will manage)
 *
 * View state is saved to BinaryView any time it changes, however due to json deser speed we must also cache it on heap.
 *	This cache is 'load bearing' and controllers on other threads may serialize it back to view after making changes, so it
 *	must be kept up to date.
 *
 *
 *
 * */

#include "SharedCache.h"
#include "ObjC.h"
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <fcntl.h>
#include <memory>
#include <chrono>
#include <thread>


using namespace BinaryNinja;
using namespace SharedCacheCore;

#ifdef _MSC_VER

int count_trailing_zeros(uint64_t value) {
	unsigned long index; // 32-bit long on Windows
	if (_BitScanForward64(&index, value)) {
		return index;
	} else {
		return 64; // If the value is 0, return 64.
	}
}
#else
int count_trailing_zeros(uint64_t value) {
	return value == 0 ? 64 : __builtin_ctzll(value);
}
#endif

struct SharedCache::State
{
	std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, std::pair<BNSymbolType, std::string>>>>
		exportInfos;
	std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, std::pair<BNSymbolType, std::string>>>>
		symbolInfos;

	std::unordered_map<std::string, uint64_t> imageStarts;
	std::unordered_map<uint64_t, SharedCacheMachOHeader> headers;

	std::vector<CacheImage> images;

	std::vector<MemoryRegion> regionsMappedIntoMemory;

	std::vector<BackingCache> backingCaches;

	std::vector<MemoryRegion> stubIslandRegions;  // TODO honestly both of these should be refactored into nonImageRegions. :p
	std::vector<MemoryRegion> dyldDataRegions;
	std::vector<MemoryRegion> nonImageRegions;

	std::optional<std::pair<size_t, size_t>> objcOptimizationDataRange;

	std::string baseFilePath;
	SharedCacheFormat cacheFormat;
	DSCViewState viewState = DSCViewStateUnloaded;
};

struct SharedCache::ViewSpecificState {
	std::mutex typeLibraryMutex;
	std::unordered_map<std::string, Ref<TypeLibrary>> typeLibraries;

	std::mutex viewOperationsThatInfluenceMetadataMutex;

	std::atomic<BNDSCViewLoadProgress> progress;

	std::mutex stateMutex;
	std::shared_ptr<struct SharedCache::State> cachedState;
};


std::shared_ptr<SharedCache::ViewSpecificState> ViewSpecificStateForId(uint64_t viewIdentifier, bool insertIfNeeded = true) {
	static std::mutex viewSpecificStateMutex;
	static std::unordered_map<uint64_t, std::weak_ptr<SharedCache::ViewSpecificState>> viewSpecificState;

	std::lock_guard lock(viewSpecificStateMutex);

	if (auto it = viewSpecificState.find(viewIdentifier); it != viewSpecificState.end()) {
		if (auto statePtr = it->second.lock()) {
			return statePtr;
		}
	}

	if (!insertIfNeeded) {
		return nullptr;
	}

	auto statePtr = std::make_shared<SharedCache::ViewSpecificState>();
	viewSpecificState[viewIdentifier] = statePtr;

	// Prune entries for any views that are no longer in use.
	for (auto it = viewSpecificState.begin(); it != viewSpecificState.end(); ) {
		if (it->second.expired()) {
			it = viewSpecificState.erase(it);
		} else {
			++it;
		}
	}

	return statePtr;
}

std::shared_ptr<SharedCache::ViewSpecificState> ViewSpecificStateForView(Ref<BinaryNinja::BinaryView> view) {
	return ViewSpecificStateForId(view->GetFile()->GetSessionId());
}

std::string base_name(std::string const& path)
{
	return path.substr(path.find_last_of("/\\") + 1);
}

BNSegmentFlag SegmentFlagsFromMachOProtections(int initProt, int maxProt) {

	uint32_t flags = 0;
	if (initProt & MACHO_VM_PROT_READ)
		flags |= SegmentReadable;
	if (initProt & MACHO_VM_PROT_WRITE)
		flags |= SegmentWritable;
	if (initProt & MACHO_VM_PROT_EXECUTE)
		flags |= SegmentExecutable;
	if (((initProt & MACHO_VM_PROT_WRITE) == 0) &&
		((maxProt & MACHO_VM_PROT_WRITE) == 0))
		flags |= SegmentDenyWrite;
	if (((initProt & MACHO_VM_PROT_EXECUTE) == 0) &&
		((maxProt & MACHO_VM_PROT_EXECUTE) == 0))
		flags |= SegmentDenyExecute;
	return (BNSegmentFlag)flags;
}


#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
static int64_t readSLEB128(const uint8_t*& current, const uint8_t* end)
{
	uint8_t cur;
	int64_t value = 0;
	size_t shift = 0;
	while (current != end)
	{
		cur = *current++;
		value |= (cur & 0x7f) << shift;
		shift += 7;
		if ((cur & 0x80) == 0)
			break;
	}
	value = (value << (64 - shift)) >> (64 - shift);
	return value;
}
#pragma clang diagnostic pop


static uint64_t readLEB128(const uint8_t*& current, const uint8_t* end)
{
	uint64_t result = 0;
	int bit = 0;
	do
	{
		if (current >= end)
			return -1;

		uint64_t slice = *current & 0x7f;

		if (bit > 63)
			return -1;
		else
		{
			result |= (slice << bit);
			bit += 7;
		}
	} while (*current++ & 0x80);
	return result;
}


uint64_t readValidULEB128(const uint8_t*& current, const uint8_t* end)
{
	uint64_t value = readLEB128(current, end);
	if ((int64_t)value == -1)
		throw ReadException();
	return value;
}


uint64_t SharedCache::FastGetBackingCacheCount(BinaryNinja::Ref<BinaryNinja::BinaryView> dscView)
{
	std::shared_ptr<MMappedFileAccessor> baseFile;
	try {
		baseFile = MMappedFileAccessor::Open(dscView, dscView->GetFile()->GetSessionId(), dscView->GetFile()->GetOriginalFilename())->lock();
	}
	catch (...){
		LogError("Shared Cache preload: Failed to open file %s", dscView->GetFile()->GetOriginalFilename().c_str());
		return 0;
	}

	dyld_cache_header header {};
	size_t header_size = baseFile->ReadUInt32(16);
	baseFile->Read(&header, 0, std::min(header_size, sizeof(dyld_cache_header)));

	SharedCacheFormat cacheFormat;

	if (header.imagesCountOld != 0)
		cacheFormat = RegularCacheFormat;

	size_t subCacheOff = offsetof(struct dyld_cache_header, subCacheArrayOffset);
	size_t headerEnd = header.mappingOffset;
	if (headerEnd > subCacheOff)
	{
		if (header.cacheType != 2)
		{
			if (std::filesystem::exists(ResolveFilePath(dscView, baseFile->Path() + ".01")))
				cacheFormat = LargeCacheFormat;
			else
				cacheFormat = SplitCacheFormat;
		}
		else
			cacheFormat = iOS16CacheFormat;
	}

	switch (cacheFormat)
	{
	case RegularCacheFormat:
	{
		return 1;
	}
	case LargeCacheFormat:
	{
		auto mainFileName = baseFile->Path();
		auto subCacheCount = header.subCacheArrayCount;
		return subCacheCount + 1;
	}
	case SplitCacheFormat:
	{
		auto mainFileName = baseFile->Path();
		auto subCacheCount = header.subCacheArrayCount;
		return subCacheCount + 2;
	}
	case iOS16CacheFormat:
	{
		auto mainFileName = baseFile->Path();
		auto subCacheCount = header.subCacheArrayCount;
		return subCacheCount + 2;
	}
	}
}


void SharedCache::PerformInitialLoad()
{
	m_logger->LogInfo("Performing initial load of Shared Cache");
	auto path = m_dscView->GetFile()->GetOriginalFilename();
	auto baseFile = MMappedFileAccessor::Open(m_dscView, m_dscView->GetFile()->GetSessionId(), path)->lock();

	m_viewSpecificState->progress = LoadProgressLoadingCaches;

	WillMutateState();

	MutableState().baseFilePath = path;

	DataBuffer sig = baseFile->ReadBuffer(0, 4);
	if (sig.GetLength() != 4)
		abort();
	const char* magic = (char*)sig.GetData();
	if (strncmp(magic, "dyld", 4) != 0)
		abort();

	MutableState().cacheFormat = RegularCacheFormat;

	dyld_cache_header primaryCacheHeader {};
	size_t header_size = baseFile->ReadUInt32(16);
	baseFile->Read(&primaryCacheHeader, 0, std::min(header_size, sizeof(dyld_cache_header)));

	if (primaryCacheHeader.imagesCountOld != 0)
		MutableState().cacheFormat = RegularCacheFormat;

	size_t subCacheOff = offsetof(struct dyld_cache_header, subCacheArrayOffset);
	size_t headerEnd = primaryCacheHeader.mappingOffset;
	if (headerEnd > subCacheOff)
	{
		if (primaryCacheHeader.cacheType != 2)
		{
			if (std::filesystem::exists(ResolveFilePath(m_dscView, baseFile->Path() + ".01")))
				MutableState().cacheFormat = LargeCacheFormat;
			else
				MutableState().cacheFormat = SplitCacheFormat;
		}
		else
			MutableState().cacheFormat = iOS16CacheFormat;
	}

	if (primaryCacheHeader.objcOptsOffset && primaryCacheHeader.objcOptsSize) {
		uint64_t objcOptsOffset = primaryCacheHeader.objcOptsOffset;
		uint64_t objcOptsSize = primaryCacheHeader.objcOptsSize;
		MutableState().objcOptimizationDataRange = {objcOptsOffset, objcOptsSize};
	}

	switch (State().cacheFormat)
	{
	case RegularCacheFormat:
	{
		dyld_cache_mapping_info mapping {};
		BackingCache cache;
		cache.isPrimary = true;
		cache.path = path;

		for (size_t i = 0; i < primaryCacheHeader.mappingCount; i++)
		{
			baseFile->Read(&mapping, primaryCacheHeader.mappingOffset + (i * sizeof(mapping)), sizeof(mapping));
			cache.mappings.push_back(mapping);
		}
		MutableState().backingCaches.push_back(std::move(cache));

		dyld_cache_image_info img {};

		for (size_t i = 0; i < primaryCacheHeader.imagesCountOld; i++)
		{
			baseFile->Read(&img, primaryCacheHeader.imagesOffsetOld + (i * sizeof(img)), sizeof(img));
			auto iname = baseFile->ReadNullTermString(img.pathFileOffset);
			MutableState().imageStarts[iname] = img.address;
		}

		m_logger->LogInfo("Found %d images in the shared cache", primaryCacheHeader.imagesCountOld);

		if (primaryCacheHeader.branchPoolsCount)
		{
			std::vector<uint64_t> addresses;
			for (size_t i = 0; i < primaryCacheHeader.branchPoolsCount; i++)
			{
				addresses.push_back(baseFile->ReadULong(primaryCacheHeader.branchPoolsOffset + (i * m_dscView->GetAddressSize())));
			}
			baseFile.reset(); // No longer needed, we're about to remap this file into VM space so we can load these.
			uint64_t i = 0;
			for (auto address : addresses)
			{
				i++;
				auto vm = GetVMMap(true);
				auto machoHeader = SharedCache::LoadHeaderForAddress(vm, address, "dyld_shared_cache_branch_islands_" + std::to_string(i));
				if (machoHeader)
				{
					for (const auto& segment : machoHeader->segments)
					{
						MemoryRegion stubIslandRegion;
						stubIslandRegion.start = segment.vmaddr;
						stubIslandRegion.size = segment.filesize;
						char segName[17];
						memcpy(segName, segment.segname, 16);
						segName[16] = 0;
						std::string segNameStr = std::string(segName);
						stubIslandRegion.prettyName = "dyld_shared_cache_branch_islands_" + std::to_string(i) + "::" + segNameStr;
						stubIslandRegion.flags = (BNSegmentFlag)(BNSegmentFlag::SegmentReadable | BNSegmentFlag::SegmentExecutable);
						MutableState().stubIslandRegions.push_back(std::move(stubIslandRegion));
					}
				}
			}
		}

		m_logger->LogInfo("Found %d branch pools in the shared cache", primaryCacheHeader.branchPoolsCount);

		break;
	}
	case LargeCacheFormat:
	{
		dyld_cache_mapping_info mapping {};	 // We're going to reuse this for all of the mappings. We only need it
											 // briefly.

		BackingCache cache;
		cache.isPrimary = true;
		cache.path = path;

		for (size_t i = 0; i < primaryCacheHeader.mappingCount; i++)
		{
			baseFile->Read(&mapping, primaryCacheHeader.mappingOffset + (i * sizeof(mapping)), sizeof(mapping));
			cache.mappings.push_back(mapping);
		}
		MutableState().backingCaches.push_back(std::move(cache));

		dyld_cache_image_info img {};

		for (size_t i = 0; i < primaryCacheHeader.imagesCount; i++)
		{
			baseFile->Read(&img, primaryCacheHeader.imagesOffset + (i * sizeof(img)), sizeof(img));
			auto iname = baseFile->ReadNullTermString(img.pathFileOffset);
			MutableState().imageStarts[iname] = img.address;
		}

		if (primaryCacheHeader.branchPoolsCount)
		{
			std::vector<uint64_t> pool {};
			for (size_t i = 0; i < primaryCacheHeader.branchPoolsCount; i++)
			{
				MutableState().imageStarts["dyld_shared_cache_branch_islands_" + std::to_string(i)] =
					baseFile->ReadULong(primaryCacheHeader.branchPoolsOffset + (i * m_dscView->GetAddressSize()));
			}
		}
		std::string mainFileName = base_name(path);
		if (auto projectFile = m_dscView->GetFile()->GetProjectFile())
			mainFileName = projectFile->GetName();
		auto subCacheCount = primaryCacheHeader.subCacheArrayCount;

		dyld_subcache_entry2 _entry {};
		std::vector<dyld_subcache_entry2> subCacheEntries;
		for (size_t i = 0; i < subCacheCount; i++)
		{
			baseFile->Read(&_entry, primaryCacheHeader.subCacheArrayOffset + (i * sizeof(dyld_subcache_entry2)),
				sizeof(dyld_subcache_entry2));
			subCacheEntries.push_back(_entry);
		}

		baseFile.reset();
		for (const auto& entry : subCacheEntries)
		{
			std::string subCachePath;
			std::string subCacheFilename;
			if (std::string(entry.fileExtension).find('.') != std::string::npos)
			{
				subCachePath = path + entry.fileExtension;
				subCacheFilename = mainFileName + entry.fileExtension;
			}
			else
			{
				subCachePath = path + "." + entry.fileExtension;
				subCacheFilename = mainFileName + "." + entry.fileExtension;
			}
			auto subCacheFile = MMappedFileAccessor::Open(m_dscView, m_dscView->GetFile()->GetSessionId(), subCachePath)->lock();

			dyld_cache_header subCacheHeader {};
			uint64_t headerSize = subCacheFile->ReadUInt32(16);
			if (headerSize > sizeof(dyld_cache_header))
			{
				m_logger->LogDebug("Header size is larger than expected (0x%llx), using default size (0x%llx)", headerSize,
					sizeof(dyld_cache_header));
				headerSize = sizeof(dyld_cache_header);
			}
			subCacheFile->Read(&subCacheHeader, 0, headerSize);

			dyld_cache_mapping_info subCacheMapping {};
			BackingCache subCache;
			subCache.isPrimary = false;
			subCache.path = subCachePath;

			for (size_t j = 0; j < subCacheHeader.mappingCount; j++)
			{
				subCacheFile->Read(&subCacheMapping, subCacheHeader.mappingOffset + (j * sizeof(subCacheMapping)),
					sizeof(subCacheMapping));
				subCache.mappings.push_back(subCacheMapping);
			}

			if (subCacheHeader.mappingCount == 1 && subCacheHeader.imagesCountOld == 0 && subCacheHeader.imagesCount == 0
				&& subCacheHeader.imagesTextOffset == 0)
			{
				auto pathBasename = subCachePath.substr(subCachePath.find_last_of("/\\") + 1);
				uint64_t address = subCacheMapping.address;
				uint64_t size = subCacheMapping.size;
				MemoryRegion stubIslandRegion;
				stubIslandRegion.start = address;
				stubIslandRegion.size = size;
				stubIslandRegion.prettyName = subCacheFilename + "::_stubs";
				stubIslandRegion.flags = (BNSegmentFlag)(BNSegmentFlag::SegmentReadable | BNSegmentFlag::SegmentExecutable);
				MutableState().stubIslandRegions.push_back(std::move(stubIslandRegion));
			}

			MutableState().backingCaches.push_back(std::move(subCache));
		}
		break;
	}
	case SplitCacheFormat:
	{
		dyld_cache_mapping_info mapping {};	 // We're going to reuse this for all of the mappings. We only need it
											 // briefly.
		BackingCache cache;
		cache.isPrimary = true;
		cache.path = path;

		for (size_t i = 0; i < primaryCacheHeader.mappingCount; i++)
		{
			baseFile->Read(&mapping, primaryCacheHeader.mappingOffset + (i * sizeof(mapping)), sizeof(mapping));
			cache.mappings.push_back(mapping);
		}
		MutableState().backingCaches.push_back(std::move(cache));

		dyld_cache_image_info img {};

		for (size_t i = 0; i < primaryCacheHeader.imagesCount; i++)
		{
			baseFile->Read(&img, primaryCacheHeader.imagesOffset + (i * sizeof(img)), sizeof(img));
			auto iname = baseFile->ReadNullTermString(img.pathFileOffset);
			MutableState().imageStarts[iname] = img.address;
		}

		if (primaryCacheHeader.branchPoolsCount)
		{
			std::vector<uint64_t> pool {};
			for (size_t i = 0; i < primaryCacheHeader.branchPoolsCount; i++)
			{
				MutableState().imageStarts["dyld_shared_cache_branch_islands_" + std::to_string(i)] =
					baseFile->ReadULong(primaryCacheHeader.branchPoolsOffset + (i * m_dscView->GetAddressSize()));
			}
		}

		std::string mainFileName = base_name(path);
		if (auto projectFile = m_dscView->GetFile()->GetProjectFile())
			mainFileName = projectFile->GetName();
		auto subCacheCount = primaryCacheHeader.subCacheArrayCount;

		baseFile.reset();

		for (size_t i = 1; i <= subCacheCount; i++)
		{
			auto subCachePath = path + "." + std::to_string(i);
			auto subCacheFilename = mainFileName + "." + std::to_string(i);
			auto subCacheFile = MMappedFileAccessor::Open(m_dscView, m_dscView->GetFile()->GetSessionId(), subCachePath)->lock();

			dyld_cache_header subCacheHeader {};
			uint64_t headerSize = subCacheFile->ReadUInt32(16);
			if (headerSize > sizeof(dyld_cache_header))
			{
				m_logger->LogDebug("Header size is larger than expected (0x%llx), using default size (0x%llx)", headerSize,
					sizeof(dyld_cache_header));
				headerSize = sizeof(dyld_cache_header);
			}
			subCacheFile->Read(&subCacheHeader, 0, headerSize);

			BackingCache subCache;
			subCache.isPrimary = false;
			subCache.path = subCachePath;

			dyld_cache_mapping_info subCacheMapping {};

			for (size_t j = 0; j < subCacheHeader.mappingCount; j++)
			{
				subCacheFile->Read(&subCacheMapping, subCacheHeader.mappingOffset + (j * sizeof(subCacheMapping)),
					sizeof(subCacheMapping));
				subCache.mappings.push_back(subCacheMapping);
			}

			MutableState().backingCaches.push_back(std::move(subCache));

			if (subCacheHeader.mappingCount == 1 && subCacheHeader.imagesCountOld == 0 && subCacheHeader.imagesCount == 0
				&& subCacheHeader.imagesTextOffset == 0)
			{
				auto pathBasename = subCachePath.substr(subCachePath.find_last_of("/\\") + 1);
				uint64_t address = subCacheMapping.address;
				uint64_t size = subCacheMapping.size;
				MemoryRegion stubIslandRegion;
				stubIslandRegion.start = address;
				stubIslandRegion.size = size;
				stubIslandRegion.prettyName = subCacheFilename + "::_stubs";
				stubIslandRegion.flags = (BNSegmentFlag)(BNSegmentFlag::SegmentReadable | BNSegmentFlag::SegmentExecutable);
				MutableState().stubIslandRegions.push_back(std::move(stubIslandRegion));
			}
		}

		// Load .symbols subcache
		try {
			auto subCachePath = path + ".symbols";
			auto subCacheFile = MMappedFileAccessor::Open(m_dscView, m_dscView->GetFile()->GetSessionId(), subCachePath)->lock();

			dyld_cache_header subCacheHeader {};
			uint64_t headerSize = subCacheFile->ReadUInt32(16);
			if (headerSize > sizeof(dyld_cache_header))
			{
				m_logger->LogDebug("Header size is larger than expected (0x%llx), using default size (0x%llx)", headerSize,
					sizeof(dyld_cache_header));
				headerSize = sizeof(dyld_cache_header);
			}
			subCacheFile->Read(&subCacheHeader, 0, headerSize);

			dyld_cache_mapping_info subCacheMapping {};
			BackingCache subCache;

			for (size_t j = 0; j < subCacheHeader.mappingCount; j++)
			{
				subCacheFile->Read(&subCacheMapping, subCacheHeader.mappingOffset + (j * sizeof(subCacheMapping)),
					sizeof(subCacheMapping));
				subCache.mappings.push_back(subCacheMapping);
			}

			MutableState().backingCaches.push_back(std::move(subCache));
		}
		catch (...)
		{
			m_logger->LogWarn("Failed to locate .symbols subcache. Non-exported symbol information may be missing.");
		}
		break;
	}
	case iOS16CacheFormat:
	{
		dyld_cache_mapping_info mapping {};

		BackingCache cache;
		cache.isPrimary = true;
		cache.path = path;

		for (size_t i = 0; i < primaryCacheHeader.mappingCount; i++)
		{
			baseFile->Read(&mapping, primaryCacheHeader.mappingOffset + (i * sizeof(mapping)), sizeof(mapping));
			cache.mappings.push_back(mapping);
		}

		MutableState().backingCaches.push_back(std::move(cache));

		dyld_cache_image_info img {};

		for (size_t i = 0; i < primaryCacheHeader.imagesCount; i++)
		{
			baseFile->Read(&img, primaryCacheHeader.imagesOffset + (i * sizeof(img)), sizeof(img));
			auto iname = baseFile->ReadNullTermString(img.pathFileOffset);
			MutableState().imageStarts[iname] = img.address;
		}

		if (primaryCacheHeader.branchPoolsCount)
		{
			std::vector<uint64_t> pool {};
			for (size_t i = 0; i < primaryCacheHeader.branchPoolsCount; i++)
			{
				MutableState().imageStarts["dyld_shared_cache_branch_islands_" + std::to_string(i)] =
					baseFile->ReadULong(primaryCacheHeader.branchPoolsOffset + (i * m_dscView->GetAddressSize()));
			}
		}

		std::string mainFileName = base_name(path);
		if (auto projectFile = m_dscView->GetFile()->GetProjectFile())
			mainFileName = projectFile->GetName();
		auto subCacheCount = primaryCacheHeader.subCacheArrayCount;

		dyld_subcache_entry2 _entry {};

		std::vector<dyld_subcache_entry2> subCacheEntries;
		for (size_t i = 0; i < subCacheCount; i++)
		{
			baseFile->Read(&_entry, primaryCacheHeader.subCacheArrayOffset + (i * sizeof(dyld_subcache_entry2)),
				sizeof(dyld_subcache_entry2));
			subCacheEntries.push_back(_entry);
		}

		baseFile.reset();

		for (const auto& entry : subCacheEntries)
		{
			std::string subCachePath;
			std::string subCacheFilename;
			if (std::string(entry.fileExtension).find('.') != std::string::npos)
			{
				subCachePath = path + entry.fileExtension;
				subCacheFilename = mainFileName + entry.fileExtension;
			}
			else
			{
				subCachePath = path + "." + entry.fileExtension;
				subCacheFilename = mainFileName + "." + entry.fileExtension;
			}

			auto subCacheFile = MMappedFileAccessor::Open(m_dscView, m_dscView->GetFile()->GetSessionId(), subCachePath)->lock();

			dyld_cache_header subCacheHeader {};
			uint64_t headerSize = subCacheFile->ReadUInt32(16);
			if (headerSize > sizeof(dyld_cache_header))
			{
				m_logger->LogDebug("Header size is larger than expected (0x%llx), using default size (0x%llx)", headerSize,
					sizeof(dyld_cache_header));
				headerSize = sizeof(dyld_cache_header);
			}
			subCacheFile->Read(&subCacheHeader, 0, headerSize);

			dyld_cache_mapping_info subCacheMapping {};

			BackingCache subCache;
			subCache.isPrimary = false;
			subCache.path = subCachePath;

			for (size_t j = 0; j < subCacheHeader.mappingCount; j++)
			{
				subCacheFile->Read(&subCacheMapping, subCacheHeader.mappingOffset + (j * sizeof(subCacheMapping)),
					sizeof(subCacheMapping));
				subCache.mappings.push_back(subCacheMapping);

				if (subCachePath.find(".dylddata") != std::string::npos)
				{
					auto pathBasename = subCachePath.substr(subCachePath.find_last_of("/\\") + 1);
					uint64_t address = subCacheMapping.address;
					uint64_t size = subCacheMapping.size;
					MemoryRegion dyldDataRegion;
					dyldDataRegion.start = address;
					dyldDataRegion.size = size;
					dyldDataRegion.prettyName = subCacheFilename + "::_data" + std::to_string(j);
					dyldDataRegion.flags = (BNSegmentFlag)(BNSegmentFlag::SegmentReadable);
					MutableState().dyldDataRegions.push_back(std::move(dyldDataRegion));
				}
			}

			MutableState().backingCaches.push_back(std::move(subCache));

			if (subCacheHeader.mappingCount == 1 && subCacheHeader.imagesCountOld == 0 && subCacheHeader.imagesCount == 0
				&& subCacheHeader.imagesTextOffset == 0)
			{
				auto pathBasename = subCachePath.substr(subCachePath.find_last_of("/\\") + 1);
				uint64_t address = subCacheMapping.address;
				uint64_t size = subCacheMapping.size;
				MemoryRegion stubIslandRegion;
				stubIslandRegion.start = address;
				stubIslandRegion.size = size;
				stubIslandRegion.prettyName = subCacheFilename + "::_stubs";
				stubIslandRegion.flags = (BNSegmentFlag)(BNSegmentFlag::SegmentReadable | BNSegmentFlag::SegmentExecutable);
				MutableState().stubIslandRegions.push_back(std::move(stubIslandRegion));
			}
		}

		// Load .symbols subcache
		try
		{
			auto subCachePath = path + ".symbols";
			auto subCacheFile = MMappedFileAccessor::Open(m_dscView, m_dscView->GetFile()->GetSessionId(), subCachePath)->lock();
			dyld_cache_header subCacheHeader {};
			uint64_t headerSize = subCacheFile->ReadUInt32(16);
			if (subCacheFile->ReadUInt32(16) > sizeof(dyld_cache_header))
			{
				m_logger->LogDebug("Header size is larger than expected, using default size");
				headerSize = sizeof(dyld_cache_header);
			}
			subCacheFile->Read(&subCacheHeader, 0, headerSize);

			BackingCache subCache;
			subCache.isPrimary = false;
			subCache.path = subCachePath;

			dyld_cache_mapping_info subCacheMapping {};

			for (size_t j = 0; j < subCacheHeader.mappingCount; j++)
			{
				subCacheFile->Read(&subCacheMapping, subCacheHeader.mappingOffset + (j * sizeof(subCacheMapping)),
					sizeof(subCacheMapping));
				subCache.mappings.push_back(subCacheMapping);
			}

			MutableState().backingCaches.push_back(std::move(subCache));
		}
		catch (...)
		{}
		break;
	}
	}
	baseFile.reset();

	m_viewSpecificState->progress = LoadProgressLoadingImages;

	// We have set up enough metadata to map VM now.

	auto vm = GetVMMap(true);
	if (!vm)
	{
		m_logger->LogError("Failed to map VM pages for Shared Cache on initial load, this is fatal.");
		return;
	}
	for (const auto& start : State().imageStarts)
	{
		try {
			auto imageHeader = SharedCache::LoadHeaderForAddress(vm, start.second, start.first);
			if (imageHeader)
			{
				if (imageHeader->linkeditPresent && vm->AddressIsMapped(imageHeader->linkeditSegment.vmaddr))
				{
					auto mapping = vm->MappingAtAddress(imageHeader->linkeditSegment.vmaddr);
					imageHeader->exportTriePath = mapping.first.fileAccessor->filePath();
				}
				MutableState().headers[start.second] = imageHeader.value();
				CacheImage image;
				image.installName = start.first;
				image.headerLocation = start.second;
				for (const auto& segment : imageHeader->segments)
				{
					char segName[17];
					memcpy(segName, segment.segname, 16);
					segName[16] = 0;
					MemoryRegion sectionRegion;
					sectionRegion.prettyName = imageHeader.value().identifierPrefix + "::" + std::string(segName);
					sectionRegion.start = segment.vmaddr;
					sectionRegion.size = segment.vmsize;
					uint32_t flags = SegmentFlagsFromMachOProtections(segment.initprot, segment.maxprot);

					// if we're positive we have an entry point for some reason, force the segment
					// executable. this helps with kernel images.
					for (auto &entryPoint : imageHeader->m_entryPoints)
						if (segment.vmaddr <= entryPoint && (entryPoint < (segment.vmaddr + segment.filesize)))
							flags |= SegmentExecutable;

					sectionRegion.flags = (BNSegmentFlag)flags;
					image.regions.push_back(sectionRegion);
				}
				MutableState().images.push_back(image);
			}
			else
			{
				m_logger->LogError("Failed to load Mach-O header for %s", start.first.c_str());
			}
		}
		catch (std::exception& ex)
		{
			m_logger->LogError("Failed to load Mach-O header for %s: %s", start.first.c_str(), ex.what());
		}
	}

	m_logger->LogInfo("Loaded %d Mach-O headers", State().headers.size());

	for (const auto& cache : State().backingCaches)
	{
		size_t i = 0;
		for (const auto& mapping : cache.mappings)
		{
			MemoryRegion region;
			region.start = mapping.address;
			region.size = mapping.size;
			region.prettyName = base_name(cache.path) + "::" + std::to_string(i);
			region.flags = SegmentFlagsFromMachOProtections(mapping.initProt, mapping.maxProt);
			MutableState().nonImageRegions.push_back(std::move(region));
			i++;
		}
	}

	// Iterate through each Mach-O header
	if (!State().dyldDataRegions.empty())
	{
		for (const auto& [headerKey, header] : State().headers)
		{
			// Iterate through each segment of the header
			for (const auto& segment : header.segments)
			{
				uint64_t segmentStart = segment.vmaddr;
				uint64_t segmentEnd = segmentStart + segment.vmsize;

				// Iterate through each region in m_dyldDataRegions
				for (auto it = State().dyldDataRegions.begin(); it != State().dyldDataRegions.end();)
				{
					uint64_t regionStart = it->start;
					uint64_t regionSize = it->size;
					uint64_t regionEnd = regionStart + regionSize;

					// Check if the region overlaps with the segment
					if (segmentStart < regionEnd && segmentEnd > regionStart)
					{
						// Split the region into two, removing the overlapped portion
						std::vector<MemoryRegion> newRegions;

						// Part before the overlap
						if (regionStart < segmentStart)
						{
							MemoryRegion newRegion(*it);
							newRegion.start = regionStart;
							newRegion.size = segmentStart - regionStart;
							newRegions.push_back(std::move(newRegion));
						}

						// Part after the overlap
						if (regionEnd > segmentEnd)
						{
							MemoryRegion newRegion(*it);
							newRegion.start = segmentEnd;
							newRegion.size = regionEnd - segmentEnd;
							newRegions.push_back(std::move(newRegion));
						}

						// Erase the original region
						it = MutableState().dyldDataRegions.erase(it);

						// Insert the new regions (if any)
						for (const auto& newRegion : newRegions)
						{
							it = MutableState().dyldDataRegions.insert(it, newRegion);
							++it;  // Move iterator to the next position
						}
					}
					else
					{
						++it;  // No overlap, move to the next region
					}
				}
			}
		}
	}

	// Iterate through each Mach-O header
	if (!State().nonImageRegions.empty())
	{
		for (const auto& [headerKey, header] : State().headers)
		{
			// Iterate through each segment of the header
			for (const auto& segment : header.segments)
			{
				uint64_t segmentStart = segment.vmaddr;
				uint64_t segmentEnd = segmentStart + segment.vmsize;

				// Iterate through each region in m_dyldDataRegions
				for (auto it = State().nonImageRegions.begin(); it != State().nonImageRegions.end();)
				{
					uint64_t regionStart = it->start;
					uint64_t regionSize = it->size;
					uint64_t regionEnd = regionStart + regionSize;

					// Check if the region overlaps with the segment
					if (segmentStart < regionEnd && segmentEnd > regionStart)
					{
						// Split the region into two, removing the overlapped portion
						std::vector<MemoryRegion> newRegions;

						// Part before the overlap
						if (regionStart < segmentStart)
						{
							MemoryRegion newRegion(*it);
							newRegion.start = regionStart;
							newRegion.size = segmentStart - regionStart;
							newRegions.push_back(std::move(newRegion));
						}

						// Part after the overlap
						if (regionEnd > segmentEnd)
						{
							MemoryRegion newRegion(*it);
							newRegion.start = segmentEnd;
							newRegion.size = regionEnd - segmentEnd;
							newRegions.push_back(std::move(newRegion));
						}

						// Erase the original region
						it = MutableState().nonImageRegions.erase(it);

						// Insert the new regions (if any)
						for (const auto& newRegion : newRegions)
						{
							it = MutableState().nonImageRegions.insert(it, newRegion);
							++it;  // Move iterator to the next position
						}
					}
					else
					{
						++it;  // No overlap, move to the next region
					}
				}
			}
		}
	}
	SaveToDSCView();

	m_logger->LogDebug("Finished initial load of Shared Cache");

	m_viewSpecificState->progress = LoadProgressFinished;
}

std::shared_ptr<VM> SharedCache::GetVMMap(bool mapPages)
{
	std::shared_ptr<VM> vm = std::make_shared<VM>(0x1000);

	if (mapPages)
	{
		for (const auto& cache : State().backingCaches)
		{
			for (const auto& mapping : cache.mappings)
			{
				vm->MapPages(m_dscView, m_dscView->GetFile()->GetSessionId(), mapping.address, mapping.fileOffset, mapping.size, cache.path,
					[this, vm=vm](std::shared_ptr<MMappedFileAccessor> mmap){
						ParseAndApplySlideInfoForFile(mmap);
					});
			}
		}
	}

	return vm;
}


void SharedCache::DeserializeFromRawView()
{
	if (m_dscView->QueryMetadata(SharedCacheMetadataTag))
	{
		std::lock_guard lock(m_viewSpecificState->stateMutex);
		if (m_viewSpecificState->cachedState)
		{
			m_state = m_viewSpecificState->cachedState;
			m_stateIsShared = true;
			m_metadataValid = true;
		}
		else
		{
			LoadFromString(m_dscView->GetStringMetadata(SharedCacheMetadataTag));
		}
		if (!m_metadataValid)
		{
			m_logger->LogError("Failed to deserialize Shared Cache metadata");
			WillMutateState();
			MutableState().viewState = DSCViewStateUnloaded;
		}
	}
	else
	{
		m_metadataValid = true;
		WillMutateState();
		MutableState().viewState = DSCViewStateUnloaded;
		MutableState().images.clear();	// fixme ??
	}
}


std::string to_hex_string(uint64_t value)
{
	std::stringstream ss;
	ss << std::hex << value;
	return ss.str();
}


void SharedCache::ParseAndApplySlideInfoForFile(std::shared_ptr<MMappedFileAccessor> file)
{
	if (file->SlideInfoWasApplied())
		return;

	WillMutateState();
	std::vector<std::pair<uint64_t, uint64_t>> rewrites;

	dyld_cache_header baseHeader;
	file->Read(&baseHeader, 0, sizeof(dyld_cache_header));
	uint64_t base = UINT64_MAX;
	for (const auto& backingCache : State().backingCaches)
	{
		for (const auto& mapping : backingCache.mappings)
		{
			if (mapping.address < base)
			{
				base = mapping.address;
				break;
			}
		}
	}

	std::vector<std::pair<uint64_t, MappingInfo>> mappings;

	if (baseHeader.slideInfoOffsetUnused)
	{
		// Legacy

		auto slideInfoOff = baseHeader.slideInfoOffsetUnused;
		auto slideInfoVersion = file->ReadUInt32(slideInfoOff);
		if (slideInfoVersion != 2 && slideInfoVersion != 3)
		{
			abort();
		}

		MappingInfo map;

		file->Read(&map.mappingInfo, baseHeader.mappingOffset + sizeof(dyld_cache_mapping_info), sizeof(dyld_cache_mapping_info));
		map.file = file;
		map.slideInfoVersion = slideInfoVersion;
		if (map.slideInfoVersion == 2)
			file->Read(&map.slideInfoV2, slideInfoOff, sizeof(dyld_cache_slide_info_v2));
		else if (map.slideInfoVersion == 3)
			file->Read(&map.slideInfoV3, slideInfoOff, sizeof(dyld_cache_slide_info_v3));

		mappings.emplace_back(slideInfoOff, map);
	}
	else
	{
		dyld_cache_header targetHeader;
		file->Read(&targetHeader, 0, sizeof(dyld_cache_header));

		if (targetHeader.mappingWithSlideCount == 0)
		{
			m_logger->LogDebug("No mappings with slide info found");
		}

		for (auto i = 0; i < targetHeader.mappingWithSlideCount; i++)
		{
			dyld_cache_mapping_and_slide_info mappingAndSlideInfo;
			file->Read(&mappingAndSlideInfo, targetHeader.mappingWithSlideOffset + (i * sizeof(dyld_cache_mapping_and_slide_info)), sizeof(dyld_cache_mapping_and_slide_info));
			if (mappingAndSlideInfo.slideInfoFileOffset)
			{
				MappingInfo map;
				map.file = file;
				if (mappingAndSlideInfo.size == 0)
					continue;
				map.slideInfoVersion = file->ReadUInt32(mappingAndSlideInfo.slideInfoFileOffset);
				m_logger->LogDebug("Slide Info Version: %d", map.slideInfoVersion);
				map.mappingInfo.address = mappingAndSlideInfo.address;
				map.mappingInfo.size = mappingAndSlideInfo.size;
				map.mappingInfo.fileOffset = mappingAndSlideInfo.fileOffset;
				if (map.slideInfoVersion == 2)
				{
					file->Read(
						&map.slideInfoV2, mappingAndSlideInfo.slideInfoFileOffset, sizeof(dyld_cache_slide_info_v2));
				}
				else if (map.slideInfoVersion == 3)
				{
					file->Read(
						&map.slideInfoV3, mappingAndSlideInfo.slideInfoFileOffset, sizeof(dyld_cache_slide_info_v3));
					map.slideInfoV3.auth_value_add = base;
				}
				else if (map.slideInfoVersion == 5)
				{
					file->Read(
						&map.slideInfoV5, mappingAndSlideInfo.slideInfoFileOffset, sizeof(dyld_cache_slide_info5));
					map.slideInfoV5.value_add = base;
				}
				else
				{
					m_logger->LogError("Unknown slide info version: %d", map.slideInfoVersion);
					continue;
				}

				uint64_t slideInfoOffset = mappingAndSlideInfo.slideInfoFileOffset;
				mappings.emplace_back(slideInfoOffset, map);
				m_logger->LogDebug("Filename: %s", file->Path().c_str());
				m_logger->LogDebug("Slide Info Offset: 0x%llx", slideInfoOffset);
				m_logger->LogDebug("Mapping Address: 0x%llx", map.mappingInfo.address);
				m_logger->LogDebug("Slide Info v", map.slideInfoVersion);
			}
		}
	}

	if (mappings.empty())
	{
		m_logger->LogDebug("No slide info found");
		file->SetSlideInfoWasApplied(true);
		return;
	}

	for (const auto& [off, mapping] : mappings)
	{
		m_logger->LogDebug("Slide Info Version: %d", mapping.slideInfoVersion);
		uint64_t extrasOffset = off;
		uint64_t pageStartsOffset = off;
		uint64_t pageStartCount;
		uint64_t pageSize;

		if (mapping.slideInfoVersion == 2)
		{
			pageStartsOffset += mapping.slideInfoV2.page_starts_offset;
			pageStartCount = mapping.slideInfoV2.page_starts_count;
			pageSize = mapping.slideInfoV2.page_size;
			extrasOffset += mapping.slideInfoV2.page_extras_offset;
			auto cursor = pageStartsOffset;

			for (size_t i = 0; i < pageStartCount; i++)
			{
				try
				{
					uint16_t start = mapping.file->ReadUShort(cursor);
					cursor += sizeof(uint16_t);
					if (start == DYLD_CACHE_SLIDE_PAGE_ATTR_NO_REBASE)
						continue;

					auto rebaseChain = [&](const dyld_cache_slide_info_v2& slideInfo, uint64_t pageContent, uint16_t startOffset)
					{
						uintptr_t slideAmount = 0;

						auto deltaMask = slideInfo.delta_mask;
						auto valueMask = ~deltaMask;
						auto valueAdd = slideInfo.value_add;

						auto deltaShift = count_trailing_zeros(deltaMask) - 2;

						uint32_t pageOffset = startOffset;
						uint32_t delta = 1;
						while ( delta != 0 )
						{
							uint64_t loc = pageContent + pageOffset;
							try
							{
								uintptr_t rawValue = file->ReadULong(loc);
								delta = (uint32_t)((rawValue & deltaMask) >> deltaShift);
								uintptr_t value = (rawValue & valueMask);
								if (value != 0)
								{
									value += valueAdd;
									value += slideAmount;
								}
								pageOffset += delta;
								rewrites.emplace_back(loc, value);
							}
							catch (MappingReadException& ex)
							{
								m_logger->LogError("Failed to read v2 slide pointer at 0x%llx\n", loc);
								break;
							}
						}
					};

					if (start & DYLD_CACHE_SLIDE_PAGE_ATTR_EXTRA)
					{
						int j=(start & 0x3FFF);
						bool done = false;
						do
						{
							uint64_t extraCursor = extrasOffset + (j * sizeof(uint16_t));
							try
							{
								auto extra = mapping.file->ReadUShort(extraCursor);
								uint16_t aStart = extra;
								uint64_t page = mapping.mappingInfo.fileOffset + (pageSize * i);
								uint16_t pageStartOffset = (aStart & 0x3FFF)*4;
								rebaseChain(mapping.slideInfoV2, page, pageStartOffset);
								done = (extra & DYLD_CACHE_SLIDE_PAGE_ATTR_END);
								++j;
							}
							catch (MappingReadException& ex)
							{
								m_logger->LogError("Failed to read v2 slide extra at 0x%llx\n", cursor);
								break;
							}
						} while (!done);
					}
					else
					{
						uint64_t page = mapping.mappingInfo.fileOffset + (pageSize * i);
						uint16_t pageStartOffset = start*4;
						rebaseChain(mapping.slideInfoV2, page, pageStartOffset);
					}
				}
				catch (MappingReadException& ex)
				{
					m_logger->LogError("Failed to read v2 slide info at 0x%llx\n", cursor);
				}
			}
		}
		else if (mapping.slideInfoVersion == 3) {
			// Slide Info Version 3 Logic
			pageStartsOffset += sizeof(dyld_cache_slide_info_v3);
			pageStartCount = mapping.slideInfoV3.page_starts_count;
			pageSize = mapping.slideInfoV3.page_size;
			auto cursor = pageStartsOffset;

			for (size_t i = 0; i < pageStartCount; i++)
			{
				try
				{
					uint16_t delta = mapping.file->ReadUShort(cursor);
					cursor += sizeof(uint16_t);
					if (delta == DYLD_CACHE_SLIDE_V3_PAGE_ATTR_NO_REBASE)
						continue;

					delta = delta/sizeof(uint64_t); // initial offset is byte based
					uint64_t loc = mapping.mappingInfo.fileOffset + (pageSize * i);
					do
					{
						loc += delta * sizeof(dyld_cache_slide_pointer3);
						try
						{
							dyld_cache_slide_pointer3 slideInfo;
							file->Read(&slideInfo, loc, sizeof(slideInfo));
							delta = slideInfo.plain.offsetToNextPointer;

							if (slideInfo.auth.authenticated)
							{
								uint64_t value = slideInfo.auth.offsetFromSharedCacheBase;
								value += mapping.slideInfoV3.auth_value_add;
								rewrites.emplace_back(loc, value);
							}
							else
							{
								uint64_t value51 = slideInfo.plain.pointerValue;
								uint64_t top8Bits = value51 & 0x0007F80000000000;
								uint64_t bottom43Bits = value51 & 0x000007FFFFFFFFFF;
								uint64_t value = (uint64_t)top8Bits << 13 | bottom43Bits;
								rewrites.emplace_back(loc, value);
							}
						}
						catch (MappingReadException& ex)
						{
							m_logger->LogError("Failed to read v3 slide pointer at 0x%llx\n", loc);
							break;
						}
					} while (delta != 0);
				}
				catch (MappingReadException& ex)
				{
					m_logger->LogError("Failed to read v3 slide info at 0x%llx\n", cursor);
				}
			}
		}
		else if (mapping.slideInfoVersion == 5)
		{
			pageStartsOffset += sizeof(dyld_cache_slide_info5);
			pageStartCount = mapping.slideInfoV5.page_starts_count;
			pageSize = mapping.slideInfoV5.page_size;
			auto cursor = pageStartsOffset;

			for (size_t i = 0; i < pageStartCount; i++)
			{
				try
				{
					uint16_t delta = mapping.file->ReadUShort(cursor);
					cursor += sizeof(uint16_t);
					if (delta == DYLD_CACHE_SLIDE_V5_PAGE_ATTR_NO_REBASE)
						continue;

					delta = delta/sizeof(uint64_t); // initial offset is byte based
					uint64_t loc = mapping.mappingInfo.fileOffset + (pageSize * i);
					do
					{
						loc += delta * sizeof(dyld_cache_slide_pointer5);
						try
						{
							dyld_cache_slide_pointer5 slideInfo;
							file->Read(&slideInfo, loc, sizeof(slideInfo));
							delta = slideInfo.regular.next;
							if (slideInfo.auth.auth)
							{
								uint64_t value = mapping.slideInfoV5.value_add + slideInfo.auth.runtimeOffset;
								rewrites.emplace_back(loc, value);
							}
							else
							{
								uint64_t value = mapping.slideInfoV5.value_add + slideInfo.regular.runtimeOffset;
								rewrites.emplace_back(loc, value);
							}
						}
						catch (MappingReadException& ex)
						{
							m_logger->LogError("Failed to read v5 slide pointer at 0x%llx\n", loc);
							break;
						}
					} while (delta != 0);
				}
				catch (MappingReadException& ex)
				{
					m_logger->LogError("Failed to read v5 slide info at 0x%llx\n", cursor);
				}
			}
		}
	}
	for (const auto& [loc, value] : rewrites)
	{
		file->WritePointer(loc, value);
#ifdef SLIDEINFO_DEBUG_TAGS
		uint64_t vmAddr = 0;
		{
			for (uint64_t off = baseHeader.mappingOffset; off < baseHeader.mappingOffset + baseHeader.mappingCount * sizeof(dyld_cache_mapping_info); off += sizeof(dyld_cache_mapping_info))
			{
				dyld_cache_mapping_info mapping;
				file->Read(&mapping, off, sizeof(dyld_cache_mapping_info));
				if (mapping.fileOffset <= loc && loc < mapping.fileOffset + mapping.size)
				{
					vmAddr = mapping.address + (loc - mapping.fileOffset);
					break;
				}
			}
		}
		Ref<TagType> type = m_dscView->GetTagType("slideinfo");
		if (!type)
		{
			m_dscView->AddTagType(new TagType(m_dscView, "slideinfo", "\xF0\x9F\x9A\x9E"));
			type = m_dscView->GetTagType("slideinfo");
		}
		m_dscView->AddAutoDataTag(vmAddr, new Tag(type, "0x" + to_hex_string(file->ReadULong(loc)) + " => 0x" + to_hex_string(value)));
#endif
	}
	m_logger->LogDebug("Applied slide info for %s (0x%llx rewrites)", file->Path().c_str(), rewrites.size());
	file->SetSlideInfoWasApplied(true);
}


SharedCache::SharedCache(BinaryNinja::Ref<BinaryNinja::BinaryView> dscView) : m_dscView(dscView), m_viewSpecificState(ViewSpecificStateForView(dscView))
{
	if (dscView->GetTypeName() != VIEW_NAME)
	{
		// Unreachable?
		m_logger->LogError("Attempted to create SharedCache object from non-Shared Cache view");
		return;
	}
	sharedCacheReferences++;
	INIT_SHAREDCACHE_API_OBJECT()
	m_logger = LogRegistry::GetLogger("SharedCache", dscView->GetFile()->GetSessionId());
	DeserializeFromRawView();
	if (!m_metadataValid)
		return;

	if (State().viewState != DSCViewStateUnloaded) {
		m_viewSpecificState->progress = LoadProgressFinished;
		return;
	}

	std::unique_lock lock(m_viewSpecificState->viewOperationsThatInfluenceMetadataMutex);
	try {
		PerformInitialLoad();
	}
	catch (...)
	{
		m_logger->LogError("Failed to perform initial load of Shared Cache");
	}

	auto settings = m_dscView->GetLoadSettings(VIEW_NAME);
	bool autoLoadLibsystem = true;
	if (settings && settings->Contains("loader.dsc.autoLoadLibSystem"))
	{
		autoLoadLibsystem = settings->Get<bool>("loader.dsc.autoLoadLibSystem", m_dscView);
	}
	if (autoLoadLibsystem)
	{
		for (const auto& [_, header] : State().headers)
		{
			if (header.installName.find("libsystem_c.dylib") != std::string::npos)
			{
				lock.unlock();
				m_logger->LogInfo("Loading core libsystem_c.dylib library");
				LoadImageWithInstallName(header.installName, false);
				break;
			}
		}
	}

	MutableState().viewState = DSCViewStateLoaded;
	SaveToDSCView();
}

SharedCache::~SharedCache() {
	sharedCacheReferences--;
}

SharedCache* SharedCache::GetFromDSCView(BinaryNinja::Ref<BinaryNinja::BinaryView> dscView)
{
	if (dscView->GetTypeName() != VIEW_NAME)
		return nullptr;
	try {
		return new SharedCache(dscView);
	}
	catch (...)
	{
		return nullptr;
	}
}

std::optional<uint64_t> SharedCache::GetImageStart(std::string installName)
{
	for (const auto& [name, start] : State().imageStarts)
	{
		if (name == installName)
		{
			return start;
		}
	}
	return {};
}

std::optional<SharedCacheMachOHeader> SharedCache::HeaderForAddress(uint64_t address)
{
	// We _could_ mark each page with the image start? :grimacing emoji:
	// But that'd require mapping pages :grimacing emoji: :grimacing emoji:
	// There's not really any other hacks that could make this faster, that I can think of...
	for (const auto& [start, header] : State().headers)
	{
		for (const auto& segment : header.segments)
		{
			if (segment.vmaddr <= address && segment.vmaddr + segment.vmsize > address)
			{
				return header;
			}
		}
	}
	return {};
}

std::string SharedCache::NameForAddress(uint64_t address)
{
	for (const auto& stubIsland : State().stubIslandRegions)
	{
		if (stubIsland.start <= address && stubIsland.start + stubIsland.size > address)
		{
			return stubIsland.prettyName;
		}
	}
	for (const auto& dyldData : State().dyldDataRegions)
	{
		if (dyldData.start <= address && dyldData.start + dyldData.size > address)
		{
			return dyldData.prettyName;
		}
	}
	for (const auto& nonImageRegion : State().nonImageRegions)
	{
		if (nonImageRegion.start <= address && nonImageRegion.start + nonImageRegion.size > address)
		{
			return nonImageRegion.prettyName;
		}
	}
	if (auto header = HeaderForAddress(address))
	{
		for (const auto& section : header->sections)
		{
			if (section.addr <= address && section.addr + section.size > address)
			{
				char sectionName[17];
				strncpy(sectionName, section.sectname, 16);
				sectionName[16] = '\0';
				return header->identifierPrefix + "::" + sectionName;
			}
		}
	}
	return "";
}

std::string SharedCache::ImageNameForAddress(uint64_t address)
{
	if (auto header = HeaderForAddress(address))
	{
		return header->identifierPrefix;
	}
	return "";
}

bool SharedCache::LoadImageContainingAddress(uint64_t address, bool skipObjC)
{
	for (const auto& [start, header] : State().headers)
	{
		for (const auto& segment : header.segments)
		{
			if (segment.vmaddr <= address && segment.vmaddr + segment.vmsize > address)
			{
				return LoadImageWithInstallName(header.installName, skipObjC);
			}
		}
	}

	return false;
}

bool SharedCache::LoadSectionAtAddress(uint64_t address)
{
	std::unique_lock lock(m_viewSpecificState->viewOperationsThatInfluenceMetadataMutex);
	DeserializeFromRawView();
	WillMutateState();

	auto vm = GetVMMap();
	if (!vm)
	{
		m_logger->LogError("Failed to map VM pages for Shared Cache.");
		return false;
	}

	SharedCacheMachOHeader targetHeader;
	CacheImage* targetImage = nullptr;
	MemoryRegion* targetSegment = nullptr;

	for (auto& image : MutableState().images)
	{
		for (auto& region : image.regions)
		{
			if (region.start <= address && region.start + region.size > address)
			{
				targetHeader = MutableState().headers[image.headerLocation];
				targetImage = &image;
				targetSegment = &region;
				break;
			}
		}
		if (targetSegment)
			break;
	}
	if (!targetSegment)
	{
		for (auto& stubIsland : MutableState().stubIslandRegions)
		{
			if (stubIsland.start <= address && stubIsland.start + stubIsland.size > address)
			{
				if (stubIsland.loaded)
				{
					return true;
				}
				m_logger->LogInfo("Loading stub island %s @ 0x%llx", stubIsland.prettyName.c_str(), stubIsland.start);
				auto targetFile = vm->MappingAtAddress(stubIsland.start).first.fileAccessor->lock();
				ParseAndApplySlideInfoForFile(targetFile);
				auto reader = VMReader(vm);
				auto buff = reader.ReadBuffer(stubIsland.start, stubIsland.size);
				m_dscView->GetMemoryMap()->AddDataMemoryRegion(stubIsland.prettyName, stubIsland.start, buff, SegmentReadable | SegmentExecutable);
				m_dscView->AddUserSection(stubIsland.prettyName, stubIsland.start, stubIsland.size, ReadOnlyCodeSectionSemantics);

				stubIsland.loaded = true;

				MutableState().regionsMappedIntoMemory.push_back(stubIsland);

				SaveToDSCView();

				m_dscView->AddAnalysisOption("linearsweep");
				m_dscView->UpdateAnalysis();

				return true;
			}
		}

		for (auto& dyldData : MutableState().dyldDataRegions)
		{
			if (dyldData.start <= address && dyldData.start + dyldData.size > address)
			{
				if (dyldData.loaded)
				{
					return true;
				}
				m_logger->LogInfo("Loading dyld data %s", dyldData.prettyName.c_str());
				auto targetFile = vm->MappingAtAddress(dyldData.start).first.fileAccessor->lock();
				ParseAndApplySlideInfoForFile(targetFile);
				auto reader = VMReader(vm);
				auto buff = reader.ReadBuffer(dyldData.start, dyldData.size);
				m_dscView->GetMemoryMap()->AddDataMemoryRegion(dyldData.prettyName, dyldData.start, buff, SegmentReadable);
				m_dscView->AddUserSection(dyldData.prettyName, dyldData.start, dyldData.size, ReadOnlyDataSectionSemantics);

				dyldData.loaded = true;

				MutableState().regionsMappedIntoMemory.push_back(dyldData);

				SaveToDSCView();

				m_dscView->AddAnalysisOption("linearsweep");
				m_dscView->UpdateAnalysis();

				return true;
			}
		}

		for (auto& region : MutableState().nonImageRegions)
		{
			if (region.start <= address && region.start + region.size > address)
			{
				if (region.loaded)
				{
					return true;
				}
				m_logger->LogInfo("Loading non-image region %s", region.prettyName.c_str());
				auto targetFile = vm->MappingAtAddress(region.start).first.fileAccessor->lock();
				ParseAndApplySlideInfoForFile(targetFile);
				auto reader = VMReader(vm);
				auto buff = reader.ReadBuffer(region.start, region.size);
				m_dscView->GetMemoryMap()->AddDataMemoryRegion(region.prettyName, region.start, buff, region.flags);
				m_dscView->AddUserSection(region.prettyName, region.start, region.size, region.flags & SegmentDenyExecute ? ReadOnlyDataSectionSemantics : ReadOnlyCodeSectionSemantics);

				region.loaded = true;

				MutableState().regionsMappedIntoMemory.push_back(region);

				SaveToDSCView();

				m_dscView->AddAnalysisOption("linearsweep");
				m_dscView->UpdateAnalysis();

				return true;
			}
		}

		m_logger->LogError("Failed to find a segment containing address 0x%llx", address);
		return false;
	}

	auto id = m_dscView->BeginUndoActions();
	auto reader = VMReader(vm);

	m_logger->LogDebug("Partial loading image %s", targetHeader.installName.c_str());

	auto targetFile = vm->MappingAtAddress(targetSegment->start).first.fileAccessor->lock();
	ParseAndApplySlideInfoForFile(targetFile);
	auto buff = reader.ReadBuffer(targetSegment->start, targetSegment->size);
	m_dscView->GetMemoryMap()->AddDataMemoryRegion(targetSegment->prettyName, targetSegment->start, buff, targetSegment->flags);

	targetSegment->loaded = true;

	MutableState().regionsMappedIntoMemory.push_back(*targetSegment);

	SaveToDSCView();

	if (!targetSegment->headerInitialized)
	{
		SharedCache::InitializeHeader(m_dscView, vm.get(), targetHeader, {targetSegment});
	}

	m_dscView->AddAnalysisOption("linearsweep");
	m_dscView->UpdateAnalysis();

	m_dscView->CommitUndoActions(id);

	return true;
}

static void GetObjCSettings(Ref<BinaryView> view, bool* processObjCMetadata, bool* processCFStrings)
{
	auto settings = view->GetLoadSettings(VIEW_NAME);
	*processCFStrings = true;
	*processObjCMetadata = true;
	if (settings && settings->Contains("loader.dsc.processCFStrings"))
		*processCFStrings = settings->Get<bool>("loader.dsc.processCFStrings", view);
	if (settings && settings->Contains("loader.dsc.processObjC"))
		*processObjCMetadata = settings->Get<bool>("loader.dsc.processObjC", view);
}

static void ProcessObjCSectionsForImageWithName(std::string baseName, std::shared_ptr<VM> vm, std::shared_ptr<DSCObjC::DSCObjCProcessor> objc, bool processCFStrings, bool processObjCMetadata, Ref<Logger> logger)
{
	try
	{
		if (processObjCMetadata)
			objc->ProcessObjCData(vm, baseName);
		if (processCFStrings)
			objc->ProcessCFStrings(vm, baseName);
	}
	catch (const std::exception& ex)
	{
		logger->LogWarn("Error processing ObjC data for image %s: %s", baseName.c_str(), ex.what());
	}
	catch (...)
	{
		logger->LogWarn("Error processing ObjC data for image %s", baseName.c_str());
	}
}

void SharedCache::ProcessObjCSectionsForImageWithInstallName(std::string installName)
{
	bool processCFStrings;
	bool processObjCMetadata;
	GetObjCSettings(m_dscView, &processCFStrings, &processObjCMetadata);

	if (!processObjCMetadata && !processCFStrings)
		return;

	auto objc = std::make_shared<DSCObjC::DSCObjCProcessor>(m_dscView, this, false);
	auto vm = GetVMMap();

	ProcessObjCSectionsForImageWithName(base_name(installName), vm, objc, processCFStrings, processObjCMetadata, m_logger);
}

void SharedCache::ProcessAllObjCSections()
{
	bool processCFStrings;
	bool processObjCMetadata;
	GetObjCSettings(m_dscView, &processCFStrings, &processObjCMetadata);

	if (!processObjCMetadata && !processCFStrings)
		return;

	auto objc = std::make_shared<DSCObjC::DSCObjCProcessor>(m_dscView, this, false);
	auto vm = GetVMMap();

	std::set<uint64_t> processedImageHeaders;
	for (auto region : GetMappedRegions())
	{
		if (!region.loaded)
			continue;

		// Don't repeat the same images multiple times
		auto header = HeaderForAddress(region.start);
		if (!header)
			continue;
		if (processedImageHeaders.find(header->textBase) != processedImageHeaders.end())
			continue;
		processedImageHeaders.insert(header->textBase);

		ProcessObjCSectionsForImageWithName(header->identifierPrefix, vm, objc, processCFStrings, processObjCMetadata, m_logger);
	}
}

bool SharedCache::LoadImageWithInstallName(std::string installName, bool skipObjC)
{
	auto settings = m_dscView->GetLoadSettings(VIEW_NAME);

	std::unique_lock lock(m_viewSpecificState->viewOperationsThatInfluenceMetadataMutex);

	DeserializeFromRawView();
	WillMutateState();

	m_logger->LogInfo("Loading image %s", installName.c_str());

	auto vm = GetVMMap();
	CacheImage* targetImage = nullptr;

	for (auto& cacheImage : MutableState().images)
	{
		if (cacheImage.installName == installName)
		{
			targetImage = &cacheImage;
			break;
		}
	}
	auto it = State().headers.find(targetImage->headerLocation);
	if (it == State().headers.end())
	{
		return false;
	}
	const auto& header = it->second;

	auto id = m_dscView->BeginUndoActions();
	MutableState().viewState = DSCViewStateLoadedWithImages;

	auto reader = VMReader(vm);
	reader.Seek(targetImage->headerLocation);

	std::vector<MemoryRegion*> regionsToLoad;

	for (auto& region : targetImage->regions)
	{
		bool allowLoadingLinkedit = false;
		if (settings && settings->Contains("loader.dsc.allowLoadingLinkeditSegments"))
			allowLoadingLinkedit = settings->Get<bool>("loader.dsc.allowLoadingLinkeditSegments", m_dscView);
		if ((region.prettyName.find("__LINKEDIT") != std::string::npos) && !allowLoadingLinkedit)
			continue;

		if (region.loaded)
		{
			m_logger->LogDebug("Skipping region %s as it is already loaded.", region.prettyName.c_str());
			continue;
		}

		auto targetFile = vm->MappingAtAddress(region.start).first.fileAccessor->lock();
		ParseAndApplySlideInfoForFile(targetFile);

		auto buff = reader.ReadBuffer(region.start, region.size);

		region.loaded = true;

		MutableState().regionsMappedIntoMemory.push_back(region);
		m_dscView->GetMemoryMap()->AddDataMemoryRegion(region.prettyName, region.start, buff, region.flags);

		regionsToLoad.push_back(&region);
	}

	if (regionsToLoad.empty())
	{
		m_logger->LogWarn("No regions to load for image %s", installName.c_str());
		return false;
	}

	auto typeLib = TypeLibraryForImage(header.installName);

	SaveToDSCView();

	auto h = SharedCache::LoadHeaderForAddress(vm, targetImage->headerLocation, installName);
	if (!h.has_value())
	{
		return false;
	}

	std::vector<MemoryRegion*> regions;
	for (auto& region : regionsToLoad)
	{
		regions.push_back(region);
	}

	SharedCache::InitializeHeader(m_dscView, vm.get(), *h, regions);

	if (!skipObjC)
	{
		bool processCFStrings;
		bool processObjCMetadata;
		GetObjCSettings(m_dscView, &processCFStrings, &processObjCMetadata);

		ProcessObjCSectionsForImageWithName(h->identifierPrefix, vm, std::make_shared<DSCObjC::DSCObjCProcessor>(m_dscView, this, false), processCFStrings, processObjCMetadata, m_logger);
	}

	m_dscView->AddAnalysisOption("linearsweep");
	m_dscView->UpdateAnalysis();

	m_dscView->CommitUndoActions(id);

	return true;
}

std::optional<SharedCacheMachOHeader> SharedCache::LoadHeaderForAddress(std::shared_ptr<VM> vm, uint64_t address, std::string installName)
{
	SharedCacheMachOHeader header;

	header.textBase = address;
	header.installName = installName;
	header.identifierPrefix = base_name(installName);

	std::string errorMsg;
	// address is a Raw file offset
	VMReader reader(vm);
	reader.Seek(address);

	header.ident.magic = reader.Read32();

	BNEndianness endianness;
	if (header.ident.magic == MH_MAGIC || header.ident.magic == MH_MAGIC_64)
		endianness = LittleEndian;
	else if (header.ident.magic == MH_CIGAM || header.ident.magic == MH_CIGAM_64)
		endianness = BigEndian;
	else
	{
		return {};
	}

	reader.SetEndianness(endianness);
	header.ident.cputype = reader.Read32();
	header.ident.cpusubtype = reader.Read32();
	header.ident.filetype = reader.Read32();
	header.ident.ncmds = reader.Read32();
	header.ident.sizeofcmds = reader.Read32();
	header.ident.flags = reader.Read32();
	if ((header.ident.cputype & MachOABIMask) == MachOABI64)  // address size == 8
	{
		header.ident.reserved = reader.Read32();
	}
	header.loadCommandOffset = reader.GetOffset();

	bool first = true;
	// Parse segment commands
	try
	{
		for (size_t i = 0; i < header.ident.ncmds; i++)
		{
			// BNLogInfo("of 0x%llx", reader.GetOffset());
			load_command load;
			segment_command_64 segment64;
			section_64 sect;
			memset(&sect, 0, sizeof(sect));
			size_t curOffset = reader.GetOffset();
			load.cmd = reader.Read32();
			load.cmdsize = reader.Read32();
			size_t nextOffset = curOffset + load.cmdsize;
			if (load.cmdsize < sizeof(load_command))
				return {};

			switch (load.cmd)
			{
			case LC_MAIN:
			{
				uint64_t entryPoint = reader.Read64();
				header.entryPoints.push_back({entryPoint, true});
				(void)reader.Read64();	// Stack start
				break;
			}
			case LC_SEGMENT:  // map the 32bit version to 64 bits
				segment64.cmd = LC_SEGMENT_64;
				reader.Read(&segment64.segname, 16);
				segment64.vmaddr = reader.Read32();
				segment64.vmsize = reader.Read32();
				segment64.fileoff = reader.Read32();
				segment64.filesize = reader.Read32();
				segment64.maxprot = reader.Read32();
				segment64.initprot = reader.Read32();
				segment64.nsects = reader.Read32();
				segment64.flags = reader.Read32();
				if (first)
				{
					if (!((header.ident.flags & MH_SPLIT_SEGS) || header.ident.cputype == MACHO_CPU_TYPE_X86_64)
						|| (segment64.flags & MACHO_VM_PROT_WRITE))
					{
						header.relocationBase = segment64.vmaddr;
						first = false;
					}
				}
				for (size_t j = 0; j < segment64.nsects; j++)
				{
					reader.Read(&sect.sectname, 16);
					reader.Read(&sect.segname, 16);
					sect.addr = reader.Read32();
					sect.size = reader.Read32();
					sect.offset = reader.Read32();
					sect.align = reader.Read32();
					sect.reloff = reader.Read32();
					sect.nreloc = reader.Read32();
					sect.flags = reader.Read32();
					sect.reserved1 = reader.Read32();
					sect.reserved2 = reader.Read32();
					// if the segment isn't mapped into virtual memory don't add the corresponding sections.
					if (segment64.vmsize > 0)
					{
						header.sections.push_back(sect);
					}
					if (!strncmp(sect.sectname, "__mod_init_func", 15))
						header.moduleInitSections.push_back(sect);
					if ((sect.flags & (S_ATTR_SELF_MODIFYING_CODE | S_SYMBOL_STUBS))
						== (S_ATTR_SELF_MODIFYING_CODE | S_SYMBOL_STUBS))
						header.symbolStubSections.push_back(sect);
					if ((sect.flags & S_NON_LAZY_SYMBOL_POINTERS) == S_NON_LAZY_SYMBOL_POINTERS)
						header.symbolPointerSections.push_back(sect);
					if ((sect.flags & S_LAZY_SYMBOL_POINTERS) == S_LAZY_SYMBOL_POINTERS)
						header.symbolPointerSections.push_back(sect);
				}
				header.segments.push_back(segment64);
				break;
			case LC_SEGMENT_64:
				segment64.cmd = LC_SEGMENT_64;
				reader.Read(&segment64.segname, 16);
				segment64.vmaddr = reader.Read64();
				segment64.vmsize = reader.Read64();
				segment64.fileoff = reader.Read64();
				segment64.filesize = reader.Read64();
				segment64.maxprot = reader.Read32();
				segment64.initprot = reader.Read32();
				segment64.nsects = reader.Read32();
				segment64.flags = reader.Read32();
				if (strncmp(segment64.segname, "__LINKEDIT", 10) == 0)
				{
					header.linkeditSegment = segment64;
					header.linkeditPresent = true;
				}
				if (first)
				{
					if (!((header.ident.flags & MH_SPLIT_SEGS) || header.ident.cputype == MACHO_CPU_TYPE_X86_64)
						|| (segment64.flags & MACHO_VM_PROT_WRITE))
					{
						header.relocationBase = segment64.vmaddr;
						first = false;
					}
				}
				for (size_t j = 0; j < segment64.nsects; j++)
				{
					reader.Read(&sect.sectname, 16);
					reader.Read(&sect.segname, 16);
					sect.addr = reader.Read64();
					sect.size = reader.Read64();
					sect.offset = reader.Read32();
					sect.align = reader.Read32();
					sect.reloff = reader.Read32();
					sect.nreloc = reader.Read32();
					sect.flags = reader.Read32();
					sect.reserved1 = reader.Read32();
					sect.reserved2 = reader.Read32();
					sect.reserved3 = reader.Read32();
					// if the segment isn't mapped into virtual memory don't add the corresponding sections.
					if (segment64.vmsize > 0)
					{
						header.sections.push_back(sect);
					}

					if (!strncmp(sect.sectname, "__mod_init_func", 15))
						header.moduleInitSections.push_back(sect);
					if ((sect.flags & (S_ATTR_SELF_MODIFYING_CODE | S_SYMBOL_STUBS))
						== (S_ATTR_SELF_MODIFYING_CODE | S_SYMBOL_STUBS))
						header.symbolStubSections.push_back(sect);
					if ((sect.flags & S_NON_LAZY_SYMBOL_POINTERS) == S_NON_LAZY_SYMBOL_POINTERS)
						header.symbolPointerSections.push_back(sect);
					if ((sect.flags & S_LAZY_SYMBOL_POINTERS) == S_LAZY_SYMBOL_POINTERS)
						header.symbolPointerSections.push_back(sect);
				}
				header.segments.push_back(segment64);
				break;
			case LC_ROUTINES:  // map the 32bit version to 64bits
				header.routines64.cmd = LC_ROUTINES_64;
				header.routines64.init_address = reader.Read32();
				header.routines64.init_module = reader.Read32();
				header.routines64.reserved1 = reader.Read32();
				header.routines64.reserved2 = reader.Read32();
				header.routines64.reserved3 = reader.Read32();
				header.routines64.reserved4 = reader.Read32();
				header.routines64.reserved5 = reader.Read32();
				header.routines64.reserved6 = reader.Read32();
				header.routinesPresent = true;
				break;
			case LC_ROUTINES_64:
				header.routines64.cmd = LC_ROUTINES_64;
				header.routines64.init_address = reader.Read64();
				header.routines64.init_module = reader.Read64();
				header.routines64.reserved1 = reader.Read64();
				header.routines64.reserved2 = reader.Read64();
				header.routines64.reserved3 = reader.Read64();
				header.routines64.reserved4 = reader.Read64();
				header.routines64.reserved5 = reader.Read64();
				header.routines64.reserved6 = reader.Read64();
				header.routinesPresent = true;
				break;
			case LC_FUNCTION_STARTS:
				header.functionStarts.funcoff = reader.Read32();
				header.functionStarts.funcsize = reader.Read32();
				header.functionStartsPresent = true;
				break;
			case LC_SYMTAB:
				header.symtab.symoff = reader.Read32();
				header.symtab.nsyms = reader.Read32();
				header.symtab.stroff = reader.Read32();
				header.symtab.strsize = reader.Read32();
				break;
			case LC_DYSYMTAB:
				header.dysymtab.ilocalsym = reader.Read32();
				header.dysymtab.nlocalsym = reader.Read32();
				header.dysymtab.iextdefsym = reader.Read32();
				header.dysymtab.nextdefsym = reader.Read32();
				header.dysymtab.iundefsym = reader.Read32();
				header.dysymtab.nundefsym = reader.Read32();
				header.dysymtab.tocoff = reader.Read32();
				header.dysymtab.ntoc = reader.Read32();
				header.dysymtab.modtaboff = reader.Read32();
				header.dysymtab.nmodtab = reader.Read32();
				header.dysymtab.extrefsymoff = reader.Read32();
				header.dysymtab.nextrefsyms = reader.Read32();
				header.dysymtab.indirectsymoff = reader.Read32();
				header.dysymtab.nindirectsyms = reader.Read32();
				header.dysymtab.extreloff = reader.Read32();
				header.dysymtab.nextrel = reader.Read32();
				header.dysymtab.locreloff = reader.Read32();
				header.dysymtab.nlocrel = reader.Read32();
				header.dysymPresent = true;
				break;
			case LC_DYLD_CHAINED_FIXUPS:
				header.chainedFixups.dataoff = reader.Read32();
				header.chainedFixups.datasize = reader.Read32();
				header.chainedFixupsPresent = true;
				break;
			case LC_DYLD_INFO:
			case LC_DYLD_INFO_ONLY:
				header.dyldInfo.rebase_off = reader.Read32();
				header.dyldInfo.rebase_size = reader.Read32();
				header.dyldInfo.bind_off = reader.Read32();
				header.dyldInfo.bind_size = reader.Read32();
				header.dyldInfo.weak_bind_off = reader.Read32();
				header.dyldInfo.weak_bind_size = reader.Read32();
				header.dyldInfo.lazy_bind_off = reader.Read32();
				header.dyldInfo.lazy_bind_size = reader.Read32();
				header.dyldInfo.export_off = reader.Read32();
				header.dyldInfo.export_size = reader.Read32();
				header.exportTrie.dataoff = header.dyldInfo.export_off;
				header.exportTrie.datasize = header.dyldInfo.export_size;
				header.exportTriePresent = true;
				header.dyldInfoPresent = true;
				break;
			case LC_DYLD_EXPORTS_TRIE:
				header.exportTrie.dataoff = reader.Read32();
				header.exportTrie.datasize = reader.Read32();
				header.exportTriePresent = true;
				break;
			case LC_THREAD:
			case LC_UNIXTHREAD:
				/*while (reader.GetOffset() < nextOffset)
				{

					thread_command thread;
					thread.flavor = reader.Read32();
					thread.count = reader.Read32();
					switch (m_archId)
					{
						case MachOx64:
							m_logger->LogDebug("x86_64 Thread state\n");
							if (thread.flavor != X86_THREAD_STATE64)
							{
								reader.SeekRelative(thread.count * sizeof(uint32_t));
								break;
							}
							//This wont be big endian so we can just read the whole thing
							reader.Read(&thread.statex64, sizeof(thread.statex64));
							header.entryPoints.push_back({thread.statex64.rip, false});
							break;
						case MachOx86:
							m_logger->LogDebug("x86 Thread state\n");
							if (thread.flavor != X86_THREAD_STATE32)
							{
								reader.SeekRelative(thread.count * sizeof(uint32_t));
								break;
							}
							//This wont be big endian so we can just read the whole thing
							reader.Read(&thread.statex86, sizeof(thread.statex86));
							header.entryPoints.push_back({thread.statex86.eip, false});
							break;
						case MachOArm:
							m_logger->LogDebug("Arm Thread state\n");
							if (thread.flavor != _ARM_THREAD_STATE)
							{
								reader.SeekRelative(thread.count * sizeof(uint32_t));
								break;
							}
							//This wont be big endian so we can just read the whole thing
							reader.Read(&thread.statearmv7, sizeof(thread.statearmv7));
							header.entryPoints.push_back({thread.statearmv7.r15, false});
							break;
						case MachOAarch64:
						case MachOAarch6432:
							m_logger->LogDebug("Aarch64 Thread state\n");
							if (thread.flavor != _ARM_THREAD_STATE64)
							{
								reader.SeekRelative(thread.count * sizeof(uint32_t));
								break;
							}
							reader.Read(&thread.stateaarch64, sizeof(thread.stateaarch64));
							header.entryPoints.push_back({thread.stateaarch64.pc, false});
							break;
						case MachOPPC:
							m_logger->LogDebug("PPC Thread state\n");
							if (thread.flavor != PPC_THREAD_STATE)
							{
								reader.SeekRelative(thread.count * sizeof(uint32_t));
								break;
							}
							//Read individual entries for endian reasons
							header.entryPoints.push_back({reader.Read32(), false});
							(void)reader.Read32();
							(void)reader.Read32();
							//Read the rest of the structure
							(void)reader.Read(&thread.stateppc.r1, sizeof(thread.stateppc) - (3 * 4));
							break;
						case MachOPPC64:
							m_logger->LogDebug("PPC64 Thread state\n");
							if (thread.flavor != PPC_THREAD_STATE64)
							{
								reader.SeekRelative(thread.count * sizeof(uint32_t));
								break;
							}
							header.entryPoints.push_back({reader.Read64(), false});
							(void)reader.Read64();
							(void)reader.Read64(); // Stack start
							(void)reader.Read(&thread.stateppc64.r1, sizeof(thread.stateppc64) - (3 * 8));
							break;
						default:
							m_logger->LogError("Unknown archid: %x", m_archId);
					}

				}*/
				break;
			case LC_LOAD_DYLIB:
			{
				uint32_t offset = reader.Read32();
				if (offset < nextOffset)
				{
					reader.Seek(curOffset + offset);
					std::string libname = reader.ReadCString(reader.GetOffset());
					header.dylibs.push_back(libname);
				}
			}
			break;
			case LC_BUILD_VERSION:
			{
				// m_logger->LogDebug("LC_BUILD_VERSION:");
				header.buildVersion.platform = reader.Read32();
				header.buildVersion.minos = reader.Read32();
				header.buildVersion.sdk = reader.Read32();
				header.buildVersion.ntools = reader.Read32();
				// m_logger->LogDebug("Platform: %s", BuildPlatformToString(header.buildVersion.platform).c_str());
				// m_logger->LogDebug("MinOS: %s", BuildToolVersionToString(header.buildVersion.minos).c_str());
				// m_logger->LogDebug("SDK: %s", BuildToolVersionToString(header.buildVersion.sdk).c_str());
				for (uint32_t j = 0; (i < header.buildVersion.ntools) && (j < 10); j++)
				{
					uint32_t tool = reader.Read32();
					uint32_t version = reader.Read32();
					header.buildToolVersions.push_back({tool, version});
					// m_logger->LogDebug("Build Tool: %s: %s", BuildToolToString(tool).c_str(),
					// BuildToolVersionToString(version).c_str());
				}
				break;
			}
			case LC_FILESET_ENTRY:
			{
				throw ReadException();
			}
			default:
				// m_logger->LogDebug("Unhandled command: %s : %" PRIu32 "\n", CommandToString(load.cmd).c_str(),
				// load.cmdsize);
				break;
			}
			if (reader.GetOffset() != nextOffset)
			{
				// m_logger->LogDebug("Didn't parse load command: %s fully %" PRIx64 ":%" PRIxPTR,
				// CommandToString(load.cmd).c_str(), reader.GetOffset(), nextOffset);
			}
			reader.Seek(nextOffset);
		}

		for (auto& section : header.sections)
		{
			char sectionName[17];
			memcpy(sectionName, section.sectname, sizeof(section.sectname));
			sectionName[16] = 0;
			if (header.identifierPrefix.empty())
				header.sectionNames.push_back(sectionName);
			else
				header.sectionNames.push_back(header.identifierPrefix + "::" + sectionName);
		}
	}
	catch (ReadException&)
	{
		return {};
	}

	return header;
}

void SharedCache::InitializeHeader(
	Ref<BinaryView> view, VM* vm, const SharedCacheMachOHeader& header, std::vector<MemoryRegion*> regionsToLoad)
{
	WillMutateState();

	Ref<Settings> settings = view->GetLoadSettings(VIEW_NAME);
	bool applyFunctionStarts = true;
	if (settings && settings->Contains("loader.dsc.processFunctionStarts"))
		applyFunctionStarts = settings->Get<bool>("loader.dsc.processFunctionStarts", view);

	for (size_t i = 0; i < header.sections.size(); i++)
	{
		bool skip = false;
		for (const auto& region : regionsToLoad)
		{
			if (header.sections[i].addr >= region->start && header.sections[i].addr < region->start + region->size)
			{
				if (region->headerInitialized)
				{
					skip = true;
				}
				break;
			}
		}
		if (!header.sections[i].size || skip)
			continue;

		std::string type;
		BNSectionSemantics semantics = DefaultSectionSemantics;
		switch (header.sections[i].flags & 0xff)
		{
		case S_REGULAR:
			if (header.sections[i].flags & S_ATTR_PURE_INSTRUCTIONS)
			{
				type = "PURE_CODE";
				semantics = ReadOnlyCodeSectionSemantics;
			}
			else if (header.sections[i].flags & S_ATTR_SOME_INSTRUCTIONS)
			{
				type = "CODE";
				semantics = ReadOnlyCodeSectionSemantics;
			}
			else
			{
				type = "REGULAR";
			}
			break;
		case S_ZEROFILL:
			type = "ZEROFILL";
			semantics = ReadWriteDataSectionSemantics;
			break;
		case S_CSTRING_LITERALS:
			type = "CSTRING_LITERALS";
			semantics = ReadOnlyDataSectionSemantics;
			break;
		case S_4BYTE_LITERALS:
			type = "4BYTE_LITERALS";
			break;
		case S_8BYTE_LITERALS:
			type = "8BYTE_LITERALS";
			break;
		case S_LITERAL_POINTERS:
			type = "LITERAL_POINTERS";
			semantics = ReadOnlyDataSectionSemantics;
			break;
		case S_NON_LAZY_SYMBOL_POINTERS:
			type = "NON_LAZY_SYMBOL_POINTERS";
			semantics = ReadOnlyDataSectionSemantics;
			break;
		case S_LAZY_SYMBOL_POINTERS:
			type = "LAZY_SYMBOL_POINTERS";
			semantics = ReadOnlyDataSectionSemantics;
			break;
		case S_SYMBOL_STUBS:
			type = "SYMBOL_STUBS";
			semantics = ReadOnlyCodeSectionSemantics;
			break;
		case S_MOD_INIT_FUNC_POINTERS:
			type = "MOD_INIT_FUNC_POINTERS";
			semantics = ReadOnlyDataSectionSemantics;
			break;
		case S_MOD_TERM_FUNC_POINTERS:
			type = "MOD_TERM_FUNC_POINTERS";
			semantics = ReadOnlyDataSectionSemantics;
			break;
		case S_COALESCED:
			type = "COALESCED";
			break;
		case S_GB_ZEROFILL:
			type = "GB_ZEROFILL";
			semantics = ReadWriteDataSectionSemantics;
			break;
		case S_INTERPOSING:
			type = "INTERPOSING";
			break;
		case S_16BYTE_LITERALS:
			type = "16BYTE_LITERALS";
			break;
		case S_DTRACE_DOF:
			type = "DTRACE_DOF";
			break;
		case S_LAZY_DYLIB_SYMBOL_POINTERS:
			type = "LAZY_DYLIB_SYMBOL_POINTERS";
			semantics = ReadOnlyDataSectionSemantics;
			break;
		case S_THREAD_LOCAL_REGULAR:
			type = "THREAD_LOCAL_REGULAR";
			break;
		case S_THREAD_LOCAL_ZEROFILL:
			type = "THREAD_LOCAL_ZEROFILL";
			break;
		case S_THREAD_LOCAL_VARIABLES:
			type = "THREAD_LOCAL_VARIABLES";
			break;
		case S_THREAD_LOCAL_VARIABLE_POINTERS:
			type = "THREAD_LOCAL_VARIABLE_POINTERS";
			break;
		case S_THREAD_LOCAL_INIT_FUNCTION_POINTERS:
			type = "THREAD_LOCAL_INIT_FUNCTION_POINTERS";
			break;
		default:
			type = "UNKNOWN";
			break;
		}
		if (i >= header.sectionNames.size())
			break;
		if (strncmp(header.sections[i].sectname, "__text", sizeof(header.sections[i].sectname)) == 0)
			semantics = ReadOnlyCodeSectionSemantics;
		if (strncmp(header.sections[i].sectname, "__const", sizeof(header.sections[i].sectname)) == 0)
			semantics = ReadOnlyDataSectionSemantics;
		if (strncmp(header.sections[i].sectname, "__data", sizeof(header.sections[i].sectname)) == 0)
			semantics = ReadWriteDataSectionSemantics;
		if (strncmp(header.sections[i].segname, "__DATA_CONST", sizeof(header.sections[i].segname)) == 0)
			semantics = ReadOnlyDataSectionSemantics;

		view->AddUserSection(header.sectionNames[i], header.sections[i].addr, header.sections[i].size, semantics,
			type, header.sections[i].align);
	}

	auto typeLib = view->GetTypeLibrary(header.installName);

	BinaryReader virtualReader(view);

	bool applyHeaderTypes = false;
	for (const auto& region : regionsToLoad)
	{
		if (header.textBase >= region->start && header.textBase < region->start + region->size)
		{
			if (!region->headerInitialized)
				applyHeaderTypes = true;
			break;
		}
	}
	if (applyHeaderTypes)
	{
		view->DefineDataVariable(header.textBase, Type::NamedType(view, QualifiedName("mach_header_64")));
		view->DefineAutoSymbol(
			new Symbol(DataSymbol, "__macho_header::" + header.identifierPrefix, header.textBase, LocalBinding));

		try
		{
			virtualReader.Seek(header.textBase + sizeof(mach_header_64));
			size_t sectionNum = 0;
			for (size_t i = 0; i < header.ident.ncmds; i++)
			{
				load_command load;
				uint64_t curOffset = virtualReader.GetOffset();
				load.cmd = virtualReader.Read32();
				load.cmdsize = virtualReader.Read32();
				uint64_t nextOffset = curOffset + load.cmdsize;
				switch (load.cmd)
				{
				case LC_SEGMENT:
				{
					view->DefineDataVariable(curOffset, Type::NamedType(view, QualifiedName("segment_command")));
					virtualReader.SeekRelative(5 * 8);
					size_t numSections = virtualReader.Read32();
					virtualReader.SeekRelative(4);
					for (size_t j = 0; j < numSections; j++)
					{
						view->DefineDataVariable(
							virtualReader.GetOffset(), Type::NamedType(view, QualifiedName("section")));
						view->DefineUserSymbol(new Symbol(DataSymbol,
							"__macho_section::" + header.identifierPrefix + "_[" + std::to_string(sectionNum++) + "]",
							virtualReader.GetOffset(), LocalBinding));
						virtualReader.SeekRelative((8 * 8) + 4);
					}
					break;
				}
				case LC_SEGMENT_64:
				{
					view->DefineDataVariable(curOffset, Type::NamedType(view, QualifiedName("segment_command_64")));
					virtualReader.SeekRelative(7 * 8);
					size_t numSections = virtualReader.Read32();
					virtualReader.SeekRelative(4);
					for (size_t j = 0; j < numSections; j++)
					{
						view->DefineDataVariable(
							virtualReader.GetOffset(), Type::NamedType(view, QualifiedName("section_64")));
						view->DefineUserSymbol(new Symbol(DataSymbol,
							"__macho_section_64::" + header.identifierPrefix + "_[" + std::to_string(sectionNum++) + "]",
							virtualReader.GetOffset(), LocalBinding));
						virtualReader.SeekRelative(10 * 8);
					}
					break;
				}
				case LC_SYMTAB:
					view->DefineDataVariable(curOffset, Type::NamedType(view, QualifiedName("symtab")));
					break;
				case LC_DYSYMTAB:
					view->DefineDataVariable(curOffset, Type::NamedType(view, QualifiedName("dysymtab")));
					break;
				case LC_UUID:
					view->DefineDataVariable(curOffset, Type::NamedType(view, QualifiedName("uuid")));
					break;
				case LC_ID_DYLIB:
				case LC_LOAD_DYLIB:
				case LC_REEXPORT_DYLIB:
				case LC_LOAD_WEAK_DYLIB:
				case LC_LOAD_UPWARD_DYLIB:
					view->DefineDataVariable(curOffset, Type::NamedType(view, QualifiedName("dylib_command")));
					if (load.cmdsize - 24 <= 150)
						view->DefineDataVariable(
							curOffset + 24, Type::ArrayType(Type::IntegerType(1, true), load.cmdsize - 24));
					break;
				case LC_CODE_SIGNATURE:
				case LC_SEGMENT_SPLIT_INFO:
				case LC_FUNCTION_STARTS:
				case LC_DATA_IN_CODE:
				case LC_DYLIB_CODE_SIGN_DRS:
				case LC_DYLD_EXPORTS_TRIE:
				case LC_DYLD_CHAINED_FIXUPS:
					view->DefineDataVariable(curOffset, Type::NamedType(view, QualifiedName("linkedit_data")));
					break;
				case LC_ENCRYPTION_INFO:
					view->DefineDataVariable(curOffset, Type::NamedType(view, QualifiedName("encryption_info")));
					break;
				case LC_VERSION_MIN_MACOSX:
				case LC_VERSION_MIN_IPHONEOS:
					view->DefineDataVariable(curOffset, Type::NamedType(view, QualifiedName("version_min")));
					break;
				case LC_DYLD_INFO:
				case LC_DYLD_INFO_ONLY:
					view->DefineDataVariable(curOffset, Type::NamedType(view, QualifiedName("dyld_info")));
					break;
				default:
					view->DefineDataVariable(curOffset, Type::NamedType(view, QualifiedName("load_command")));
					break;
				}

				view->DefineAutoSymbol(new Symbol(DataSymbol,
					"__macho_load_command::" + header.identifierPrefix + "_[" + std::to_string(i) + "]", curOffset,
					LocalBinding));
				virtualReader.Seek(nextOffset);
			}
		}
		catch (ReadException&)
		{
			LogError("Error when applying Mach-O header types at %" PRIx64, header.textBase);
		}
	}

	if (applyFunctionStarts && header.functionStartsPresent && header.linkeditPresent && vm->AddressIsMapped(header.linkeditSegment.vmaddr))
	{
		auto funcStarts =
			vm->MappingAtAddress(header.linkeditSegment.vmaddr)
				.first.fileAccessor->lock()
				->ReadBuffer(header.functionStarts.funcoff, header.functionStarts.funcsize);
		size_t i = 0;
		uint64_t curfunc = header.textBase;
		uint64_t curOffset;

		auto current = static_cast<const uint8_t*>(funcStarts.GetData());
		auto end = current + funcStarts.GetLength();
		while (current != end)
		{
			curOffset = readLEB128(current, end);
			bool addFunction = false;
			for (const auto& region : regionsToLoad)
			{
				if (curfunc >= region->start && curfunc < region->start + region->size)
				{
					if (!region->headerInitialized)
						addFunction = true;
				}
			}
			// LogError("0x%llx, 0x%llx", header.textBase, curOffset);
			if (curOffset == 0 || !addFunction)
				continue;
			curfunc += curOffset;
			uint64_t target = curfunc;
			Ref<Platform> targetPlatform = view->GetDefaultPlatform();
			view->AddFunctionForAnalysis(targetPlatform, target);
		}
	}

	view->BeginBulkModifySymbols();
	if (header.symtab.symoff != 0 && header.linkeditPresent && vm->AddressIsMapped(header.linkeditSegment.vmaddr))
	{
		// Mach-O View symtab processing with
		// a ton of stuff cut out so it can work

		auto reader = vm->MappingAtAddress(header.linkeditSegment.vmaddr).first.fileAccessor->lock();
		// auto symtab = reader->ReadBuffer(header.symtab.symoff, header.symtab.nsyms * sizeof(nlist_64));
		auto strtab = reader->ReadBuffer(header.symtab.stroff, header.symtab.strsize);
		nlist_64 sym;
		memset(&sym, 0, sizeof(sym));
		auto N_TYPE = 0xE;	// idk
		std::vector<std::pair<uint64_t, std::pair<BNSymbolType, std::string>>> symbolInfos;
		for (size_t i = 0; i < header.symtab.nsyms; i++)
		{
			reader->Read(&sym, header.symtab.symoff + i * sizeof(nlist_64), sizeof(nlist_64));
			if (sym.n_strx >= header.symtab.strsize || ((sym.n_type & N_TYPE) == N_INDR))
				continue;

			std::string symbol((char*)strtab.GetDataAt(sym.n_strx));
			// BNLogError("%s: 0x%llx", symbol.c_str(), sym.n_value);
			if (symbol == "<redacted>")
				continue;

			BNSymbolType type = DataSymbol;
			uint32_t flags;
			if ((sym.n_type & N_TYPE) == N_SECT && sym.n_sect > 0 && (size_t)(sym.n_sect - 1) < header.sections.size())
			{}
			else if ((sym.n_type & N_TYPE) == N_ABS)
			{}
			else if ((sym.n_type & 0x1))
			{
				type = ExternalSymbol;
			}
			else
				continue;

			for (auto s : header.sections)
			{
				if (s.addr < sym.n_value)
				{
					if (s.addr + s.size > sym.n_value)
					{
						flags = s.flags;
					}
				}
			}

			if (type != ExternalSymbol)
			{
				if ((flags & S_ATTR_PURE_INSTRUCTIONS) == S_ATTR_PURE_INSTRUCTIONS
					|| (flags & S_ATTR_SOME_INSTRUCTIONS) == S_ATTR_SOME_INSTRUCTIONS)
					type = FunctionSymbol;
				else
					type = DataSymbol;
			}
			if ((sym.n_desc & N_ARM_THUMB_DEF) == N_ARM_THUMB_DEF)
				sym.n_value++;

			auto symbolObj = new Symbol(type, symbol, sym.n_value, GlobalBinding);
			if (type == FunctionSymbol)
			{
				Ref<Platform> targetPlatform = view->GetDefaultPlatform();
				view->AddFunctionForAnalysis(targetPlatform, sym.n_value);
			}
			if (typeLib)
			{
				auto _type = m_dscView->ImportTypeLibraryObject(typeLib, {symbolObj->GetFullName()});
				if (_type)
				{
					view->DefineAutoSymbolAndVariableOrFunction(view->GetDefaultPlatform(), symbolObj, _type);
				}
				else
					view->DefineAutoSymbol(symbolObj);
			}
			else
				view->DefineAutoSymbol(symbolObj);
			symbolInfos.push_back({sym.n_value, {type, symbol}});
		}
		MutableState().symbolInfos[header.textBase] = symbolInfos;
	}

	if (header.exportTriePresent && header.linkeditPresent && vm->AddressIsMapped(header.linkeditSegment.vmaddr))
	{
		auto symbols = SharedCache::ParseExportTrie(vm->MappingAtAddress(header.linkeditSegment.vmaddr).first.fileAccessor->lock(), header);
		std::vector<std::pair<uint64_t, std::pair<BNSymbolType, std::string>>> exportMapping;
		for (const auto& symbol : symbols)
		{
			exportMapping.push_back({symbol->GetAddress(), {symbol->GetType(), symbol->GetRawName()}});
			if (typeLib)
			{
				auto type = m_dscView->ImportTypeLibraryObject(typeLib, {symbol->GetFullName()});

				if (type)
				{
					view->DefineAutoSymbolAndVariableOrFunction(view->GetDefaultPlatform(), symbol, type);
				}
				else
					view->DefineAutoSymbol(symbol);

				if (view->GetAnalysisFunction(view->GetDefaultPlatform(), symbol->GetAddress()))
				{
					auto func = view->GetAnalysisFunction(view->GetDefaultPlatform(), symbol->GetAddress());
					if (symbol->GetFullName() == "_objc_msgSend")
					{
						func->SetHasVariableArguments(false);
					}
					else if (symbol->GetFullName().find("_objc_retain_x") != std::string::npos || symbol->GetFullName().find("_objc_release_x") != std::string::npos)
					{
						auto x = symbol->GetFullName().rfind("x");
						auto num = symbol->GetFullName().substr(x + 1);

						std::vector<BinaryNinja::FunctionParameter> callTypeParams;
						auto cc = m_dscView->GetDefaultArchitecture()->GetCallingConventionByName("apple-arm64-objc-fast-arc-" + num);

						callTypeParams.push_back({"obj", m_dscView->GetTypeByName({ "id" }), true, BinaryNinja::Variable()});

						auto funcType = BinaryNinja::Type::FunctionType(m_dscView->GetTypeByName({ "id" }), cc, callTypeParams);
						func->SetUserType(funcType);
					}
				}
			}
			else
				view->DefineAutoSymbol(symbol);
		}
		MutableState().exportInfos[header.textBase] = std::move(exportMapping);
	}
	view->EndBulkModifySymbols();

	for (auto region : regionsToLoad)
	{
		region->headerInitialized = true;
	}
}


void SharedCache::ReadExportNode(std::vector<Ref<Symbol>>& symbolList, const SharedCacheMachOHeader& header,
	const uint8_t* begin, const uint8_t* end, const uint8_t* current, uint64_t textBase, const std::string& currentText)
{
	if (current >= end)
		throw ReadException();

	uint64_t terminalSize = readValidULEB128(current, end);
	const uint8_t* child = current + terminalSize;
	if (terminalSize != 0) {
		uint64_t imageOffset = 0;
		uint64_t flags = readValidULEB128(current, end);
		if (!(flags & EXPORT_SYMBOL_FLAGS_REEXPORT))
		{
			imageOffset = readValidULEB128(current, end);
			if (!currentText.empty() && textBase + imageOffset)
			{
				uint32_t flags;
				BNSymbolType type;
				for (auto s : header.sections)
				{
					if (s.addr < textBase + imageOffset)
					{
						if (s.addr + s.size > textBase + imageOffset)
						{
							flags = s.flags;
							break;
						}
					}
				}
				if ((flags & S_ATTR_PURE_INSTRUCTIONS) == S_ATTR_PURE_INSTRUCTIONS
					|| (flags & S_ATTR_SOME_INSTRUCTIONS) == S_ATTR_SOME_INSTRUCTIONS)
					type = FunctionSymbol;
				else
					type = DataSymbol;

#if EXPORT_TRIE_DEBUG
					// BNLogInfo("export: %s -> 0x%llx", n.text.c_str(), image.baseAddress + n.offset);
#endif
				// TODO: The usual `Symbol` constructors take a `NameSpace` and do unnecessary memory allocations
				// to pass its fields down to the core API. Here we pass nullptr for the namespace which is treated
				// the same, but avoids the memory allocations. Switch back to directly constructing a `Symbol`
				// once it gains constructors without that overhead.
				auto symbol = new Symbol(BNCreateSymbol(type, currentText.c_str(), currentText.c_str(),
					currentText.c_str(), textBase + imageOffset, NoBinding, nullptr, 0));
				symbolList.push_back(symbol);
			}
		}
	}
	current = child;
	uint8_t childCount = *current++;
	std::string childText = currentText;
	for (uint8_t i = 0; i < childCount; ++i)
	{
		if (current >= end)
			throw ReadException();
		auto it = std::find(current, end, 0);
		childText.append(current, it);
		current = it + 1;
		if (current >= end)
			throw ReadException();
		auto next = readValidULEB128(current, end);
		if (next == 0)
			throw ReadException();
		ReadExportNode(symbolList, header, begin, end, begin + next, textBase, childText);
		childText.resize(currentText.size());
	}
}


std::vector<Ref<Symbol>> SharedCache::ParseExportTrie(std::shared_ptr<MMappedFileAccessor> linkeditFile, const SharedCacheMachOHeader& header)
{
	if (!header.exportTrie.datasize) {
		return {};
	}

	try
	{
		std::vector<Ref<Symbol>> symbols;
		auto [begin, end] = linkeditFile->ReadSpan(header.exportTrie.dataoff, header.exportTrie.datasize);
		ReadExportNode(symbols, header, begin, end, begin, header.textBase, "");
		return symbols;
	}
	catch (std::exception& e)
	{
		BNLogError("Failed to load Export Trie");
		return {};
	}
}

std::vector<std::string> SharedCache::GetAvailableImages()
{
	std::vector<std::string> installNames;
	for (const auto& header : State().headers)
	{
		installNames.push_back(header.second.installName);
	}
	return installNames;
}


std::vector<std::pair<std::string, Ref<Symbol>>> SharedCache::LoadAllSymbolsAndWait()
{
	WillMutateState();

	std::lock_guard initialLoadBlock(m_viewSpecificState->viewOperationsThatInfluenceMetadataMutex);

	std::vector<std::pair<std::string, Ref<Symbol>>> symbols;
	for (const auto& img : State().images)
	{
		auto header = HeaderForAddress(img.headerLocation);
		std::shared_ptr<MMappedFileAccessor> mapping;
		try {
			mapping = MMappedFileAccessor::Open(m_dscView, m_dscView->GetFile()->GetSessionId(), header->exportTriePath)->lock();
		}
		catch (...)
		{
			m_logger->LogWarn("Serious Error: Failed to open export trie %s for %s", header->exportTriePath.c_str(), header->installName.c_str());
			continue;
		}
		auto exportList = SharedCache::ParseExportTrie(mapping, *header);
		std::vector<std::pair<uint64_t, std::pair<BNSymbolType, std::string>>> exportMapping;
		for (const auto& sym : exportList)
		{
			exportMapping.push_back({sym->GetAddress(), {sym->GetType(), sym->GetRawName()}});
			symbols.push_back({img.installName, sym});
		}
		MutableState().exportInfos[header->textBase] = std::move(exportMapping);
	}

	SaveToDSCView();

	return symbols;
}


std::string SharedCache::SerializedImageHeaderForAddress(uint64_t address)
{
	auto header = HeaderForAddress(address);
	if (header)
	{
		return header->AsString();
	}
	return "";
}


std::string SharedCache::SerializedImageHeaderForName(std::string name)
{
	if (auto it = State().imageStarts.find(name); it != State().imageStarts.end())
	{
		if (auto header = HeaderForAddress(it->second))
		{
			return header->AsString();
		}
	}
	return "";
}

Ref<TypeLibrary> SharedCache::TypeLibraryForImage(const std::string& installName) {
	std::lock_guard lock(m_viewSpecificState->typeLibraryMutex);
	if (auto it = m_viewSpecificState->typeLibraries.find(installName); it != m_viewSpecificState->typeLibraries.end()) {
		return it->second;
	}

	auto typeLib = m_dscView->GetTypeLibrary(installName);
	if (!typeLib) {
		auto typeLibs = m_dscView->GetDefaultPlatform()->GetTypeLibrariesByName(installName);
		if (!typeLibs.empty()) {
			typeLib = typeLibs[0];
			m_dscView->AddTypeLibrary(typeLib);
		}
	}

	m_viewSpecificState->typeLibraries[installName] = typeLib;
	return typeLib;
}

void SharedCache::FindSymbolAtAddrAndApplyToAddr(
	uint64_t symbolLocation, uint64_t targetLocation, bool triggerReanalysis)
{
	WillMutateState();

	std::string prefix = "";
	if (symbolLocation != targetLocation)
		prefix = "j_";
	if (auto preexistingSymbol = m_dscView->GetSymbolByAddress(targetLocation))
	{
		if (preexistingSymbol->GetFullName().find("j_") != std::string::npos)
			return;
	}
	auto id = m_dscView->BeginUndoActions();
	if (auto loadedSymbol = m_dscView->GetSymbolByAddress(symbolLocation))
	{
		if (m_dscView->GetAnalysisFunction(m_dscView->GetDefaultPlatform(), targetLocation))
			m_dscView->DefineUserSymbol(new Symbol(FunctionSymbol, prefix + loadedSymbol->GetFullName(), targetLocation));
		else
			m_dscView->DefineUserSymbol(new Symbol(loadedSymbol->GetType(), prefix + loadedSymbol->GetFullName(), targetLocation));
	}
	else if (auto sym = m_dscView->GetSymbolByAddress(symbolLocation))
	{
		if (m_dscView->GetAnalysisFunction(m_dscView->GetDefaultPlatform(), targetLocation))
			m_dscView->DefineUserSymbol(new Symbol(FunctionSymbol, prefix + sym->GetFullName(), targetLocation));
		else
			m_dscView->DefineUserSymbol(new Symbol(sym->GetType(), prefix + sym->GetFullName(), targetLocation));
	}
	m_dscView->ForgetUndoActions(id);
	auto header = HeaderForAddress(symbolLocation);
	if (header)
	{
		std::shared_ptr<MMappedFileAccessor> mapping;
		try {
			mapping = MMappedFileAccessor::Open(m_dscView, m_dscView->GetFile()->GetSessionId(), header->exportTriePath)->lock();
		}
		catch (...)
		{
			m_logger->LogWarn("Serious Error: Failed to open export trie for %s", header->installName.c_str());
			return;
		}
		auto exportList = SharedCache::ParseExportTrie(mapping, *header);
		std::vector<std::pair<uint64_t, std::pair<BNSymbolType, std::string>>> exportMapping;
		auto typeLib = TypeLibraryForImage(header->installName);
		id = m_dscView->BeginUndoActions();
		m_dscView->BeginBulkModifySymbols();
		for (const auto& sym : exportList)
		{
			exportMapping.push_back({sym->GetAddress(), {sym->GetType(), sym->GetRawName()}});
			if (sym->GetAddress() == symbolLocation)
			{
				if (auto func = m_dscView->GetAnalysisFunction(m_dscView->GetDefaultPlatform(), targetLocation))
				{
					m_dscView->DefineUserSymbol(
						new Symbol(FunctionSymbol, prefix + sym->GetFullName(), targetLocation));

					if (typeLib)
						if (auto type = m_dscView->ImportTypeLibraryObject(typeLib, {sym->GetFullName()}))
							func->SetUserType(type);
				}
				else
				{
					m_dscView->DefineUserSymbol(
						new Symbol(sym->GetType(), prefix + sym->GetFullName(), targetLocation));

					if (typeLib)
						if (auto type = m_dscView->ImportTypeLibraryObject(typeLib, {sym->GetFullName()}))
							m_dscView->DefineUserDataVariable(targetLocation, type);
				}
				if (triggerReanalysis)
				{
					auto func = m_dscView->GetAnalysisFunction(m_dscView->GetDefaultPlatform(), targetLocation);
					if (func)
						func->Reanalyze();
				}
				break;
			}
		}
		{
			std::lock_guard lock(m_viewSpecificState->viewOperationsThatInfluenceMetadataMutex);
			MutableState().exportInfos[header->textBase] = std::move(exportMapping);
		}
		m_dscView->EndBulkModifySymbols();
		m_dscView->ForgetUndoActions(id);
	}
}


bool SharedCache::SaveToDSCView()
{
	if (m_dscView)
	{
		auto data = AsMetadata();
		m_dscView->StoreMetadata(SharedCacheMetadataTag, data);
		m_dscView->GetParentView()->StoreMetadata(SharedCacheMetadataTag, data);

		// By moving our state the to cache we can avoid creating a copy in the case
		// that no further mutations are made to `this`. If we're not done being mutated,
		// the data will be copied on the first mutation.
		auto cachedState = std::make_shared<struct State>(std::move(*m_state));
		m_state = cachedState;
		m_stateIsShared = true;

		std::lock_guard lock(m_viewSpecificState->stateMutex);
		m_viewSpecificState->cachedState = std::move(cachedState);

		m_metadataValid = true;

		return true;
	}
	return false;
}
std::vector<MemoryRegion> SharedCache::GetMappedRegions() const
{
	std::lock_guard lock(m_viewSpecificState->viewOperationsThatInfluenceMetadataMutex);
	return State().regionsMappedIntoMemory;
}

bool SharedCache::IsMemoryMapped(uint64_t address)
{
	return m_dscView->IsValidOffset(address);
}

extern "C"
{
	BNSharedCache* BNGetSharedCache(BNBinaryView* data)
	{
		if (!data)
			return nullptr;

		Ref<BinaryView> view = new BinaryView(BNNewViewReference(data));
		if (auto cache = SharedCache::GetFromDSCView(view))
		{
			cache->AddAPIRef();
			return cache->GetAPIObject();
		}

		return nullptr;
	}

	BNSharedCache* BNNewSharedCacheReference(BNSharedCache* cache)
	{
		if (!cache->object)
			return nullptr;

		cache->object->AddAPIRef();
		return cache;
	}

	void BNFreeSharedCacheReference(BNSharedCache* cache)
	{
		if (!cache->object)
			return;

		cache->object->ReleaseAPIRef();
	}

	bool BNDSCViewLoadImageWithInstallName(BNSharedCache* cache, char* name, bool skipObjC)
	{
		std::string imageName = std::string(name);
		// FIXME !!!!!!!! BNFreeString(name);

		if (cache->object)
			return cache->object->LoadImageWithInstallName(imageName, skipObjC);

		return false;
	}

	bool BNDSCViewLoadSectionAtAddress(BNSharedCache* cache, uint64_t addr)
	{
		if (cache->object)
		{
			return cache->object->LoadSectionAtAddress(addr);
		}

		return false;
	}

	bool BNDSCViewLoadImageContainingAddress(BNSharedCache* cache, uint64_t address, bool skipObjC)
	{
		if (cache->object)
		{
			return cache->object->LoadImageContainingAddress(address, skipObjC);
		}

		return false;
	}

	void BNDSCViewProcessObjCSectionsForImageWithInstallName(BNSharedCache* cache, char* name, bool deallocName)
	{
		std::string imageName = std::string(name);
		if (deallocName)
			BNFreeString(name);

		if (cache->object)
			cache->object->ProcessObjCSectionsForImageWithInstallName(imageName);
	}

	void BNDSCViewProcessAllObjCSections(BNSharedCache* cache)
	{
		if (cache->object)
			cache->object->ProcessAllObjCSections();
	}

	char** BNDSCViewGetInstallNames(BNSharedCache* cache, size_t* count)
	{
		if (cache->object)
		{
			auto value = cache->object->GetAvailableImages();
			*count = value.size();

			std::vector<const char*> cstrings;
			for (size_t i = 0; i < value.size(); i++)
			{
				cstrings.push_back(value[i].c_str());
			}
			return BNAllocStringList(cstrings.data(), cstrings.size());
		}
		*count = 0;
		return nullptr;
	}

	BNDSCSymbolRep* BNDSCViewLoadAllSymbolsAndWait(BNSharedCache* cache, size_t* count)
	{
		if (cache->object)
		{
			auto value = cache->object->LoadAllSymbolsAndWait();
			*count = value.size();

			BNDSCSymbolRep* symbols = (BNDSCSymbolRep*)malloc(sizeof(BNDSCSymbolRep) * value.size());
			for (size_t i = 0; i < value.size(); i++)
			{
				symbols[i].address = value[i].second->GetAddress();
				symbols[i].name = BNAllocString(value[i].second->GetRawName().c_str());
				symbols[i].image = BNAllocString(value[i].first.c_str());
			}
			return symbols;
		}
		*count = 0;
		return nullptr;
	}

	void BNDSCViewFreeSymbols(BNDSCSymbolRep* symbols, size_t count)
	{
		for (size_t i = 0; i < count; i++)
		{
			BNFreeString(symbols[i].name);
			BNFreeString(symbols[i].image);
		}
		delete symbols;
	}

	char* BNDSCViewGetNameForAddress(BNSharedCache* cache, uint64_t address)
	{
		if (cache->object)
		{
			return BNAllocString(cache->object->NameForAddress(address).c_str());
		}

		return nullptr;
	}

	char* BNDSCViewGetImageNameForAddress(BNSharedCache* cache, uint64_t address)
	{
		if (cache->object)
		{
			return BNAllocString(cache->object->ImageNameForAddress(address).c_str());
		}

		return nullptr;
	}

	uint64_t BNDSCViewLoadedImageCount(BNSharedCache* cache)
	{
		// FIXME?
		return 0;
	}

	BNDSCViewState BNDSCViewGetState(BNSharedCache* cache)
	{
		if (cache->object)
		{
			return (BNDSCViewState)cache->object->ViewState();
		}

		return BNDSCViewState::Unloaded;
	}


	BNDSCMappedMemoryRegion* BNDSCViewGetLoadedRegions(BNSharedCache* cache, size_t* count)
	{
		if (cache->object)
		{
			auto regions = cache->object->GetMappedRegions();
			*count = regions.size();
			BNDSCMappedMemoryRegion* mappedRegions = (BNDSCMappedMemoryRegion*)malloc(sizeof(BNDSCMappedMemoryRegion) * regions.size());
			for (size_t i = 0; i < regions.size(); i++)
			{
				mappedRegions[i].vmAddress = regions[i].start;
				mappedRegions[i].size = regions[i].size;
				mappedRegions[i].name = BNAllocString(regions[i].prettyName.c_str());
			}
			return mappedRegions;
		}
		*count = 0;
		return nullptr;
	}

	void BNDSCViewFreeLoadedRegions(BNDSCMappedMemoryRegion* images, size_t count)
	{
		for (size_t i = 0; i < count; i++)
		{
			BNFreeString(images[i].name);
		}
		delete images;
	}


	BNDSCBackingCache* BNDSCViewGetBackingCaches(BNSharedCache* cache, size_t* count)
	{
		BNDSCBackingCache* caches = nullptr;

		if (cache->object)
		{
			auto viewCaches = cache->object->BackingCaches();
			*count = viewCaches.size();
			caches = (BNDSCBackingCache*)malloc(sizeof(BNDSCBackingCache) * viewCaches.size());
			for (size_t i = 0; i < viewCaches.size(); i++)
			{
				caches[i].path = BNAllocString(viewCaches[i].path.c_str());
				caches[i].isPrimary = viewCaches[i].isPrimary;

				BNDSCBackingCacheMapping* mappings;
				mappings = (BNDSCBackingCacheMapping*)malloc(sizeof(BNDSCBackingCacheMapping) * viewCaches[i].mappings.size());

				size_t j = 0;
				for (const auto& mapping : viewCaches[i].mappings)
				{
					mappings[j].vmAddress = mapping.address;
					mappings[j].size = mapping.size;
					mappings[j].fileOffset = mapping.fileOffset;
					j++;
				}
				caches[i].mappings = mappings;
				caches[i].mappingCount = viewCaches[i].mappings.size();
			}
		}

		return caches;
	}

	void BNDSCViewFreeBackingCaches(BNDSCBackingCache* caches, size_t count)
	{
		for (size_t i = 0; i < count; i++)
		{
			delete[] caches[i].mappings;
			BNFreeString(caches[i].path);
		}
		delete[] caches;
	}

	void BNDSCFindSymbolAtAddressAndApplyToAddress(BNSharedCache* cache, uint64_t symbolLocation, uint64_t targetLocation, bool triggerReanalysis)
	{
		if (cache->object)
		{
			cache->object->FindSymbolAtAddrAndApplyToAddr(symbolLocation, targetLocation, triggerReanalysis);
		}
	}

	BNDSCImage* BNDSCViewGetAllImages(BNSharedCache* cache, size_t* count)
	{
		if (cache->object)
		{
			try {
				auto vm = cache->object->GetVMMap(true);
				auto viewImageHeaders = cache->object->AllImageHeaders();
				*count = viewImageHeaders.size();
				BNDSCImage* images = (BNDSCImage*)malloc(sizeof(BNDSCImage) * viewImageHeaders.size());
				size_t i = 0;
				for (const auto& [baseAddress, header] : viewImageHeaders)
				{
					images[i].name = BNAllocString(header.installName.c_str());
					images[i].headerAddress = baseAddress;
					images[i].mappingCount = header.sections.size();
					images[i].mappings = (BNDSCImageMemoryMapping*)malloc(sizeof(BNDSCImageMemoryMapping) * header.sections.size());
					for (size_t j = 0; j < header.sections.size(); j++)
					{
						const auto sectionStart = header.sections[j].addr;
						images[i].mappings[j].rawViewOffset = header.sections[j].offset;
						images[i].mappings[j].vmAddress = sectionStart;
						images[i].mappings[j].size = header.sections[j].size;
						images[i].mappings[j].name = BNAllocString(header.sectionNames[j].c_str());
						auto fileAccessor = vm->MappingAtAddress(sectionStart).first.fileAccessor;
						images[i].mappings[j].filePath = BNAllocStringWithLength(fileAccessor->filePath().data(), fileAccessor->filePath().length());
						images[i].mappings[j].loaded = cache->object->IsMemoryMapped(sectionStart);
					}
					i++;
				}
				return images;
			}
			catch (...)
			{
				LogError("SharedCache: Failed to load image listing. Likely caused by a ser/deserialization error or load failure");
				*count = 0;
				return nullptr;
			}
		}
		*count = 0;
		return nullptr;
	}

	void BNDSCViewFreeAllImages(BNDSCImage* images, size_t count)
	{
		for (size_t i = 0; i < count; i++)
		{
			for (size_t j = 0; j < images[i].mappingCount; j++)
			{
				BNFreeString(images[i].mappings[j].name);
				BNFreeString(images[i].mappings[j].filePath);
			}
			delete[] images[i].mappings;
			BNFreeString(images[i].name);
		}
		delete[] images;
	}

	char* BNDSCViewGetImageHeaderForAddress(BNSharedCache* cache, uint64_t address)
	{
		if (cache->object)
		{
			auto header = cache->object->SerializedImageHeaderForAddress(address);
			return BNAllocString(header.c_str());
		}

		return nullptr;
	}

	char* BNDSCViewGetImageHeaderForName(BNSharedCache* cache, char* name)
	{
		std::string imageName = std::string(name);
		BNFreeString(name);
		if (cache->object)
		{
			auto header = cache->object->SerializedImageHeaderForName(imageName);
			return BNAllocString(header.c_str());
		}

		return nullptr;
	}

	BNDSCMemoryUsageInfo BNDSCViewGetMemoryUsageInfo()
	{
		BNDSCMemoryUsageInfo info;
		info.mmapRefs = mmapCount.load();
		info.sharedCacheRefs = sharedCacheReferences.load();
		return info;
	}

	BNDSCViewLoadProgress BNDSCViewGetLoadProgress(uint64_t sessionID)
	{
		if (auto viewSpecificState = ViewSpecificStateForId(sessionID, false)) {
			return viewSpecificState->progress;
		}

		return LoadProgressNotStarted;
	}

	uint64_t BNDSCViewFastGetBackingCacheCount(BNBinaryView* data)
	{
		Ref<BinaryView> view = new BinaryView(BNNewViewReference(data));
		return SharedCache::FastGetBackingCacheCount(view);
	}
}

[[maybe_unused]] DSCViewType* g_dscViewType;

void InitDSCViewType()
{
	MMappedFileAccessor::InitialVMSetup();
	std::atexit(VMShutdown);

	static DSCViewType type;
	BinaryViewType::Register(&type);
	g_dscViewType = &type;
}

namespace SharedCacheCore {

void Serialize(SerializationContext& context, const dyld_cache_mapping_info& value)
{
       context.writer.StartArray();
       Serialize(context, value.address);
       Serialize(context, value.size);
       Serialize(context, value.fileOffset);
       Serialize(context, value.maxProt);
       Serialize(context, value.initProt);
       context.writer.EndArray();
}

void Deserialize(DeserializationContext& context, std::string_view name, std::vector<dyld_cache_mapping_info>& b)
{

       auto bArr = context.doc[name.data()].GetArray();
       for (auto& s : bArr)
       {
               dyld_cache_mapping_info mapping;
               auto s2 = s.GetArray();
               mapping.address = s2[0].GetUint64();
               mapping.size = s2[1].GetUint64();
               mapping.fileOffset = s2[2].GetUint64();
               mapping.maxProt = s2[3].GetUint();
               mapping.initProt = s2[4].GetUint();
               b.push_back(mapping);
       }
}

void SharedCache::Store(SerializationContext& context) const
{
	Serialize(context, "metadataVersion", METADATA_VERSION);

    Serialize(context, "m_viewState", State().viewState);
    Serialize(context, "m_cacheFormat", State().cacheFormat);
    Serialize(context, "m_imageStarts", State().imageStarts);
    Serialize(context, "m_baseFilePath", State().baseFilePath);

	Serialize(context, "headers");
	context.writer.StartArray();
	for (auto& [k, v] : State().headers)
	{
		context.writer.StartObject();
		v.Store(context);
		context.writer.EndObject();
	}
	context.writer.EndArray();

	Serialize(context, "exportInfos");
	context.writer.StartArray();
	for (const auto& pair1 : State().exportInfos)
	{
		context.writer.StartObject();
		Serialize(context, "key", pair1.first);
		Serialize(context, "value");
		context.writer.StartArray();
		for (const auto& pair2 : pair1.second)
		{
			context.writer.StartObject();
			Serialize(context, "key", pair2.first);
			Serialize(context, "val1", pair2.second.first);
			Serialize(context, "val2", pair2.second.second);
			context.writer.EndObject();
		}
		context.writer.EndArray();
		context.writer.EndObject();
	}
	context.writer.EndArray();

	Serialize(context, "symbolInfos");
	context.writer.StartArray();
	for (const auto& pair1 : State().symbolInfos)
	{
		context.writer.StartObject();
		Serialize(context, "key", pair1.first);
		Serialize(context, "value");
		context.writer.StartArray();
		for (const auto& pair2 : pair1.second)
		{
			context.writer.StartObject();
			Serialize(context, "key", pair2.first);
			Serialize(context, "val1", pair2.second.first);
			Serialize(context, "val2", pair2.second.second);
			context.writer.EndObject();
		}
		context.writer.EndArray();
		context.writer.EndObject();
	}
	context.writer.EndArray();

	Serialize(context, "backingCaches", State().backingCaches);
	Serialize(context, "stubIslands", State().stubIslandRegions);
	Serialize(context, "images", State().images);
	Serialize(context, "regionsMappedIntoMemory", State().regionsMappedIntoMemory);
	Serialize(context, "dyldDataSections", State().dyldDataRegions);
	Serialize(context, "nonImageRegions", State().nonImageRegions);
}

void SharedCache::Load(DeserializationContext& context)
{
	if (context.doc.HasMember("metadataVersion"))
	{
		if (context.doc["metadataVersion"].GetUint() != METADATA_VERSION)
		{
			m_logger->LogError("Shared Cache metadata version mismatch");
			return;
		}
	}
	else
	{
		m_logger->LogError("Shared Cache metadata version missing");
		return;
	}

	m_stateIsShared = false;
	m_state = std::make_shared<struct SharedCache::State>();

	MutableState().viewState = static_cast<DSCViewState>(context.load<uint8_t>("m_viewState"));
	MutableState().cacheFormat = static_cast<SharedCacheFormat>(context.load<uint8_t>("m_cacheFormat"));

	for (auto& startAndHeader : context.doc["headers"].GetArray())
	{
		SharedCacheMachOHeader header;
		header.LoadFromValue(startAndHeader);
		MutableState().headers[header.textBase] = std::move(header);
	}

	Deserialize(context, "m_imageStarts", MutableState().imageStarts);
	Deserialize(context, "m_baseFilePath", MutableState().baseFilePath);

	for (const auto& obj1 : context.doc["exportInfos"].GetArray())
	{
		std::vector<std::pair<uint64_t, std::pair<BNSymbolType, std::string>>> innerVec;
		for (const auto& obj2 : obj1["value"].GetArray())
		{
			std::pair<BNSymbolType, std::string> innerPair = {
				(BNSymbolType)obj2["val1"].GetUint64(), obj2["val2"].GetString()};
			innerVec.push_back({obj2["key"].GetUint64(), innerPair});
		}

		MutableState().exportInfos[obj1["key"].GetUint64()] = std::move(innerVec);
	}

	for (auto& symbolInfo : context.doc["symbolInfos"].GetArray())
	{
		std::vector<std::pair<uint64_t, std::pair<BNSymbolType, std::string>>>
			symbolInfos;
		for (auto& si : symbolInfo["value"].GetArray())
		{
			symbolInfos.push_back({si["key"].GetUint64(),
				{static_cast<BNSymbolType>(si["val1"].GetUint64()), si["val2"].GetString()}});
		}
		MutableState().symbolInfos[symbolInfo["key"].GetUint64()] = std::move(symbolInfos);
	}

	for (auto& bcV : context.doc["backingCaches"].GetArray())
	{
		BackingCache bc;
		bc.LoadFromValue(bcV);
		MutableState().backingCaches.push_back(std::move(bc));
	}

	for (auto& imgV : context.doc["images"].GetArray())
	{
		CacheImage img;
		img.LoadFromValue(imgV);
		MutableState().images.push_back(std::move(img));
	}

	for (auto& rV : context.doc["regionsMappedIntoMemory"].GetArray())
	{
		MemoryRegion r;
		r.LoadFromValue(rV);
		MutableState().regionsMappedIntoMemory.push_back(std::move(r));
	}

	for (auto& siV : context.doc["stubIslands"].GetArray())
	{
		MemoryRegion si;
		si.LoadFromValue(siV);
		MutableState().stubIslandRegions.push_back(std::move(si));
	}

	for (auto& siV : context.doc["dyldDataSections"].GetArray())
	{
		MemoryRegion si;
		si.LoadFromValue(siV);
		MutableState().dyldDataRegions.push_back(std::move(si));
	}

	for (auto& siV : context.doc["nonImageRegions"].GetArray())
	{
		MemoryRegion si;
		si.LoadFromValue(siV);
		MutableState().nonImageRegions.push_back(std::move(si));
	}

	m_metadataValid = true;
}

void BackingCache::Store(SerializationContext& context) const
{
	MSS(path);
	MSS(isPrimary);
	MSS(mappings);
}
void BackingCache::Load(DeserializationContext& context)
{
	MSL(path);
	MSL(isPrimary);
	MSL(mappings);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline)) void SharedCache::AssertMutable() const
#elif defined(_MSC_VER)
__forceinline void SharedCache::AssertMutable() const
#else
#error "Unsupported compiler"
#endif

{
	if (m_stateIsShared)
	{
		abort();
	}
}

void SharedCache::WillMutateState()
{
	if (!m_state)
	{
		m_state = std::make_shared<struct State>();
	}
	else if (m_stateIsShared)
	{
		m_state = std::make_shared<struct State>(*m_state);
	}
	m_stateIsShared = false;
}


const std::vector<BackingCache>& SharedCache::BackingCaches() const
{
	return State().backingCaches;
}

DSCViewState SharedCache::ViewState() const
{
	return State().viewState;
}

const std::unordered_map<std::string, uint64_t>& SharedCache::AllImageStarts() const
{
	return State().imageStarts;
}

const std::unordered_map<uint64_t, SharedCacheMachOHeader>& SharedCache::AllImageHeaders() const
{
	return State().headers;
}
size_t SharedCache::GetBaseAddress() const {
	if (State().backingCaches.empty()) {
		return 0;
	}

	const BackingCache& primaryCache = State().backingCaches[0];
	if (!primaryCache.isPrimary) {
		abort();
		return 0;
	}

	if (primaryCache.mappings.empty()) {
		return 0;
	}

	return primaryCache.mappings[0].address;
}

// Intentionally takes a copy to avoid modifying the cursor position in the original reader.
std::optional<ObjCOptimizationHeader> SharedCache::GetObjCOptimizationHeader(VMReader reader) const {
	if (!State().objcOptimizationDataRange) {
		return {};
	}

	ObjCOptimizationHeader header{};
	// Ignoring `objcOptsSize` in favor of `sizeof(ObjCOptimizationHeader)` matches dyld's behavior.
	reader.Read(&header, GetBaseAddress() + State().objcOptimizationDataRange->first, sizeof(ObjCOptimizationHeader));

	return header;
}

size_t SharedCache::GetObjCRelativeMethodBaseAddress(const VMReader& reader) const {
	if (auto header = GetObjCOptimizationHeader(reader); header.has_value()) {
		return GetBaseAddress() + header->relativeMethodSelectorBaseAddressOffset;
	}
	return 0;
}

}  // namespace SharedCacheCore
