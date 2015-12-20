/*
 *  This file is part of nzbget
 *
 *  Copyright (C) 2015 Andrey Prygunkov <hugbug@users.sourceforge.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * $Revision$
 * $Date$
 *
 */


#include "nzbget.h"
#include "DupeMatcher.h"
#include "Log.h"
#include "Util.h"
#include "Options.h"
#include "Script.h"
#include "Thread.h"

class RarLister : public Thread, public ScriptController
{
private:
	DupeMatcher*		m_owner;
	int64				m_maxSize;
	bool				m_compressed;
	bool				m_lastSizeMax;
	int64				m_expectedSize;
	char*				m_filenameBuf;
	int					m_filenameBufLen;
	BString<1024>		m_lastFilename;

protected:
	virtual void		AddMessage(Message::EKind kind, const char* text);

public:
	virtual void		Run();
	static bool			FindLargestFile(DupeMatcher* owner, const char* directory,
							char* filenameBuf, int filenameBufLen, int64 expectedSize,
							int timeoutSec, int64* maxSize, bool* compressed);
};

bool RarLister::FindLargestFile(DupeMatcher* owner, const char* directory,
	char* filenameBuf, int filenameBufLen, int64 expectedSize,
	int timeoutSec, int64* maxSize, bool* compressed)
{
	RarLister unrar;
	unrar.m_owner = owner;
	unrar.m_expectedSize = expectedSize;
	unrar.m_maxSize = -1;
	unrar.m_compressed = false;
	unrar.m_lastSizeMax = false;
	unrar.m_filenameBuf = filenameBuf;
	unrar.m_filenameBufLen = filenameBufLen;

	char** cmdArgs = NULL;
	if (!Util::SplitCommandLine(g_Options->GetUnrarCmd(), &cmdArgs))
	{
		return false;
	}
	const char* unrarPath = *cmdArgs;
	unrar.SetScript(unrarPath);

	const char* args[4];
	args[0] = unrarPath;
	args[1] = "lt";
	args[2] = "*.rar";
	args[3] = NULL;
	unrar.SetArgs(args, false);
	unrar.SetWorkingDir(directory);

	time_t curTime = time(NULL);

	unrar.Start();

	// wait up to iTimeoutSec for unrar output
	while (unrar.IsRunning() &&
		curTime + timeoutSec > time(NULL) &&
		curTime >= time(NULL))					// in a case clock was changed
	{
		usleep(200 * 1000);
	}

	if (unrar.IsRunning())
	{
		unrar.Terminate();
	}

	// wait until terminated or killed
	while (unrar.IsRunning())
	{
		usleep(200 * 1000);
	}

	for (char** argPtr = cmdArgs; *argPtr; argPtr++)
	{
		free(*argPtr);
	}
	free(cmdArgs);

	*maxSize = unrar.m_maxSize;
	*compressed = unrar.m_compressed;

	return true;
}

void RarLister::Run()
{
	Execute();
}

void RarLister::AddMessage(Message::EKind kind, const char* text)
{
	if (!strncasecmp(text, "Archive: ", 9))
	{
		m_owner->PrintMessage(Message::mkDetail, "Reading file %s", text + 9);
	}
	else if (!strncasecmp(text, "        Name: ", 14))
	{
		m_lastFilename = text + 14;
	}
	else if (!strncasecmp(text, "        Size: ", 14))
	{
		m_lastSizeMax = false;
		int64 size = atoll(text + 14);
		if (size > m_maxSize)
		{
			m_maxSize = size;
			m_lastSizeMax = true;
			strncpy(m_filenameBuf, m_lastFilename, m_filenameBufLen);
			m_filenameBuf[m_filenameBufLen-1] = '\0';
		}
		return;
	}

	if (m_lastSizeMax && !strncasecmp(text, " Compression: ", 14))
	{
		m_compressed = !strstr(text, " -m0");
		if (m_maxSize > m_expectedSize ||
			DupeMatcher::SizeDiffOK(m_maxSize, m_expectedSize, 20))
		{
			// alread found the largest file, aborting unrar
			Terminate();
		}
	}
}


DupeMatcher::DupeMatcher(const char* destDir, int64 expectedSize)
{
	m_destDir = destDir;
	m_expectedSize = expectedSize;
	m_maxSize = -1;
	m_compressed = false;
}

bool DupeMatcher::SizeDiffOK(int64 size1, int64 size2, int maxDiffPercent)
{
	if (size1 == 0 || size2 == 0)
	{
		return false;
	}

	int64 diff = size1 - size2;
	diff = diff > 0 ? diff : -diff;
	int64 max = size1 > size2 ? size1 : size2;
	int diffPercent = (int)(diff * 100 / max);
	return diffPercent < maxDiffPercent;
}

bool DupeMatcher::Prepare()
{
	char filename[1024];
	FindLargestFile(m_destDir, filename, sizeof(filename), &m_maxSize, &m_compressed);
	bool sizeOK = SizeDiffOK(m_maxSize, m_expectedSize, 20);
	PrintMessage(Message::mkDetail, "Found main file %s with size %lli bytes%s",
		filename, m_maxSize, sizeOK ? "" : ", size mismatch");
	return sizeOK;
}

bool DupeMatcher::MatchDupeContent(const char* dupeDir)
{
	int64 dupeMaxSize = 0;
	bool dupeCompressed = false;
	char filename[1024];
	FindLargestFile(dupeDir, filename, sizeof(filename), &dupeMaxSize, &dupeCompressed);
	bool ok = dupeMaxSize == m_maxSize && dupeCompressed == m_compressed;
	PrintMessage(Message::mkDetail, "Found main file %s with size %lli bytes%s",
		filename, m_maxSize, ok ? "" : ", size mismatch");
	return ok;
}

void DupeMatcher::FindLargestFile(const char* directory, char* filenameBuf, int bufLen,
	int64* maxSize, bool* compressed)
{
	*maxSize = 0;
	*compressed = false;

	DirBrowser dir(directory);
	while (const char* filename = dir.Next())
	{
		if (strcmp(filename, ".") && strcmp(filename, ".."))
		{
			BString<1024> fullFilename("%s%c%s", directory, PATH_SEPARATOR, filename);

			int64 fileSize = Util::FileSize(fullFilename);
			if (fileSize > *maxSize)
			{
				*maxSize = fileSize;
				strncpy(filenameBuf, filename, bufLen);
				filenameBuf[bufLen-1] = '\0';
			}

			if (Util::MatchFileExt(filename, ".rar", ","))
			{
				RarLister::FindLargestFile(this, directory, filenameBuf, bufLen,
					m_maxSize, 60, maxSize, compressed);
				return;
			}
		}
	}
}
