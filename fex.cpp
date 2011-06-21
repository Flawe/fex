#include <stdio.h>
#include <vector>
#include <string>

#include <re2/re2.h>
#include <util/util.h>
#include <util/logging.h>

#include "zlib/zlib.h"

// must include after re2 stuff
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#define SET_BINARY_MODE(file) _setmode(_fileno(file), O_BINARY)

struct sFile
{
	std::string name;
	unsigned int size;
};

struct sBufferFile
{
	char name[256];
	unsigned int ptrOffset;
};

std::vector<std::string> filters;
std::vector<sFile> files;

char searchPath[256];
char folderPath[256];
char cachePath[256];
std::vector<std::string> folders;

char* megaBuffer = NULL;
unsigned int megaBufferSize = 0;

unsigned int numBufferFiles = 0;
sBufferFile* bufferFiles = NULL;

char* globalBuffer = NULL;
unsigned int globalBufferSize = 0;

const int DISPLAY_LINE_LENGTH = 128;
char displayLine[DISPLAY_LINE_LENGTH] = {0};

char buildFilter[256];
bool recursive = false;
bool cache = false;
bool recache = false;

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

void CleanUp()
{
	delete globalBuffer;
	globalBuffer = NULL;
	globalBufferSize = 0;
	megaBuffer = NULL;
	bufferFiles = NULL;
	numBufferFiles = 0;
}

void BuildPackage()
{
	bool foundCache = false;

	std::string currentFolder = folders.back();
	strcpy_s(cachePath, currentFolder.c_str());
	strcat_s(cachePath, "\\.fex_cache");

	if (cache && !recache)
	{
		// TODO: handle several directories

		FILE* fp = fopen(cachePath, "r");
		if (fp)
		{
			SET_BINARY_MODE(fp);

			fseek(fp, 0L, SEEK_END);
			int size = ftell(fp);
			fseek(fp, 0L, SEEK_SET);

			char* inBuffer = new char[size];
			if (fread(inBuffer, 1, size, fp) != size && ferror(fp))
			{
				printf("fex warning: couldn't read from file %s (%d)\n", cachePath, ferror(fp));
				fclose(fp);
				delete inBuffer;
				goto fallback;
			}
			fclose(fp);
			
			z_stream stream;
			stream.zalloc = Z_NULL;
			stream.zfree = Z_NULL;
			stream.opaque = Z_NULL;
			stream.avail_in = size - sizeof(int);
			stream.next_in = (Bytef*)(inBuffer + sizeof(int));

			int ret = inflateInit(&stream);
			if (ret != Z_OK)
			{
				printf("fex warning: couldn't init zlib for uncompression (%d)\n", ret);
				delete inBuffer;
				goto fallback;
			}

			globalBufferSize = *(int*)(inBuffer);
			globalBuffer = new char[globalBufferSize];

			stream.avail_out = globalBufferSize;
			stream.next_out = (Bytef*)globalBuffer;
			ret = inflate(&stream, Z_FINISH);
			if (ret != Z_STREAM_END)
			{
				printf("fex warning: couldn't uncompress cache (%d)\n", ret);
				delete inBuffer;
				delete globalBuffer;
				megaBufferSize = 0;
				goto fallback;
			}

			// fixup pointers
			numBufferFiles = *(int*)globalBuffer;							// first word is number of files in bufferFiles
			bufferFiles = (sBufferFile*)(globalBuffer + sizeof(int));		// next chunk is the bufferFiles buffer
			megaBuffer = (char*)(bufferFiles + numBufferFiles);				// the rest is the mega buffer with the text
			megaBufferSize = globalBufferSize - (sizeof(int) + sizeof(sBufferFile) * numBufferFiles);

			inflateEnd(&stream);
			delete inBuffer;
			foundCache = true;
		}
	}

	if (foundCache)
	{
		return;
	}

fallback:
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

	if (numFiles == 0)
	{
		return;
	}

	CleanUp();

	megaBufferSize = (unsigned int)size.QuadPart + (1024 * 1024); // an extra meg as safety net

	globalBufferSize = sizeof(sBufferFile) * numFiles;
	globalBufferSize += megaBufferSize;

	// allocate global buffer, store sizes and fixup pointers
	globalBuffer = new char[globalBufferSize];
	*(int*)globalBuffer = numFiles;									// first word is number of files in bufferFiles
	bufferFiles = (sBufferFile*)(globalBuffer + sizeof(int));		// next chunk is the bufferFiles buffer
	megaBuffer = (char*)(bufferFiles + numFiles);					// the rest is the mega buffer with the text

	char* put = megaBuffer;
	if (!globalBuffer)
	{
		printf("fex error: out of memory!\n");
		return;
	}

	// read all files into memory...yes
	unsigned int bfIdx = 0;
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

		sprintf_s(bufferFiles[bfIdx].name, "%s", file.name.c_str());
		bufferFiles[bfIdx].ptrOffset = put - megaBuffer;
		bfIdx++;
	}
	numBufferFiles = bfIdx;

	if (cache || recache)
	{
		// TODO: handle several directories

		FILE* fp = fopen(cachePath, "w");
		if (fp)
		{
			SET_BINARY_MODE(fp);
			
			z_stream stream;
			stream.zalloc = Z_NULL;
			stream.zfree = Z_NULL;
			stream.opaque = Z_NULL;

			int ret = deflateInit(&stream, Z_DEFAULT_COMPRESSION);
			if (ret != Z_OK)
			{
				printf("fex warning: couldn't init zlib for compression (%d)\n", ret);
				return;
			}

			char* outBuffer = new char[globalBufferSize];

			stream.avail_in = globalBufferSize;
			stream.next_in = (Bytef*)globalBuffer;
			stream.avail_out = globalBufferSize - sizeof(int);
			stream.next_out = (Bytef*)(outBuffer + sizeof(int));
			ret = deflate(&stream, Z_FINISH);
			if (ret != Z_STREAM_END)
			{
				printf("fex warning: couldn't decompress cache (%d)\n", ret);
				delete outBuffer;
				return;
			}
			deflateEnd(&stream);

			*(int*)outBuffer = globalBufferSize;

			if (fwrite(outBuffer, 1, stream.total_out + sizeof(int), fp) != stream.total_out + sizeof(int) && ferror(fp))
			{
				printf("fex warning: couldn't write to file %s (%d)\n", searchPath, ferror(fp));
				delete outBuffer;
			}
			fclose(fp);
		}
	}
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
	printf("\t -c\t Use the cache for all searches. If no cache is found, create on in the search directory.\n\n");
	printf("\t -C\t Re-cache all found source files in the search dir. Use this flag to renew the cache.\n\n");

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
		case 'c':
			cache = true;
			break;
		case 'C':
			recache = true;
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

	//LARGE_INTEGER start1, stop1, start2, stop2;
	//QueryPerformanceCounter(&start1);

	// build our data
	BuildPackage();

	//QueryPerformanceCounter(&stop1);

	if (megaBuffer)
	{
		//QueryPerformanceCounter(&start2);

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
			for (unsigned int i = lastFound; i < numBufferFiles; ++i)
			{
				if (megaString.data() <= megaBuffer + bufferFiles[i].ptrOffset)
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
						beg = i == 0 ? megaBuffer : megaBuffer + bufferFiles[i - 1].ptrOffset;
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
					printf("%s:%d:%s\n", bufferFiles[i].name, numNewLines + 1, displayLine);
					lastFound = i;
					break;
				}
			}
			num++;
		}

		//QueryPerformanceCounter(&stop2);
		//printf("fex: data accumulation done in: %.2f seconds, %d files searched\n", (stop1.QuadPart - start1.QuadPart) / 1000000.f, numBufferFiles);
		//printf("fex: regexp match done in: %.2f seconds, found %d instances\n", (stop2.QuadPart - start2.QuadPart) / 1000000.f, num);
	}

	CleanUp();
}
