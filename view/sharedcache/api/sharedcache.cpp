//
// Created by kat on 5/21/23.
//

#include "sharedcacheapi.h"

namespace SharedCacheAPI {

	SharedCache::SharedCache(Ref<BinaryView> view) {
		m_object = BNGetSharedCache(view->GetObject());
	}

	BNDSCViewLoadProgress SharedCache::GetLoadProgress(Ref<BinaryView> view)
	{
		return BNDSCViewGetLoadProgress(view->GetFile()->GetSessionId());
	}

	uint64_t SharedCache::FastGetBackingCacheCount(Ref<BinaryView> view)
	{
		return BNDSCViewFastGetBackingCacheCount(view->GetObject());
	}

	bool SharedCache::LoadImageWithInstallName(std::string installName)
	{
		char* str = BNAllocString(installName.c_str());
		return BNDSCViewLoadImageWithInstallName(m_object, str);
	}

	bool SharedCache::LoadSectionAtAddress(uint64_t addr)
	{
		return BNDSCViewLoadSectionAtAddress(m_object, addr);
	}

	bool SharedCache::LoadImageContainingAddress(uint64_t addr)
	{
		return BNDSCViewLoadImageContainingAddress(m_object, addr);
	}

	std::vector<std::string> SharedCache::GetAvailableImages()
	{
		size_t count;
		char** value = BNDSCViewGetInstallNames(m_object, &count);
		if (value == nullptr)
		{
			return {};
		}

		std::vector<std::string> result;
		for (size_t i = 0; i < count; i++)
		{
			result.push_back(value[i]);
		}

		BNFreeStringList(value, count);
		return result;
	}

	std::vector<DSCMemoryRegion> SharedCache::GetLoadedMemoryRegions()
	{
		size_t count;
		BNDSCMappedMemoryRegion* value = BNDSCViewGetLoadedRegions(m_object, &count);
		if (value == nullptr)
		{
			return {};
		}

		std::vector<DSCMemoryRegion> result;
		for (size_t i = 0; i < count; i++)
		{
			DSCMemoryRegion region;
			region.vmAddress = value[i].vmAddress;
			region.size = value[i].size;
			region.prettyName = value[i].name;
			result.push_back(region);
		}

		BNDSCViewFreeLoadedRegions(value, count);
		return result;
	}
	std::vector<BackingCache> SharedCache::GetBackingCaches()
	{
		size_t count;
		BNDSCBackingCache* value = BNDSCViewGetBackingCaches(m_object, &count);
		if (value == nullptr)
		{
			return {};
		}

		std::vector<BackingCache> result;
		for (size_t i = 0; i < count; i++)
		{
			BackingCache cache;
			cache.path = value[i].path;
			cache.isPrimary = value[i].isPrimary;
			for (size_t j = 0; j < value[i].mappingCount; j++)
			{
				BackingCacheMapping mapping;
				mapping.vmAddress = value[i].mappings[j].vmAddress;
				mapping.size = value[i].mappings[j].size;
				mapping.fileOffset = value[i].mappings[j].fileOffset;
				cache.mappings.push_back(mapping);
			}
			result.push_back(cache);
		}

		BNDSCViewFreeBackingCaches(value, count);
		return result;
	}

	std::vector<DSCImage> SharedCache::GetImages()
	{
		size_t count;
		BNDSCImage* value = BNDSCViewGetAllImages(m_object, &count);
		if (value == nullptr)
		{
			return {};
		}

		std::vector<DSCImage> result;
		for (size_t i = 0; i < count; i++)
		{
			DSCImage img;
			img.name = value[i].name;
			img.headerAddress = value[i].headerAddress;
			for (size_t j = 0; j < value[i].mappingCount; j++)
			{
				DSCImageMemoryMapping mapping;
				mapping.filePath = value[i].mappings[j].filePath;
				mapping.name = value[i].mappings[j].name;
				mapping.vmAddress = value[i].mappings[j].vmAddress;
				mapping.rawViewOffset = value[i].mappings[j].rawViewOffset;
				mapping.size = value[i].mappings[j].size;
				mapping.loaded = value[i].mappings[j].loaded;
				img.mappings.push_back(mapping);
			}
			result.push_back(img);
		}

		BNDSCViewFreeAllImages(value, count);
		return result;
	}

	std::vector<DSCSymbol> SharedCache::LoadAllSymbolsAndWait()
	{
		size_t count;
		BNDSCSymbolRep* value = BNDSCViewLoadAllSymbolsAndWait(m_object, &count);
		if (value == nullptr)
		{
			return {};
		}

		std::vector<DSCSymbol> result;
		for (size_t i = 0; i < count; i++)
		{
			DSCSymbol sym;
			sym.address = value[i].address;
			sym.name = value[i].name;
			sym.image = value[i].image;
			result.push_back(sym);
		}

		BNDSCViewFreeSymbols(value, count);
		return result;
	}

	std::string SharedCache::GetNameForAddress(uint64_t address)
	{
		char* name = BNDSCViewGetNameForAddress(m_object, address);
		if (name == nullptr)
			return {};
		std::string result = name;
		BNFreeString(name);
		return result;
	}

	std::string SharedCache::GetImageNameForAddress(uint64_t address)
	{
		char* name = BNDSCViewGetImageNameForAddress(m_object, address);
		if (name == nullptr)
			return {};
		std::string result = name;
		BNFreeString(name);
		return result;
	}

	std::optional<SharedCacheMachOHeader> SharedCache::GetMachOHeaderForImage(std::string name)
	{
		char* str = BNAllocString(name.c_str());
		char* outputStr = BNDSCViewGetImageHeaderForName(m_object, str);
		if (outputStr == nullptr)
			return {};
		std::string output = outputStr;
		BNFreeString(outputStr);
		if (output.empty())
			return {};
		SharedCacheMachOHeader header;
		header.LoadFromString(output);
		return header;
	}

	std::optional<SharedCacheMachOHeader> SharedCache::GetMachOHeaderForAddress(uint64_t address)
	{
		char* outputStr = BNDSCViewGetImageHeaderForAddress(m_object, address);
		if (outputStr == nullptr)
			return {};
		std::string output = outputStr;
		BNFreeString(outputStr);
		if (output.empty())
			return {};
		SharedCacheMachOHeader header;
		header.LoadFromString(output);
		return header;
	}

	BNDSCViewState SharedCache::GetState()
	{
		return BNDSCViewGetState(m_object);
	}

	void SharedCache::FindSymbolAtAddrAndApplyToAddr(uint64_t symbolLocation, uint64_t targetLocation, bool triggerReanalysis) const
	{
		BNDSCFindSymbolAtAddressAndApplyToAddress(m_object, symbolLocation, targetLocation, triggerReanalysis);
	}


void Serialize(SharedCacheCore::SerializationContext& context, std::string_view name, const mach_header_64& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	bArr.PushBack(b.magic, context.allocator);
	bArr.PushBack(b.cputype, context.allocator);
	bArr.PushBack(b.cpusubtype, context.allocator);
	bArr.PushBack(b.filetype, context.allocator);
	bArr.PushBack(b.ncmds, context.allocator);
	bArr.PushBack(b.sizeofcmds, context.allocator);
	bArr.PushBack(b.flags, context.allocator);
	bArr.PushBack(b.reserved, context.allocator);
	context.doc.AddMember(key, bArr, context.allocator);
}

void Deserialize(SharedCacheCore::DeserializationContext& context, std::string_view name, mach_header_64& b)
{
	auto bArr = context.doc[name.data()].GetArray();
	b.magic = bArr[0].GetInt64();
	b.cputype = bArr[1].GetInt64();
	b.cpusubtype = bArr[2].GetInt64();
	b.filetype = bArr[3].GetInt64();
	b.ncmds = bArr[4].GetInt64();
	b.sizeofcmds = bArr[5].GetInt64();
	b.flags = bArr[6].GetInt64();
	b.reserved = bArr[7].GetInt64();
}

void Serialize(SharedCacheCore::SerializationContext& context, std::string_view name, const symtab_command& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	bArr.PushBack(b.cmd, context.allocator);
	bArr.PushBack(b.cmdsize, context.allocator);
	bArr.PushBack(b.symoff, context.allocator);
	bArr.PushBack(b.nsyms, context.allocator);
	bArr.PushBack(b.stroff, context.allocator);
	bArr.PushBack(b.strsize, context.allocator);
	context.doc.AddMember(key, bArr, context.allocator);
}

void Deserialize(SharedCacheCore::DeserializationContext& context, std::string_view name, symtab_command& b)
{
	auto bArr = context.doc[name.data()].GetArray();
	b.cmd = bArr[0].GetUint();
	b.cmdsize = bArr[1].GetUint();
	b.symoff = bArr[2].GetUint();
	b.nsyms = bArr[3].GetUint();
	b.stroff = bArr[4].GetUint();
	b.strsize = bArr[5].GetUint();
}

void Serialize(SharedCacheCore::SerializationContext& context, std::string_view name, const dysymtab_command& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	bArr.PushBack(b.cmd, context.allocator);
	bArr.PushBack(b.cmdsize, context.allocator);
	bArr.PushBack(b.ilocalsym, context.allocator);
	bArr.PushBack(b.nlocalsym, context.allocator);
	bArr.PushBack(b.iextdefsym, context.allocator);
	bArr.PushBack(b.nextdefsym, context.allocator);
	bArr.PushBack(b.iundefsym, context.allocator);
	bArr.PushBack(b.nundefsym, context.allocator);
	bArr.PushBack(b.tocoff, context.allocator);
	bArr.PushBack(b.ntoc, context.allocator);
	bArr.PushBack(b.modtaboff, context.allocator);
	bArr.PushBack(b.nmodtab, context.allocator);
	bArr.PushBack(b.extrefsymoff, context.allocator);
	bArr.PushBack(b.nextrefsyms, context.allocator);
	bArr.PushBack(b.indirectsymoff, context.allocator);
	bArr.PushBack(b.nindirectsyms, context.allocator);
	bArr.PushBack(b.extreloff, context.allocator);
	bArr.PushBack(b.nextrel, context.allocator);
	bArr.PushBack(b.locreloff, context.allocator);
	bArr.PushBack(b.nlocrel, context.allocator);
	context.doc.AddMember(key, bArr, context.allocator);
}

void Deserialize(SharedCacheCore::DeserializationContext& context, std::string_view name, dysymtab_command& b)
{
	auto bArr = context.doc[name.data()].GetArray();
	b.cmd = bArr[0].GetUint();
	b.cmdsize = bArr[1].GetUint();
	b.ilocalsym = bArr[2].GetUint();
	b.nlocalsym = bArr[3].GetUint();
	b.iextdefsym = bArr[4].GetUint();
	b.nextdefsym = bArr[5].GetUint();
	b.iundefsym = bArr[6].GetUint();
	b.nundefsym = bArr[7].GetUint();
	b.tocoff = bArr[8].GetUint();
	b.ntoc = bArr[9].GetUint();
	b.modtaboff = bArr[10].GetUint();
	b.nmodtab = bArr[11].GetUint();
	b.extrefsymoff = bArr[12].GetUint();
	b.nextrefsyms = bArr[13].GetUint();
	b.indirectsymoff = bArr[14].GetUint();
	b.nindirectsyms = bArr[15].GetUint();
	b.extreloff = bArr[16].GetUint();
	b.nextrel = bArr[17].GetUint();
	b.locreloff = bArr[18].GetUint();
	b.nlocrel = bArr[19].GetUint();
}

void Serialize(SharedCacheCore::SerializationContext& context, std::string_view name, const dyld_info_command& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	bArr.PushBack(b.cmd, context.allocator);
	bArr.PushBack(b.cmdsize, context.allocator);
	bArr.PushBack(b.rebase_off, context.allocator);
	bArr.PushBack(b.rebase_size, context.allocator);
	bArr.PushBack(b.bind_off, context.allocator);
	bArr.PushBack(b.bind_size, context.allocator);
	bArr.PushBack(b.weak_bind_off, context.allocator);
	bArr.PushBack(b.weak_bind_size, context.allocator);
	bArr.PushBack(b.lazy_bind_off, context.allocator);
	bArr.PushBack(b.lazy_bind_size, context.allocator);
	bArr.PushBack(b.export_off, context.allocator);
	bArr.PushBack(b.export_size, context.allocator);
	context.doc.AddMember(key, bArr, context.allocator);
}

void Deserialize(SharedCacheCore::DeserializationContext& context, std::string_view name, dyld_info_command& b)
{
	auto bArr = context.doc[name.data()].GetArray();
	b.cmd = bArr[0].GetUint();
	b.cmdsize = bArr[1].GetUint();
	b.rebase_off = bArr[2].GetUint();
	b.rebase_size = bArr[3].GetUint();
	b.bind_off = bArr[4].GetUint();
	b.bind_size = bArr[5].GetUint();
	b.weak_bind_off = bArr[6].GetUint();
	b.weak_bind_size = bArr[7].GetUint();
	b.lazy_bind_off = bArr[8].GetUint();
	b.lazy_bind_size = bArr[9].GetUint();
	b.export_off = bArr[10].GetUint();
	b.export_size = bArr[11].GetUint();
}

void Serialize(SharedCacheCore::SerializationContext& context, std::string_view name, const routines_command_64& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	bArr.PushBack(b.cmd, context.allocator);
	bArr.PushBack(b.cmdsize, context.allocator);
	bArr.PushBack(b.init_address, context.allocator);
	bArr.PushBack(b.init_module, context.allocator);
	context.doc.AddMember(key, bArr, context.allocator);
}

void Deserialize(SharedCacheCore::DeserializationContext& context, std::string_view name, routines_command_64& b)
{
	auto bArr = context.doc[name.data()].GetArray();
	b.cmd = bArr[0].GetUint();
	b.cmdsize = bArr[1].GetUint();
	b.init_address = bArr[2].GetUint();
	b.init_module = bArr[3].GetUint();
}

void Serialize(SharedCacheCore::SerializationContext& context, std::string_view name, const function_starts_command& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	bArr.PushBack(b.cmd, context.allocator);
	bArr.PushBack(b.cmdsize, context.allocator);
	bArr.PushBack(b.funcoff, context.allocator);
	bArr.PushBack(b.funcsize, context.allocator);
	context.doc.AddMember(key, bArr, context.allocator);
}

void Deserialize(SharedCacheCore::DeserializationContext& context, std::string_view name, function_starts_command& b)
{
	auto bArr = context.doc[name.data()].GetArray();
	b.cmd = bArr[0].GetUint();
	b.cmdsize = bArr[1].GetUint();
	b.funcoff = bArr[2].GetUint();
	b.funcsize = bArr[3].GetUint();
}

void Serialize(SharedCacheCore::SerializationContext& context, std::string_view name, const std::vector<section_64>& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	for (auto& s : b)
	{
		rapidjson::Value sArr(rapidjson::kArrayType);
		std::string sectNameStr;
		char sectName[16];
		memcpy(sectName, s.sectname, 16);
		sectName[15] = 0;
		sectNameStr = std::string(sectName);
		sArr.PushBack(rapidjson::Value(sectNameStr.c_str(), context.allocator), context.allocator);
		std::string segNameStr;
		char segName[16];
		memcpy(segName, s.segname, 16);
		segName[15] = 0;
		segNameStr = std::string(segName);
		sArr.PushBack(rapidjson::Value(segNameStr.c_str(), context.allocator), context.allocator);
		sArr.PushBack(s.addr, context.allocator);
		sArr.PushBack(s.size, context.allocator);
		sArr.PushBack(s.offset, context.allocator);
		sArr.PushBack(s.align, context.allocator);
		sArr.PushBack(s.reloff, context.allocator);
		sArr.PushBack(s.nreloc, context.allocator);
		sArr.PushBack(s.flags, context.allocator);
		sArr.PushBack(s.reserved1, context.allocator);
		sArr.PushBack(s.reserved2, context.allocator);
		sArr.PushBack(s.reserved3, context.allocator);
		bArr.PushBack(sArr, context.allocator);
	}
	context.doc.AddMember(key, bArr, context.allocator);
}

void Deserialize(SharedCacheCore::DeserializationContext& context, std::string_view name, std::vector<section_64>& b)
{
	auto bArr = context.doc[name.data()].GetArray();
	for (auto& s : bArr)
	{
		section_64 sec;
		auto s2 = s.GetArray();
		std::string sectNameStr = s2[0].GetString();
		memcpy(sec.sectname, sectNameStr.c_str(), sectNameStr.size());
		std::string segNameStr = s2[1].GetString();
		memcpy(sec.segname, segNameStr.c_str(), segNameStr.size());
		sec.addr = s2[2].GetUint64();
		sec.size = s2[3].GetUint64();
		sec.offset = s2[4].GetUint();
		sec.align = s2[5].GetUint();
		sec.reloff = s2[6].GetUint();
		sec.nreloc = s2[7].GetUint();
		sec.flags = s2[8].GetUint();
		sec.reserved1 = s2[9].GetUint();
		sec.reserved2 = s2[10].GetUint();
		sec.reserved3 = s2[11].GetUint();
		b.push_back(sec);
	}
}

void Serialize(SharedCacheCore::SerializationContext& context, std::string_view name, const linkedit_data_command& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	bArr.PushBack(b.cmd, context.allocator);
	bArr.PushBack(b.cmdsize, context.allocator);
	bArr.PushBack(b.dataoff, context.allocator);
	bArr.PushBack(b.datasize, context.allocator);
	context.doc.AddMember(key, bArr, context.allocator);
}

void Deserialize(SharedCacheCore::DeserializationContext& context, std::string_view name, linkedit_data_command& b)
{
	auto bArr = context.doc[name.data()].GetArray();
	b.cmd = bArr[0].GetUint();
	b.cmdsize = bArr[1].GetUint();
	b.dataoff = bArr[2].GetUint();
	b.datasize = bArr[3].GetUint();
}

void Serialize(SharedCacheCore::SerializationContext& context, std::string_view name, const segment_command_64& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	std::string segNameStr;
	char segName[16];
	memcpy(segName, b.segname, 16);
	segName[15] = 0;
	segNameStr = std::string(segName);
	bArr.PushBack(rapidjson::Value(segNameStr.c_str(), context.allocator), context.allocator);
	bArr.PushBack(b.vmaddr, context.allocator);
	bArr.PushBack(b.vmsize, context.allocator);
	bArr.PushBack(b.fileoff, context.allocator);
	bArr.PushBack(b.filesize, context.allocator);
	bArr.PushBack(b.maxprot, context.allocator);
	bArr.PushBack(b.initprot, context.allocator);
	bArr.PushBack(b.nsects, context.allocator);
	bArr.PushBack(b.flags, context.allocator);
	context.doc.AddMember(key, bArr, context.allocator);
}

void Deserialize(SharedCacheCore::DeserializationContext& context, std::string_view name, segment_command_64& b)
{
	auto bArr = context.doc[name.data()].GetArray();
	std::string segNameStr = bArr[0].GetString();
	memcpy(b.segname, segNameStr.c_str(), segNameStr.size());
	b.vmaddr = bArr[1].GetUint64();
	b.vmsize = bArr[2].GetUint64();
	b.fileoff = bArr[3].GetUint64();
	b.filesize = bArr[4].GetUint64();
	b.maxprot = bArr[5].GetUint();
	b.initprot = bArr[6].GetUint();
	b.nsects = bArr[7].GetUint();
	b.flags = bArr[8].GetUint();
}

void Serialize(SharedCacheCore::SerializationContext& context, std::string_view name, const std::vector<segment_command_64>& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	for (auto& s : b)
	{
		rapidjson::Value sArr(rapidjson::kArrayType);
		std::string segNameStr;
		char segName[16];
		memcpy(segName, s.segname, 16);
		segName[15] = 0;
		segNameStr = std::string(segName);
		sArr.PushBack(rapidjson::Value(segNameStr.c_str(), context.allocator), context.allocator);
		sArr.PushBack(s.vmaddr, context.allocator);
		sArr.PushBack(s.vmsize, context.allocator);
		sArr.PushBack(s.fileoff, context.allocator);
		sArr.PushBack(s.filesize, context.allocator);
		sArr.PushBack(s.maxprot, context.allocator);
		sArr.PushBack(s.initprot, context.allocator);
		sArr.PushBack(s.nsects, context.allocator);
		sArr.PushBack(s.flags, context.allocator);
		bArr.PushBack(sArr, context.allocator);
	}
	context.doc.AddMember(key, bArr, context.allocator);
}

void Deserialize(SharedCacheCore::DeserializationContext& context, std::string_view name, std::vector<segment_command_64>& b)
{
	auto bArr = context.doc[name.data()].GetArray();
	for (auto& s : bArr)
	{
		segment_command_64 sec;
		auto s2 = s.GetArray();
		std::string segNameStr = s2[0].GetString();
		memcpy(sec.segname, segNameStr.c_str(), segNameStr.size());
		sec.vmaddr = s2[1].GetUint64();
		sec.vmsize = s2[2].GetUint64();
		sec.fileoff = s2[3].GetUint64();
		sec.filesize = s2[4].GetUint64();
		sec.maxprot = s2[5].GetUint();
		sec.initprot = s2[6].GetUint();
		sec.nsects = s2[7].GetUint();
		sec.flags = s2[8].GetUint();
		b.push_back(sec);
	}
}

void Serialize(SharedCacheCore::SerializationContext& context, std::string_view name, const build_version_command& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	bArr.PushBack(b.cmd, context.allocator);
	bArr.PushBack(b.cmdsize, context.allocator);
	bArr.PushBack(b.platform, context.allocator);
	bArr.PushBack(b.minos, context.allocator);
	bArr.PushBack(b.sdk, context.allocator);
	bArr.PushBack(b.ntools, context.allocator);
	context.doc.AddMember(key, bArr, context.allocator);
}

void Deserialize(SharedCacheCore::DeserializationContext& context, std::string_view name, build_version_command& b)
{
	auto bArr = context.doc[name.data()].GetArray();
	b.cmd = bArr[0].GetUint();
	b.cmdsize = bArr[1].GetUint();
	b.platform = bArr[2].GetUint();
	b.minos = bArr[3].GetUint();
	b.sdk = bArr[4].GetUint();
	b.ntools = bArr[5].GetUint();
}

void Serialize(SharedCacheCore::SerializationContext& context, std::string_view name, const std::vector<build_tool_version>& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	for (auto& s : b)
	{
		rapidjson::Value sArr(rapidjson::kArrayType);
		sArr.PushBack(s.tool, context.allocator);
		sArr.PushBack(s.version, context.allocator);
		bArr.PushBack(sArr, context.allocator);
	}
	context.doc.AddMember(key, bArr, context.allocator);
}

void Deserialize(SharedCacheCore::DeserializationContext& context, std::string_view name, std::vector<build_tool_version>& b)
{
	auto bArr = context.doc[name.data()].GetArray();
	for (auto& s : bArr)
	{
		build_tool_version sec;
		auto s2 = s.GetArray();
		sec.tool = s2[0].GetUint();
		sec.version = s2[1].GetUint();
		b.push_back(sec);
	}
}

}	// namespace SharedCacheAPI
