#include <stdio.h>
#include <vector>
#include <string>

#include <re2/re2.h>
#include <util/util.h>
#include <util/logging.h>

// must include after re2 stuff
#include <windows.h>

struct sFile
{
	std::string name;
	unsigned int size;
};

struct sBufferFile
{
	std::string name;
	const char* ptr;
};

std::vector<std::string> filters;
std::vector<sFile> files;
std::vector<sBufferFile> bufferFiles;

char searchPath[256];
char folderPath[256];
std::vector<std::string> folders;

char* megaBuffer = NULL;
unsigned int megaBufferSize = 0;

const int DISPLAY_LINE_LENGTH = 128;
char displayLine[DISPLAY_LINE_LENGTH] = {0};

char buildFilter[256];
bool recursive = false;

const char* GetExtension(const char* filename)
{
	for (int i = strlen(filename) - 1; i >= 0; --i)
	{
		if (filename[i] == '.')
		{
			return &filename[i + 1];
		}
	}

	return NULL;
}

void BuildPackage()
{
	OSVERSIONINFO osInfo;
	osInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	GetVersionEx(&osInfo);

	// large fetch and basic info optimization is only available on windows ver 6.1 and above (win server 2008 r2 and win 7 and above)
	int largeFetchFlag = 0;
	FINDEX_INFO_LEVELS infoTypeFlag = FindExInfoStandard;
	if (osInfo.dwMajorVersion > 6 || (osInfo.dwMajorVersion == 6 && osInfo.dwMinorVersion > 0))
	{
		largeFetchFlag = FIND_FIRST_EX_LARGE_FETCH;
		infoTypeFlag = FindExInfoBasic;
	}

	// list all relevant files
	LARGE_INTEGER size;
	size.QuadPart = 0;
	int numFiles = 0;
	WIN32_FIND_DATA ffd;
	while (folders.size())
	{
		std::string currentFolder = folders.back();
		strcpy_s(searchPath, currentFolder.c_str());
		strcat_s(searchPath, "\\*.*");
		HANDLE hFind = FindFirstFileEx(searchPath, infoTypeFlag, &ffd, FindExSearchNameMatch, NULL, largeFetchFlag);
		folders.pop_back();

		if (hFind == INVALID_HANDLE_VALUE) 
		{
			printf("fex error: FindFirstFileEx failed with error %d - (%s)\n", GetLastError(), searchPath);
			continue;
		} 

		do
		{
			if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && recursive)
			{
				if (ffd.cFileName[0] != '.')
				{
					strcpy_s(folderPath, currentFolder.c_str());
					strcat_s(folderPath, "/");
					strcat_s(folderPath, ffd.cFileName);
					folders.push_back(std::string(folderPath));
				}
			}
			else
			{
				bool passed = false;
				const char* extension = GetExtension(ffd.cFileName);
				if (extension)
				{
					for (unsigned int i = 0; i < filters.size(); ++i)
					{
						if (!_stricmp(filters[i].c_str(), extension))
						{
							passed = true;
							break;
						}
					}

					if (passed)
					{
						LARGE_INTEGER filesize;
						filesize.LowPart = ffd.nFileSizeLow;
						filesize.HighPart = ffd.nFileSizeHigh;

						strcpy_s(folderPath, currentFolder.c_str());
						strcat_s(folderPath, "/");
						strcat_s(folderPath, ffd.cFileName);

						sFile newFile;
						newFile.name = std::string(folderPath);
						newFile.size = (unsigned int)filesize.QuadPart;
						files.push_back(newFile);

						numFiles++;
						size.QuadPart += filesize.QuadPart;
					}
				}
			}
		}
		while (FindNextFile(hFind, &ffd) != 0);

		FindClose(hFind);
	}

	//printf("BuildPackage: found %d files...total of %.2f Mb\n", numFiles, size.QuadPart / 1024.f / 1024.f);

	delete megaBuffer;
	megaBuffer = NULL;

	if (numFiles == 0)
	{
		return;
	}

	bufferFiles.reserve(numFiles);

	megaBufferSize = (unsigned int)size.QuadPart + (1024 * 1024); // an extra meg as safety net
	megaBuffer = new char[megaBufferSize];
	char* put = megaBuffer;
	if (!megaBuffer)
	{
		printf("fex error: out of memory!\n");
		return;
	}

	// read all files into memory...yes
	for (unsigned int i = 0; i < files.size(); ++i)
	{
		const sFile& file = files[i];
		FILE* fp = fopen(file.name.c_str(), "r");
		
		if (fread(put, 1, file.size, fp) != file.size && ferror(fp))
		{
			printf("fex warning: couldn't read from file %s (%d)\n", file.name.c_str(), ferror(fp));
			fclose(fp);
			continue;
		}

		put += file.size;
		fclose(fp);

		sBufferFile bufferFile;
		bufferFile.name = file.name;
		bufferFile.ptr = put;
		bufferFiles.push_back(bufferFile);
	}
}

void CleanUp()
{
	delete megaBuffer;
	megaBuffer = NULL;
}

void ParseFilters(const char* cFilter)
{
	char* filter = const_cast<char*>(cFilter);
	char* context = NULL;
	char* token = strtok_s(filter, " ,", &context);
	while(token)
	{
		filters.push_back(std::string(token));
		token = strtok_s(NULL, " ,", &context);
	}
}

void AddDirectory(const char* cDir)
{
	std::string dir(cDir);
	std::replace(dir.begin(), dir.end(), '/', '\\');
	if (dir.at(dir.size() - 1) == '\\')
	{
		dir = dir.substr(0, dir.size() - 1);
	}

	folders.push_back(dir);
}

void ShowHelp()
{
	printf("fex usage - github.com/flawe/fex\n\n");
	printf("fex [options] pattern\n\n");

	printf("options:\n");
	printf("\t -e\t A list of file extensions to include in the search,\n\t\t separated by space or comma.\n\t\t e.g. '-e \"c, cpp, h\"'\n\n");
	printf("\t -d\t The root directory of the search.\n\t\t e.g. '-d \"c:/my_code\"'\n\n");
	printf("\t -r\t Search recursively through sub folders.\n\n");

	printf("pattern:\n");
	printf("\t For supported patterns have a look at\n\t code.google.com/p/re2/wiki/Syntax\n");
	printf("\n");
}

void main(int argc, const char* argv[])
{
	if (argc == 1)
	{
		ShowHelp();
		return;
	}

	// parse arguments
	int i;
	for (i = 1; i < argc - 1; ++i)
	{
		if (argv[i][0] != '-' || strlen(argv[i]) != 2)
		{
			ShowHelp();
			return;
		}

		switch (argv[i][1])
		{
		case 'e':
			i++;
			ParseFilters(argv[i]);
			break;
		case 'd':
			i++;
			AddDirectory(argv[i]);
			break;
		case 'r':
			recursive = true;
			break;
		}
	}

	if (filters.size() == 0)
	{
		printf("fex: no file extensions specified\n");
		return;
	}

	if (folders.size() == 0)
	{
		printf("fex: no directories specified\n");
		return;
	}

	if (i == argc)
	{
		printf("fex: no search pattern specified\n");
		return;
	}

	// build our data
	BuildPackage();

	if (megaBuffer)
	{
		//LARGE_INTEGER start, stop;
		//QueryPerformanceCounter(&start);

		RE2 mainSearch(argv[argc - 1]);
		RE2 newLineSearch("\n");

		re2::StringPiece newLineString;
		re2::StringPiece megaString(megaBuffer, megaBufferSize);
		int num = 0;
		int lastFound = 0;
		const char* lastNewLine = NULL;
		int lastNumNewLines = 0;
		while (RE2::FindAndConsume(&megaString, mainSearch))
		{
			// identify which file the search hit was in
			for (unsigned int i = lastFound; i < bufferFiles.size(); ++i)
			{
				if (megaString.data() <= bufferFiles[i].ptr)
				{
					// do a regexp search to find all newlines for this file up to the point of the hit
					// if previous search had a hit on the same file, use the cached numNewLines and ptr so
					// we don't have to search through entire file again
					const char* beg = NULL;
					int numNewLines = 0;
					if (i == lastFound && lastNewLine != NULL)
					{
						beg = lastNewLine;
						numNewLines += lastNumNewLines;
					}
					else
					{
						beg = i == 0 ? megaBuffer : bufferFiles[i - 1].ptr;
					}

					const char* end = megaString.data();
					newLineString.set(beg, end - beg);
					
					int lastConsumed = 0;
					int consumed = 0;
					while (consumed = RE2::FindAndConsume(&newLineString, newLineSearch))
					{
						lastConsumed = consumed;
						numNewLines++;
					}
					lastNumNewLines = numNewLines;
					lastNewLine = newLineString.data();

					// copy DISPLAY_LINE_LENGTH characters to a separate buffer so we can use it as a marker for the match
					// break on newline
					for (int c = 0; c < DISPLAY_LINE_LENGTH; ++c)
					{
						if (lastNewLine[c] != '\n')
						{
							displayLine[c] = lastNewLine[c];
						}
						else
						{
							displayLine[c] = '\0';
							break;
						}
					}
					printf("%s:%d:%s\n", bufferFiles[i].name.c_str(), numNewLines + 1, displayLine);
					lastFound = i;
					break;
				}
			}
			num++;
		}

		//QueryPerformanceCounter(&stop);
		//printf("fex: done in: %.2f seconds, found %d instances\n", (stop.QuadPart - start.QuadPart) / 1000000.f, num);
	}

	CleanUp();
}
