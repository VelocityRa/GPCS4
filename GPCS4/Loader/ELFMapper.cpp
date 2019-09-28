#include "ELFMapper.h"
#include "Platform/PlatformUtils.h"

#include <cassert>
#include <algorithm>

bool ELFMapper::loadFile(std::string const & filePath, MemoryMappedModule * mod)
{
	UtilFile::file_uptr file = {};
	bool retVal = false;
	
	do
	{
		if (mod == nullptr)
		{
			LOG_ERR("Null module pointer");
			break;
		}

		m_moduleData = mod;
		file.reset(fopen(filePath.c_str(), "rb"));

		if (file == nullptr)
		{
			LOG_ERR("fail to open file %s", filePath.c_str());
			break;
		}

		fseek(file.get(), 0, SEEK_END);
		size_t fileSize = 0;

		long pos = ftell(file.get());
		if (pos == -1)
		{
			LOG_ERR("fail to get file size");
			break;
		}

		fileSize = static_cast<size_t>(pos);
		mod->fileMemory.resize(fileSize);

		fseek(file.get(), 0, SEEK_SET);
		size_t numOfBytes = fread(&mod->fileMemory[0], fileSize, 1, file.get());
		if (numOfBytes != fileSize)
		{
			LOG_ERR("fail to read file");
			break;
		}
		
		retVal = true;

	} while (false);
	return retVal;
}

bool ELFMapper::validateHeader()
{

	bool retVal = false;
	auto &fileMemory = m_moduleData->fileMemory;

	do 
	{
		if (m_moduleData == nullptr)
		{
			LOG_ERR("file has not been loaded");
			break;
		}

		if (fileMemory.size() < sizeof(m_moduleData->elfHeader))
		{
			LOG_ERR("file size error. size=%d", fileMemory.size());
			break;
		}

		m_moduleData->elfHeader = reinterpret_cast<Elf64_Ehdr*>(fileMemory.data());
		auto elfHeader = m_moduleData->elfHeader;

		if (strncmp((char*)elfHeader->e_ident, ELFMAG, SELFMAG))
		{
			LOG_ERR("ELF identifier mismatch");
			break;
		}

		if ((elfHeader->e_type != ET_SCE_DYNEXEC &&
			 elfHeader->e_type != ET_SCE_DYNAMIC) ||
			elfHeader->e_machine != EM_X86_64)
		{
			LOG_ERR("unspported TYPE/ARCH. e_type=%d, e_machine=%d",
					elfHeader->e_type, elfHeader->e_machine);

			break;
		}

		retVal = true;

	} while (false);

	return retVal;
}

bool ELFMapper::parseSegmentHeaders()
{
	bool retVal = true;

	do
	{
		if (m_moduleData == nullptr)
		{
			LOG_ERR("null pointer error");
			retVal = false;
			break;
		}

		auto &fileMemory = m_moduleData->fileMemory;
		MODULE_INFO &info = m_moduleData->moduleInfo;
		byte *pSegmentHeader = fileMemory.data() + m_moduleData->elfHeader->e_phoff;
		uint shCount = m_moduleData->elfHeader->e_phnum;

		m_moduleData->segmentHeaders.resize(shCount);

		memcpy(m_moduleData->segmentHeaders.data(), pSegmentHeader,
			   shCount * sizeof(Elf64_Phdr));

		byte* pBuffer = fileMemory.data();

		for (auto &hdr : m_moduleData->segmentHeaders)
		{
			switch (hdr.p_type)
			{
			case PT_LOAD:
			case PT_SCE_PROCPARAM:
			case PT_INTERP:
			case PT_GNU_EH_FRAME:
			case PT_SCE_RELRO:
				// TODO: what is PT_SCE_MODULEPARAM used for?
			case PT_SCE_MODULEPARAM:
				break;

			case PT_DYNAMIC:
			{
				info.pDynamic = pBuffer + hdr.p_offset;
				info.nDynamicSize = hdr.p_filesz;
			}
			break;

			case PT_TLS:
			{
				info.pTlsAddr = reinterpret_cast<byte*>(hdr.p_vaddr);
				info.nTlsInitSize = hdr.p_filesz;
				info.nTlsSize = ALIGN_ROUND(hdr.p_memsz, hdr.p_align);
			}
			break;

			case PT_SCE_DYNLIBDATA:
			{
				info.pSceDynLib = pBuffer + hdr.p_offset;
				info.nSceDynLibSize = hdr.p_filesz;
			}
			break;

			case PT_SCE_COMMENT:
			{
				info.pSceComment = pBuffer + hdr.p_offset;
				info.nSceCommentSize = hdr.p_filesz;
			}
			break;

			case PT_SCE_LIBVERSION:
			{
				info.pSceLibVersion = pBuffer + hdr.p_offset;
				info.nSceLibVersionSize = hdr.p_filesz;
			}
			break;

			default:
			{
				LOG_FIXME("segment type: %x is not supported", hdr.p_type);
				retVal = false;
			}
			break;
			}
		}
	} while (false);

	return retVal;
}

bool ELFMapper::parseDynamicSection()
{
	assert(m_moduleData != nullptr);
	MODULE_INFO &info = m_moduleData->moduleInfo;

	bool retVal = false;

	do
	{
		Elf64_Dyn *pDynEntries = reinterpret_cast<Elf64_Dyn*>(info.pDynamic);
		uint dynEntriesCount = info.nDynamicSize / sizeof(Elf64_Dyn);
		byte *pStrTab = info.pStrTab;

		for (uint i = 0; i < dynEntriesCount; i++)
		{
			retVal = prepareTables(pDynEntries[i], i);
			if (retVal == false)
			{
				break;
			}
		}

		if (retVal == false)
		{
			break;
		}

		for (uint i = 0; i < dynEntriesCount; i++)
		{
			retVal = parseSingleDynEntry(pDynEntries[i], i);
			if (retVal == false)
			{
				break;
			}
		}

	} while (false);

	return retVal;
}

bool ELFMapper::mapImageIntoMemroy()
{
	bool retVal = false;
	MODULE_INFO &info = m_moduleData->moduleInfo;

	do
	{
		size_t totalSize = calculateTotalLoadableSize();
		if (totalSize == 0)
		{
			break;
		}

		byte* buffer = reinterpret_cast<byte*>(
			UtilMemory::VMMap(totalSize, UtilMemory::VMPF_READ | UtilMemory::VMPF_EXECUTE)
			);

		if (buffer == nullptr)
		{
			break;
		}

		m_moduleData->mappedMemory.reset(buffer);
		m_moduleData->mappedSize = totalSize;

		info.pMappedAddr = buffer;
		info.nMappedSize = totalSize;

		for (auto const &phdr : m_moduleData->segmentHeaders)
		{
			if (phdr.p_flags & PF_X)
			{
				retVal = mapCodeSegment(phdr);
			}
			else if (phdr.p_type == PT_SCE_RELRO)
			{
				retVal = mapSecReloSegment(phdr);
			}
			else if (phdr.p_flags & PF_W)
			{
				retVal = mapDataSegment(phdr);
				// there should no longer be segment to be mapped,
				// and we stop enumerating right here.
				break;
			}
		}

	} while (false);

	return retVal;
}

bool ELFMapper::parseSymbols()
{
	MODULE_INFO &info = m_moduleData->moduleInfo;
	auto pSymbolTable = (Elf64_Sym*)info.pSymTab;
	auto tableSize = info.nSymTabSize / sizeof(Elf64_Sym);

	for (uint i = 0; i < tableSize; i++)
	{
		auto const &symbol = reinterpret_cast<Elf64_Sym*>(info.pSymTab)[i];
		auto binding = ELF64_ST_BIND(symbol.st_info);
		auto name = (char*)(&info.pStrTab[symbol.st_name]);

		auto type = symbol.st_value == 0 ? "UNDEF" : "EXPORT";

		switch (binding)
		{
		case STB_LOCAL:
		{
			LOG_DEBUG("%s symbol: %s BINDING: STB_LOCAL VAL: %d", type, name, symbol.st_value);
		}
		break;

		case STB_GLOBAL:
		{
			LOG_DEBUG("%s symbol: %s BINDING: STB_GLOBAL VAL: %d", type, name, symbol.st_value);
			void *addr = m_moduleData->mappedMemory.get() + symbol.st_value;
			m_moduleData->exportSymbols.insert(std::make_pair(name, addr));
		}
		break;

		case STB_WEAK:
		{
			LOG_DEBUG("%s symbol: %s BINDING: STB_WEAK VAL: %d", type, name, symbol.st_value);
			void *addr = m_moduleData->mappedMemory.get() + symbol.st_value;
			m_moduleData->exportSymbols.insert(std::make_pair(name, addr));
		}
		break;

		}
	}

	return false;
}

bool ELFMapper::prepareTables(Elf64_Dyn const & entry, uint index)
{
	MODULE_INFO &info = m_moduleData->moduleInfo;
	byte *pDynBaseAddr = info.pSceDynLib;
	bool retVal = true;

	switch (entry.d_tag)
	{
	case DT_NULL:
	case DT_NEEDED:
	case DT_DEBUG:
	case DT_TEXTREL:
	case DT_INIT_ARRAY:
	case DT_FINI_ARRAY:
	case DT_INIT_ARRAYSZ:
	case DT_FINI_ARRAYSZ:
	case DT_FLAGS:
	case DT_PREINIT_ARRAY:
	case DT_PREINIT_ARRAYSZ:
	case DT_SCE_FINGERPRINT:
	case DT_SCE_ORIGINAL_FILENAME:
	case DT_SCE_MODULE_INFO:
	case DT_SCE_NEEDED_MODULE:
	case DT_SCE_MODULE_ATTR:
	case DT_SCE_EXPORT_LIB:
	case DT_SCE_IMPORT_LIB:
	case DT_SCE_EXPORT_LIB_ATTR:
	case DT_SCE_IMPORT_LIB_ATTR:
	case DT_SCE_RELAENT:
	case DT_SCE_SYMENT:
	case DT_SCE_HASH:
	case DT_SCE_HASHSZ:
		break;

	case DT_INIT:
	{
		LOG_DEBUG("INIT addr: %08x", entry.d_un.d_ptr);
	}
	break;

	case DT_FINI:
	{
		LOG_DEBUG("FINI addr: %08x", entry.d_un.d_ptr);
	}
	break;

	case DT_SCE_PLTGOT:
	{
		LOG_DEBUG("PLTGOT addr: %08x", entry.d_un.d_ptr);
	}
	break;

	/*
		symbol table

		Is it possible for a elf file to have multiple symbol tables?
	*/
	case DT_SCE_SYMTAB:
	{
		LOG_DEBUG("    %i: d_tag = %s, d_un = %08x", index, "DT_SCE_SYMTAB", entry.d_un.d_val);

		// I'm not sure if it is possible for a ELF file to have multiple symbol tables.
		// I simply assume there could only be one, and therefore before it is being set, the
		// value of the symobl table pointer should be null.
		assert(info.pSymTab == nullptr);
		info.pSymTab = pDynBaseAddr + entry.d_un.d_ptr;
	}
	break;

	case DT_SCE_SYMTABSZ:
	{
		LOG_DEBUG("    %i: d_tag = %s, d_un = %08x", index, "DT_SCE_SYMTABSZ", entry.d_un.d_val);
		assert(info.nSymTabSize == 0);
		info.nSymTabSize = entry.d_un.d_val;
	}
	break;

	/*
		string table
	*/
	case DT_SCE_STRTAB:
	{
		LOG_DEBUG("    %i: d_tag = %s, d_un = %08x", index, "DT_SCE_STRTAB", entry.d_un.d_val);
		assert(info.pStrTab == nullptr);
		info.pStrTab = pDynBaseAddr + entry.d_un.d_ptr;
	}
	break;

	case DT_SCE_STRSZ:
	{
		LOG_DEBUG("    %i: d_tag = %s, d_un = %08x", index, "DT_SCE_STRSZ", entry.d_un.d_val);
		assert(info.nStrTabSize == 0);
		info.nStrTabSize = entry.d_un.d_val;
	}
	break;

	/*
		Relocation table
	*/
	case DT_SCE_RELA:
	{
		LOG_DEBUG("    %i: d_tag = %s, d_un = %08x", index, "DT_SCE_RELA", entry.d_un.d_val);
		assert(info.pRela == nullptr);
		info.pRela = pDynBaseAddr + entry.d_un.d_ptr;
	}
	break;

	case DT_SCE_RELASZ:
	{
		LOG_DEBUG("    %i: d_tag = %s, d_un = %08x", index, "DT_SCE_RELASZ", entry.d_un.d_val);
		assert(info.nRelaCount == 0);
		info.nRelaCount = entry.d_un.d_val / sizeof(Elf64_Rela);
	}
	break;

	/*
		PLT relocation table
	*/
	case DT_SCE_JMPREL:
	{
		LOG_DEBUG("    %i: d_tag = %s, d_un = %08x", index, "DT_SCE_JMPREL", entry.d_un.d_val);
		assert(info.pPltRela == nullptr);
		info.pPltRela = pDynBaseAddr + entry.d_un.d_ptr;
	}
	break;

	case DT_SCE_PLTREL:
	{
		LOG_DEBUG("    %i: d_tag = %s, d_un = %08x", index, "DT_SCE_PLTREL", entry.d_un.d_val);
		assert(info.nPltRelType == 0);
		info.nPltRelType = entry.d_un.d_val;
	}
	break;

	case DT_SCE_PLTRELSZ:
	{
		LOG_DEBUG("    %i: d_tag = %s, d_un = %08x", index, "DT_SCE_PLTRELSZ", entry.d_un.d_val);
		assert(info.nPltRelaCount == 0);
		info.nPltRelaCount = entry.d_un.d_val;
	}
	break;

	default:
	{
		LOG_FIXME("    %i UNKNOWN DTAG: 0x%08x", index, entry.d_tag);
		retVal = true;
	}
	break;
	}


	return retVal;
}

bool ELFMapper::parseSingleDynEntry(Elf64_Dyn const & entry, uint index)
{
	MODULE_INFO &info = m_moduleData->moduleInfo;
	byte *strTable = info.pStrTab;


	switch (entry.d_tag)
	{
	case DT_NEEDED:
	{
		char *fileName = (char*)&strTable[entry.d_un.d_ptr];
		LOG_DEBUG("DT_NEEDED: %s", fileName);
	}
	break;

	case DT_SCE_MODULE_INFO:
	{
		IMPORT_MODULE mod;
		mod.value = entry.d_un.d_val;
		mod.strName = reinterpret_cast<char*>(&strTable[mod.name_offset]);
		LOG_DEBUG("DT_SCE_MODULE_INFO: %s", mod.strName.c_str());
	}
	break;

	case DT_SCE_NEEDED_MODULE:
	{
		IMPORT_MODULE mod;
		mod.value = entry.d_un.d_val;
		mod.strName = reinterpret_cast<char*>(&strTable[mod.name_offset]);
		m_moduleData->importModules.push_back(mod);
		LOG_DEBUG("DT_SCE_NEEDED_MODULE: %s", mod.strName.c_str());
	}
	break;

	case DT_SCE_EXPORT_LIB:
	{
		IMPORT_LIBRARY lib;
		lib.value = entry.d_un.d_val;
		lib.strName = reinterpret_cast<char*>(&strTable[lib.name_offset]);
		LOG_DEBUG("DT_SCE_EXPORT_LIB %s", lib.strName.c_str());
		// m_ELFMeta->importLibraries.push_back(lib);
	}
	break;

	case DT_SCE_IMPORT_LIB:
	{
		IMPORT_LIBRARY lib;
		lib.value = entry.d_un.d_val;
		lib.strName = reinterpret_cast<char*>(&strTable[lib.name_offset]);
		m_moduleData->importLibraries.push_back(lib);
		LOG_DEBUG("DT_SCE_IMPORT_LIB %s", lib.strName.c_str());
	}
	break;

	}

	return true;
}

size_t ELFMapper::calculateTotalLoadableSize()
{
	size_t loadAddrBegin = 0;
	size_t loadAddrEnd = 0;

	for (auto &phdr : m_moduleData->segmentHeaders)
	{
		if (isSegmentLoadable(phdr))
		{
			if (phdr.p_vaddr < loadAddrBegin)
			{
				loadAddrBegin = phdr.p_vaddr;
			}

			size_t alignedAddr = ALIGN_DOWN(phdr.p_vaddr + phdr.p_memsz, phdr.p_align);
			if (alignedAddr > loadAddrEnd)
			{
				loadAddrEnd = alignedAddr;
			}
		}
	}

	return (loadAddrEnd - loadAddrBegin);
}

bool ELFMapper::isSegmentLoadable(Elf64_Phdr const & hdr)
{
	bool retVal;

	if (hdr.p_type == PT_SCE_RELRO)
	{
		retVal = true;
	}
	else if (hdr.p_type == PT_LOAD)
	{
		retVal = true;
	}
	else
	{
		retVal = false;
	}

	return retVal;
}

bool ELFMapper::mapCodeSegment(Elf64_Phdr const & phdr)
{
	bool retVal = false;
	MODULE_INFO &info = m_moduleData->moduleInfo;
	auto &fileData = m_moduleData->fileMemory;
	do
	{
		if (fileData.empty())
		{
			break;
		}

		info.nCodeSize = phdr.p_memsz;
		info.pCodeAddr = reinterpret_cast<byte*>(
			ALIGN_DOWN(size_t(info.pMappedAddr) + phdr.p_vaddr, phdr.p_align)
			);

		byte *fileDataPtr = reinterpret_cast<byte*>(m_moduleData->mappedMemory.get()) + phdr.p_offset;
		memcpy(info.pCodeAddr, fileDataPtr, phdr.p_filesz);

		info.pEntryPoint = info.pCodeAddr + m_moduleData->elfHeader->e_entry;
		info.pTlsAddr = info.pCodeAddr + (uint64_t)info.pTlsAddr;

		retVal = true;

	} while (false);

	return retVal;
}

bool ELFMapper::mapSecReloSegment(Elf64_Phdr const & phdr)
{
	bool retVal = false;
	MODULE_INFO &info = m_moduleData->moduleInfo;
	auto &fileData = m_moduleData->fileMemory;

	do
	{
		if (fileData.empty())
		{
			break;
		}

		info.nCodeSize = phdr.p_memsz;
		info.pCodeAddr = reinterpret_cast<byte*>(
			ALIGN_DOWN(size_t(info.pMappedAddr) + phdr.p_vaddr, phdr.p_align)
			);

		byte *fileDataPtr = reinterpret_cast<byte*>(m_moduleData->mappedMemory.get()) + phdr.p_offset;
		memcpy(info.pCodeAddr, fileDataPtr, phdr.p_filesz);

		info.pEntryPoint = info.pCodeAddr + m_moduleData->elfHeader->e_entry;
		info.pTlsAddr = info.pCodeAddr + (uint64_t)info.pTlsAddr;

		retVal = true;

	} while (false);

	return retVal;
}

bool ELFMapper::mapDataSegment(Elf64_Phdr const & phdr)
{
	bool retVal = false;
	MODULE_INFO &info = m_moduleData->moduleInfo;
	auto &fileData = m_moduleData->fileMemory;

	do
	{
		if (fileData.empty())
		{
			break;
		}

		info.nCodeSize = phdr.p_memsz;
		info.pCodeAddr = reinterpret_cast<byte*>(
			ALIGN_DOWN(size_t(info.pMappedAddr) + phdr.p_vaddr, phdr.p_align)
			);

		byte *fileDataPtr = reinterpret_cast<byte*>(m_moduleData->mappedMemory.get()) + phdr.p_offset;
		memcpy(info.pCodeAddr, fileDataPtr, phdr.p_filesz);

		info.pEntryPoint = info.pCodeAddr + m_moduleData->elfHeader->e_entry;
		info.pTlsAddr = info.pCodeAddr + (uint64_t)info.pTlsAddr;

		retVal = true;

	} while (false);

	return retVal;
}

bool ELFMapper::getModuleNameFromId(uint id, std::string * modName)
{
	bool retVal = false;
	auto &modules = m_moduleData->importModules;
	do
	{
		if (modName == nullptr)
		{
			break;
		}

		auto iter = std::find_if(modules.begin(), modules.end(),
								 [=](IMPORT_MODULE &mod) { return id == mod.id; });

		if (iter == modules.end())
		{
			break;
		}
		
		*modName = iter->strName;
		retVal = true;
	} while (false);

	return retVal;
}

bool ELFMapper::getLibraryNameFromId(uint id, std::string * libName)
{
	bool retVal = false;
	auto &libs = m_moduleData->importLibraries;

	do
	{
		if (libName == nullptr)
		{
			break;
		}

		auto iter = std::find_if(libs.begin(), libs.end(),
								 [=](IMPORT_LIBRARY &lib) {return id == lib.id; });

		if (iter == libs.end())
		{
			break;
		}
		
		*libName = iter->strName;
		retVal = true;

	} while (false);

	return retVal;
}

bool ELFMapper::decodeValue(std::string const & encodedStr, uint64_t &value)
{
	bool bRet = false;

	//the max length for an encode id is 11
	//from orbis-ld.exe
	const uint nEncLenMax = 11;
	const char pCodes[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

	do
	{

		if (encodedStr.size() > nEncLenMax)
		{
			LOG_ERR("encode id too long: %s", encodedStr.c_str());
			break;
		}

		bool bError = false;
		value = 0;

		for (int i = 0; i < encodedStr.size(); ++i)
		{
			auto pChPos = strchr(pCodes, encodedStr[i]);
			uint nIndex = 0;

			if (pChPos != nullptr)
			{
				nIndex = static_cast<uint>(pChPos - pCodes);
			}
			else
			{
				bError = true;
				break;
			}

			// NID is 64 bits long, thus we do 6 x 10 + 4 times
			if (i < nEncLenMax - 1)
			{
				value <<= 6;
				value |= nIndex;
			}
			else
			{
				value <<= 4;
				value |= (nIndex >> 2);
			}
		}

		if (bError)
		{
			break;
		}

		bRet = true;
	} while (false);
	return bRet;
}

bool ELFMapper::decodeEncodedName(std::string const &strEncName, uint *modId, uint *libId, uint64_t *funcNid)
{
	bool bRet = false;
	do
	{
		if (modId == nullptr || libId == nullptr || funcNid == nullptr)
		{
			break;
		}

		auto &nModuleId = *modId;
		auto &nLibraryId = *libId;
		auto &nNid = *funcNid;

		std::vector<std::string> vtNameParts;
		if (!UtilString::Split(strEncName, '#', vtNameParts))
		{
			break;
		}

		if (!decodeValue(vtNameParts[0], nNid))
		{
			break;
		}

		uint64 nLibId = 0;
		if (!decodeValue(vtNameParts[1], nLibId))
		{
			break;
		}
		nLibraryId = static_cast<uint>(nLibId);

		uint64 nModId = 0;
		if (!decodeValue(vtNameParts[2], nModId))
		{
			break;
		}
		nModuleId = static_cast<uint>(nModId);

		bRet = true;
	} while (false);
	return bRet;
}