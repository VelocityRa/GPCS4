#include "UtilPath.h"
#include "UtilString.h"
#include <algorithm>

namespace UtilPath
{;


#ifdef GPCS4_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef WIN32_LEAN_AND_MEAN


std::string GetExePath()
{
	std::string strExePath;
	do 
	{
		HMODULE hModule = GetModuleHandleW(NULL);
		if (!hModule)
		{
			break;
		}
		CHAR szPath[MAX_PATH];
		if (!GetModuleFileNameA(hModule, szPath, MAX_PATH))
		{
			break;
		}
		strExePath.assign(szPath);
		strExePath = strExePath.substr(0, strExePath.find_last_of('\\') + 1);

	} while (false);
	return strExePath;
}

std::string PS4PathToPCPath(const std::string& strPs4Path)
{
	std::string strPcPath = strPs4Path;
	std::replace(strPcPath.begin(), strPcPath.end(), '/', '\\');
	std::string strExePath = GetExePath();
	strPcPath = UtilString::ReplaceAll(strPcPath, "\\app0\\", strExePath);
	return strPcPath;
}


size_t FileCountInDirectory(const std::string& path)
{
	int counter = 0;
	WIN32_FIND_DATA ffd;
	HANDLE hFind = INVALID_HANDLE_VALUE;
	std::string dirPath = path;
	if (dirPath.at(dirPath.length() - 1) != '\\' &&
		dirPath.at(dirPath.length() - 1) != '/')
	{
		dirPath.append("\\");
	}
	dirPath.append("*");

	// Start iterating over the files in the path directory.
	hFind = FindFirstFileA(dirPath.c_str(), &ffd);
	if (hFind != INVALID_HANDLE_VALUE)
	{
		do // Managed to locate and create an handle to that folder.
		{
			counter++;
		} while (FindNextFileA(hFind, &ffd) == TRUE);
		FindClose(hFind);
	}
	else {
		LOG_ERR("Failed to find path: %s", dirPath.c_str());
	}

	return counter;
}


#else


#endif  //GPCS4_WINDOWS



}