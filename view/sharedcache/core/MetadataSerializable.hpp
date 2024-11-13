//
// Created by kat on 5/31/23.
//

/*
 * Welcome to, this file.
 *
 * This is a metadata serialization helper.
 *
 * Have you ever wished turning a complex datastructure into a Metadata object was as easy in C++ as it is in python?
 * Do you like macros and templates?
 *
 * Great news.
 *
 * Implement these on your `public MetadataSerializable<T>` subclass:
 * ```
    class MyClass : public MetadataSerializable<MyClass> {
		void Store(SerializationContext& context) const {
			MSS(m_someVariable);
			MSS(m_someOtherVariable);
		}
		void Load(DeserializationContext& context) {
			MSL(m_someVariable);
			MSL(m_someOtherVariable);
		}
	}
 ```
 * Then, you can turn your object into a Metadata object with `AsMetadata()`, and load it back with
 `LoadFromMetadata()`.
 *
 * Serialized fields will be automatically repopulated.
 *
 * Other ser/deser formats (rapidjson objects, strings) also exist. You can use these to achieve nesting, but probably
 avoid that.
 * */

#include "binaryninjaapi.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/prettywriter.h"

#ifndef SHAREDCACHE_CORE_METADATASERIALIZABLE_HPP
#define SHAREDCACHE_CORE_METADATASERIALIZABLE_HPP

namespace SharedCacheCore {

#define MSS(name)						 context.store(#name, name)
#define MSS_CAST(name, type)			 context.store(#name, (type) name)
#define MSS_SUBCLASS(name)		 		 Serialize(context, #name, name)
#define MSL(name)						 name = context.load<decltype(name)>(#name)
#define MSL_CAST(name, storedType, type) name = (type)context.load<storedType>(#name)
#define MSL_SUBCLASS(name)				 Deserialize(context, #name, name)

using namespace BinaryNinja;

struct DeserializationContext;

struct SerializationContext {
	rapidjson::Document doc;
	rapidjson::Document::AllocatorType allocator;

	SerializationContext() {
		doc.SetObject();
		allocator = doc.GetAllocator();
	}

	template <typename T>
	void store(std::string_view x, const T& y)
	{
		Serialize(*this, x, y);
	}
};

struct DeserializationContext {
	rapidjson::Document doc;

	template <typename T>
	T load(std::string_view x)
	{
		T value;
		Deserialize(*this, x, value);
		return value;
	}
};

template <typename Derived>
class MetadataSerializable
{
public:
	std::string AsString() const
	{
		rapidjson::StringBuffer strbuf;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(strbuf);
		AsDocument().Accept(writer);

		return strbuf.GetString();
	}

	rapidjson::Document AsDocument() const {
		SerializationContext context;
		AsDerived().Store(context);
		return std::move(context.doc);
	}

	void LoadFromString(const std::string& s)
	{
		DeserializationContext context;
		context.doc.Parse(s.c_str());
		AsDerived().Load(context);
	}

	void LoadFromValue(rapidjson::Value& s)
	{
		DeserializationContext context;
		context.doc.CopyFrom(s, context.doc.GetAllocator());
		AsDerived().Load(context);
	}

	Ref<Metadata> AsMetadata() { return new Metadata(AsString()); }

	bool LoadFromMetadata(const Ref<Metadata>& meta)
	{
		if (!meta->IsString())
			return false;
		LoadFromString(meta->GetString());
		return true;
	}

private:
	const Derived& AsDerived() const { return static_cast<const Derived&>(*this); }
	Derived& AsDerived() { return static_cast<Derived&>(*this); }
};

inline void Serialize(SerializationContext& context, std::string_view name, bool b)
{
	rapidjson::Value key(name.data(), context.allocator);
	context.doc.AddMember(key, b, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, bool& b) {
	b = context.doc[name.data()].GetBool();
}

inline void Serialize(SerializationContext& context, std::string_view name, uint8_t b)
{
	rapidjson::Value key(name.data(), context.allocator);
	context.doc.AddMember(key, b, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, uint8_t& b)
{
	b = static_cast<uint8_t>(context.doc[name.data()].GetUint64());
}

inline void Serialize(SerializationContext& context, std::string_view name, uint16_t b)
{
	rapidjson::Value key(name.data(), context.allocator);
	context.doc.AddMember(key, b, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, uint16_t& b)
{
	b = static_cast<uint16_t>(context.doc[name.data()].GetUint64());
}

inline void Serialize(SerializationContext& context, std::string_view name, uint32_t b)
{
	rapidjson::Value key(name.data(), context.allocator);
	context.doc.AddMember(key, b, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, uint32_t& b)
{
	b = static_cast<uint32_t>(context.doc[name.data()].GetUint64());
}

inline void Serialize(SerializationContext& context, std::string_view name, uint64_t b)
{
	rapidjson::Value key(name.data(), context.allocator);
	context.doc.AddMember(key, b, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, uint64_t& b)
{
	b = context.doc[name.data()].GetUint64();
}

inline void Serialize(SerializationContext& context, std::string_view name, int8_t b)
{
	rapidjson::Value key(name.data(), context.allocator);
	context.doc.AddMember(key, b, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, int8_t& b)
{
	b = context.doc[name.data()].GetInt64();
}

inline void Serialize(SerializationContext& context, std::string_view name, int16_t b)
{
	rapidjson::Value key(name.data(), context.allocator);
	context.doc.AddMember(key, b, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, int16_t& b)
{
	b = context.doc[name.data()].GetInt64();
}

inline void Serialize(SerializationContext& context, std::string_view name, int32_t b)
{
	rapidjson::Value key(name.data(), context.allocator);
	context.doc.AddMember(key, b, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, int32_t& b)
{
	b = context.doc[name.data()].GetInt();
}

inline void Serialize(SerializationContext& context, std::string_view name, int64_t b)
{
	rapidjson::Value key(name.data(), context.allocator);
	context.doc.AddMember(key, b, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, int64_t& b)
{
	b = context.doc[name.data()].GetInt64();
}

inline void Serialize(SerializationContext& context, std::string_view name, std::string_view b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value value(b.data(), context.allocator);
	context.doc.AddMember(key, value, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, std::string& b)
{
	b = context.doc[name.data()].GetString();
}

inline void Serialize(SerializationContext& context, std::string_view name, const std::map<uint64_t, std::string>& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	for (auto& i : b)
	{
		rapidjson::Value p(rapidjson::kArrayType);
		p.PushBack(i.first, context.allocator);
		rapidjson::Value value(i.second.c_str(), context.allocator);
		p.PushBack(value, context.allocator);
		bArr.PushBack(p, context.allocator);
	}
	context.doc.AddMember(key, bArr, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, std::map<uint64_t, std::string>& b)
{
	for (auto& i : context.doc[name.data()].GetArray())
		b[i.GetArray()[0].GetUint64()] = i.GetArray()[1].GetString();
}

inline void Serialize(SerializationContext& context, std::string_view name, const std::unordered_map<uint64_t, std::string>& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	for (auto& i : b)
	{
		rapidjson::Value p(rapidjson::kArrayType);
		p.PushBack(i.first, context.allocator);
		rapidjson::Value value(i.second.c_str(), context.allocator);
		p.PushBack(value, context.allocator);
		bArr.PushBack(p, context.allocator);
	}
	context.doc.AddMember(key, bArr, context.allocator);
}

inline void Serialize(SerializationContext& context, std::string_view name, const std::unordered_map<std::string, std::string>& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	for (auto& i : b)
	{
		rapidjson::Value p(rapidjson::kArrayType);
		rapidjson::Value _key(i.first.c_str(), context.allocator);
		rapidjson::Value value(i.second.c_str(), context.allocator);
		p.PushBack(_key, context.allocator);
		p.PushBack(value, context.allocator);
		bArr.PushBack(p, context.allocator);
	}
	context.doc.AddMember(key, bArr, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, std::unordered_map<uint64_t, std::string>& b)
{
	for (auto& i : context.doc[name.data()].GetArray())
		b[i.GetArray()[0].GetUint64()] = i.GetArray()[1].GetString();
}

inline void Serialize(SerializationContext& context, std::string_view name, const std::unordered_map<uint64_t, uint64_t>& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	for (auto& i : b)
	{
		rapidjson::Value p(rapidjson::kArrayType);
		p.PushBack(i.first, context.allocator);
		p.PushBack(i.second, context.allocator);
		bArr.PushBack(p, context.allocator);
	}
	context.doc.AddMember(key, bArr, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, std::unordered_map<uint64_t, uint64_t>& b)
{
	for (auto& i : context.doc[name.data()].GetArray())
		b[i.GetArray()[0].GetUint64()] = i.GetArray()[1].GetUint64();
}

// std::unordered_map<std::string, std::unordered_map<uint64_t, uint64_t>>
inline void Serialize(SerializationContext& context, std::string_view name, const std::unordered_map<std::string, std::unordered_map<uint64_t, uint64_t>>& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value classes(rapidjson::kArrayType);
	for (auto& i : b)
	{
		rapidjson::Value classArr(rapidjson::kArrayType);
		rapidjson::Value classKey(i.first.c_str(), context.allocator);
		classArr.PushBack(classKey, context.allocator);
		rapidjson::Value membersArr(rapidjson::kArrayType);
		for (auto& j : i.second)
		{
			rapidjson::Value member(rapidjson::kArrayType);
			member.PushBack(j.first, context.allocator);
			member.PushBack(j.second, context.allocator);
			membersArr.PushBack(member, context.allocator);
		}
		classArr.PushBack(membersArr, context.allocator);
		classes.PushBack(classArr, context.allocator);
	}
	context.doc.AddMember(key, classes, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, std::unordered_map<std::string, std::unordered_map<uint64_t, uint64_t>>& b)
{
	for (auto& i : context.doc[name.data()].GetArray())
	{
		std::string key = i.GetArray()[0].GetString();
		std::unordered_map<uint64_t, uint64_t> memArray;
		for (auto& member : i.GetArray()[1].GetArray())
		{
			memArray[member.GetArray()[0].GetUint64()] = member.GetArray()[1].GetUint64();
		}
		b[key] = memArray;
	}
}

inline void Deserialize(DeserializationContext& context, std::string_view name, std::unordered_map<std::string, std::string>& b)
{
	for (auto& i : context.doc[name.data()].GetArray())
		b[i.GetArray()[0].GetString()] = i.GetArray()[1].GetString();
}

inline void Serialize(SerializationContext& context, std::string_view name, const std::vector<std::string>& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	for (const auto& s : b)
	{
		rapidjson::Value value(s.c_str(), context.allocator);
		bArr.PushBack(value, context.allocator);
	}
	context.doc.AddMember(key, bArr, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, std::vector<std::string>& b)
{
	for (auto& i : context.doc[name.data()].GetArray())
		b.emplace_back(i.GetString());
}

inline void Serialize(SerializationContext& context, std::string_view name, const std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>>& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	for (auto& i : b)
	{
		rapidjson::Value segV(rapidjson::kArrayType);
		segV.PushBack(i.first, context.allocator);
		segV.PushBack(i.second.first, context.allocator);
		segV.PushBack(i.second.second, context.allocator);
		bArr.PushBack(segV, context.allocator);
	}
	context.doc.AddMember(key, bArr, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>>& b)
{
	for (auto& i : context.doc[name.data()].GetArray())
	{
		std::pair<uint64_t, std::pair<uint64_t, uint64_t>> j;
		j.first = i.GetArray()[0].GetUint64();
		j.second.first = i.GetArray()[1].GetUint64();
		j.second.second = i.GetArray()[2].GetUint64();
		b.push_back(j);
	}
}

inline void Serialize(SerializationContext& context, std::string_view name, const std::vector<std::pair<uint64_t, bool>>& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	for (auto& i : b)
	{
		rapidjson::Value segV(rapidjson::kArrayType);
		segV.PushBack(i.first, context.allocator);
		segV.PushBack(i.second, context.allocator);
		bArr.PushBack(segV, context.allocator);
	}
	context.doc.AddMember(key, bArr, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, std::vector<std::pair<uint64_t, bool>>& b)
{
	for (auto& i : context.doc[name.data()].GetArray())
	{
		std::pair<uint64_t, bool> j;
		j.first = i.GetArray()[0].GetUint64();
		j.second = i.GetArray()[1].GetBool();
		b.push_back(j);
	}
}

inline void Serialize(SerializationContext& context, std::string_view name, const std::vector<uint64_t>& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	for (auto& i : b)
	{
		bArr.PushBack(i, context.allocator);
	}
	context.doc.AddMember(key, bArr, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, std::vector<uint64_t>& b)
{
	for (auto& i : context.doc[name.data()].GetArray())
	{
		b.push_back(i.GetUint64());
	}
}

// std::unordered_map<std::string, uint64_t>
inline void Serialize(SerializationContext& context, std::string_view name, const std::unordered_map<std::string, uint64_t>& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	for (auto& i : b)
	{
		rapidjson::Value p(rapidjson::kArrayType);
		rapidjson::Value _key(i.first.c_str(), context.allocator);
		p.PushBack(_key, context.allocator);
		p.PushBack(i.second, context.allocator);
		bArr.PushBack(p, context.allocator);
	}
	context.doc.AddMember(key, bArr, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, std::unordered_map<std::string, uint64_t>& b)
{
	for (auto& i : context.doc[name.data()].GetArray())
	{
		b[i.GetArray()[0].GetString()] = i.GetArray()[1].GetUint64();
	}
}

// std::vector<std::pair<uint64_t, std::vector<std::pair<uint64_t, std::string>>>>
inline void Serialize(SerializationContext& context, std::string_view name, const std::vector<std::pair<uint64_t, std::vector<std::pair<uint64_t, std::string>>>>& b)
{
	rapidjson::Value key(name.data(), context.allocator);
	rapidjson::Value bArr(rapidjson::kArrayType);
	for (auto& i : b)
	{
		rapidjson::Value segV(rapidjson::kArrayType);
		segV.PushBack(i.first, context.allocator);
		rapidjson::Value segArr(rapidjson::kArrayType);
		for (auto& j : i.second)
		{
			rapidjson::Value segPair(rapidjson::kArrayType);
			segPair.PushBack(j.first, context.allocator);
			rapidjson::Value segStr(j.second.c_str(), context.allocator);
			segPair.PushBack(segStr, context.allocator);
			segArr.PushBack(segPair, context.allocator);
		}
		segV.PushBack(segArr, context.allocator);
		bArr.PushBack(segV, context.allocator);
	}
	context.doc.AddMember(key, bArr, context.allocator);
}

inline void Deserialize(DeserializationContext& context, std::string_view name, std::vector<std::pair<uint64_t, std::vector<std::pair<uint64_t, std::string>>>>& b)
{
	for (auto& i : context.doc[name.data()].GetArray())
	{
		std::pair<uint64_t, std::vector<std::pair<uint64_t, std::string>>> j;
		j.first = i.GetArray()[0].GetUint64();
		for (auto& k : i.GetArray()[1].GetArray())
		{
			j.second.push_back({k.GetArray()[0].GetUint64(), k.GetArray()[1].GetString()});
		}
		b.push_back(j);
	}
}

} // namespace SharedCacheCore

#endif	// SHAREDCACHE_METADATASERIALIZABLE_HPP
