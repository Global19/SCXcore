/*------------------------------------------------------------------------------
    Copyright (c) Microsoft Corporation. All rights reserved. See license.txt for license information.
    
*/
/**
    \file      logfileutils.cpp

    \brief     Main implementation file for Log File Provider

    \date      2008-0-08 09:35:36
*/
/*----------------------------------------------------------------------------*/

#include <scxcorelib/scxcmn.h>

#include <errno.h>
#include <fstream>
#include <iostream>
#include <locale>

#include <scxcorelib/scxexception.h>
#include <scxcorelib/scxdirectoryinfo.h>
#include <scxcorelib/scxlog.h>
#include <scxcorelib/scxmath.h>
#include <scxcorelib/scxuser.h>
#include <scxcorelib/stringaid.h>
#include <scxcorelib/scxstream.h>
#include <scxcorelib/scxregex.h>
#include <scxcorelib/scxfile.h>

#include "logfileutils.h"

using namespace SCXCoreLib;
using namespace std;

namespace SCXCore {
    const SCXCoreLib::SCXPatternFinder::SCXPatternCookie LogFileReader::s_patternID = 1;
    const std::wstring LogFileReader::s_pattern = L"SELECT * FROM SCX_LogFileRecord WHERE FileName=%PATH";
    const std::wstring LogFileReader::s_patternParameter = L"PATH";
    const unsigned int cMaxMatchedRows = 500;   //!< max number of matched log rows return limit, 1000 rows from scx log file does not work, 750 does
    const size_t cMaxTotalBytes = 60 * 1024;    //!< max number of bytes to return in a single instance

    /*----------------------------------------------------------------------------*/
    /* LogFileReader::LogFilePositionRecord                                     */
    /*----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------*/
    /**
        Constructor
        Creates a new LogFilePositionRecord with default values.

        \param[in] logfile Log file for this record.
        \param[in] qid Q ID of this record.
        \param[in] persistMedia Used to inject persistence media to use for persisting this record. 
    */
    LogFileReader::LogFilePositionRecord::LogFilePositionRecord(
        const SCXCoreLib::SCXFilePath& logfile,
        const std::wstring& qid,
        SCXCoreLib::SCXHandle<SCXCoreLib::SCXPersistMedia> persistMedia /* =  SCXCoreLib::GetPersistMedia()*/)
        : m_PersistMedia(persistMedia),
          m_LogFile(logfile),
          m_Qid(qid),
          m_ResetOnRead(false),
          m_Pos(0),
          m_StIno(0),
          m_StSize(0)
    {
        SCXUser user;
        m_IdString = L"LogFileProvider_" + user.GetName() + logfile.Get() + qid;
    }

    /*----------------------------------------------------------------------------*/
    /**
        Get the log file associated with this record.
        \returns File path to log file.
    */
    const SCXFilePath& LogFileReader::LogFilePositionRecord::GetLogFile() const
    {
        return m_LogFile;
    }

    /*----------------------------------------------------------------------------*/
    /**
        Get current "ResetOnRead" flag for the logfile and qid.
        \returns Current reset-on-read flag.
    */
    bool LogFileReader::LogFilePositionRecord::GetResetOnRead() const
    {
        return m_ResetOnRead;
    }

    /*----------------------------------------------------------------------------*/
    /**
        Set current "ResetOnRead" flag for the logfile and qid.
        \param[in] fSet New reset-on-read flag.
    */
    void LogFileReader::LogFilePositionRecord::SetResetOnRead(bool fSet)
    {
        m_ResetOnRead = fSet;
    }

    /*----------------------------------------------------------------------------*/
    /**
        Get current file position for the logfile and qid.
        \returns Current file position.
    */
    std::streamoff LogFileReader::LogFilePositionRecord::GetPos() const
    {
        return m_Pos;
    }

    /*----------------------------------------------------------------------------*/
    /**
        Set current file position for the logfile and qid.
        \param[in] pos Current file position.
    */
    void LogFileReader::LogFilePositionRecord::SetPos(std::streamoff pos)
    {
        m_Pos = pos;
    }

    /*----------------------------------------------------------------------------*/
    /**
        Get the st_ino filed of a stat struct for this file.
        \returns st_ino field of a stat structure.
    */
    scxulong LogFileReader::LogFilePositionRecord::GetStatStIno() const
    {
        return m_StIno;
    }

    /*----------------------------------------------------------------------------*/
    /**
        Set the st_ino filed of a stat struct for this file.
        \param[in] st_ino st_ino field of a stat structure.
    */
    void LogFileReader::LogFilePositionRecord::SetStatStIno(scxulong st_ino)
    {
        m_StIno = st_ino;
    }

    /*----------------------------------------------------------------------------*/
    /**
        Get the st_size filed of a stat struct for this file.
        \returns st_size field of a stat structure.
    */
    scxulong LogFileReader::LogFilePositionRecord::GetStatStSize() const
    {
        return m_StSize;
    }

    /*----------------------------------------------------------------------------*/
    /**
        Set the st_size filed of a stat struct for this file.
        \param[in] st_size st_size field of a stat structure.
    */
    void LogFileReader::LogFilePositionRecord::SetStatStSize(scxulong st_size)
    {
        m_StSize = st_size;
    }

    /*----------------------------------------------------------------------------*/
    /**
        Persist data
    */
    void LogFileReader::LogFilePositionRecord::Persist()
    {
        if (static_cast<scxulong>(m_Pos) > m_StSize)
        {
            m_StSize = static_cast<scxulong>(m_Pos);
        }
        SCXHandle<SCXPersistDataWriter> pwriter = m_PersistMedia->CreateWriter(m_IdString, 1);
        pwriter->WriteValue(L"Filename", SCXCoreLib::StrFrom(m_LogFile.Get()));
        pwriter->WriteValue(L"QID", SCXCoreLib::StrFrom(m_Qid));
        pwriter->WriteValue(L"Reset", SCXCoreLib::StrFrom(m_ResetOnRead));
        pwriter->WriteValue(L"Pos", SCXCoreLib::StrFrom(m_Pos));
        pwriter->WriteStartGroup(L"Stat");
        pwriter->WriteValue(L"StIno", SCXCoreLib::StrFrom(m_StIno));
        pwriter->WriteValue(L"StSize", SCXCoreLib::StrFrom(m_StSize));
        pwriter->WriteEndGroup();
        pwriter->DoneWriting();
    }

    /*----------------------------------------------------------------------------*/
    /**
        Recover persisted data
        \returns false if no data had previously been persisted.
    */
    bool LogFileReader::LogFilePositionRecord::Recover()
    {
        try
        {
            SCXHandle<SCXPersistDataReader> preader = m_PersistMedia->CreateReader(m_IdString);
            int version = preader->GetVersion();
            if (0 != version && 1 != version)
            {
                // Wrong version. Just ignore. It will be re-persisted later.
                return false;
            }

            // Version 0 does not include Filename, QID, or Reset; Version 1 does
            // By being version-aware, we always recover properly

            if (version >= 1)
            {
                // We already have filename, but consume to avoid errors
                preader->ConsumeValue(L"Filename");
                m_Qid = preader->ConsumeValue(L"QID");
                m_ResetOnRead = SCXCoreLib::StrToLong(preader->ConsumeValue(L"Reset"));
            }

            m_Pos = SCXCoreLib::StrToULong(preader->ConsumeValue(L"Pos"));
            preader->ConsumeStartGroup(L"Stat");
            m_StIno = SCXCoreLib::StrToULong(preader->ConsumeValue(L"StIno"));
            m_StSize = SCXCoreLib::StrToULong(preader->ConsumeValue(L"StSize"));
            preader->ConsumeEndGroup();
            return true;
        }
        catch (SCXNotSupportedException&)
        {
            // Data is corrupt (a value could not be parsed). Just ignore. It will be re-persisted later.
            return false;
        }
        catch (PersistUnexpectedDataException&)
        {
            // Data is corrupt. Just ignore. It will be re-persisted later.
            return false;
        }
        catch (PersistDataNotFoundException&)
        {
            // No persisted data found.
            return false;
        }
    }

    /*----------------------------------------------------------------------------*/
    /**
        Remove persisted data
        \returns false if no data had previously been persisted.
    */
    bool LogFileReader::LogFilePositionRecord::UnPersist()
    {
        try
        {
            m_PersistMedia->UnPersist(m_IdString);
        }
        catch (PersistDataNotFoundException&)
        {
            // No persisted data found.
            return false;
        }
        return true;
    }

    /*----------------------------------------------------------------------------*/
    /* LogFileReader::LogFileStreamPositioner                                   */
    /*----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------*/
    /**
        Constructor

        \param[in] logfile Log file for this record.
        \param[in] qid Q ID of this record.
        \param[in] persistMedia Used to inject persistence media to use for persisting this record. 
        \throws SCXFilePathNotFoundException if log file does not exist.
    */
    LogFileReader::LogFileStreamPositioner::LogFileStreamPositioner(
        const SCXCoreLib::SCXFilePath& logfile,
        const std::wstring& qid,
        SCXCoreLib::SCXHandle<SCXCoreLib::SCXPersistMedia> persistMedia /* =  SCXCoreLib::GetPersistMedia()*/)
        : m_Record(0),
          m_Stream(0),
          m_log(SCXLogHandleFactory::GetLogHandle(L"scx.core.providers.logfileprovider.logfilestreampositioner"))
    {
        m_Record = new LogFilePositionRecord(logfile, qid, persistMedia);
        m_Stream = SCXFile::OpenWFstream(logfile, std::ios_base::in);

        // Set the locale on the stream to the system locale (based on environment variables)
        // Note: This overrides SCXLocale settings (which is used for everything else)
        //
        // If we get an exception, just let it fly (hopefully things will still work okay)
        // This is better than dying because some bizarre locale is set

        try {
            std::locale newLocale("");
            m_Stream->imbue(newLocale);
        }
        catch (...)
        {
        }

        // On all platforms (even Linux), tellg() can return -1 (see test case testTellgBehavior() in unit test).
        // To try and protect against that, we position to EOF and save the position, and we'll use that position
        // if tellg() returns -1.
        //
        // There's a race condition here were we can return the same lines multiple times depending on concurrency
        // issues.  And there's a WI for that, wi 16772.

        m_Stream->seekg(0, std::ios_base::end);
        std::streamoff pos = m_Stream->tellg();

        if ( ! m_Record->Recover() )
        {
            // First time (i.e when no persisted data exists) we just go to end of file.
            SCX_LOGTRACE(m_log, L"OpenStream " + m_Record->GetLogFile().Get() + L" - First time - Seek to end");
            SCX_LOGTRACE(m_log, StrAppend(L"LogFileProvider OpenStream last pos = ", pos));
        }
        else
        {
            if ( m_Record->GetResetOnRead() )
            {
                // We were requested to reset on the next read; now's the time
                SCX_LOGTRACE(m_log, L"OpenStream " + m_Record->GetLogFile().Get() + L" - ResetOnRead - Seek to end");
                SCX_LOGTRACE(m_log, StrAppend(L"LogFileProvider OpenStream last pos = ", pos));
                m_Record->SetResetOnRead(false);
            }
            else if ( ! IsFileNew() )
            {
                // File has not wrapped so we seek to last position.
                SCX_LOGTRACE(m_log, StrAppend(L"LogFileProvider OpenLogFile " + m_Record->GetLogFile().Get()
                                              + L"- Seek to: ", m_Record->GetPos()));
                SCXFile::SeekG(*m_Stream, m_Record->GetPos());
            }
            else
            {
                // File has wrapped so we find new last position.
                SCX_LOGTRACE(m_log, L"LogFileProvider OpenLogFile " + m_Record->GetLogFile().Get() + L" - File has wrapped");
                SCXFile::SeekG(*m_Stream, 0);
                SCX_LOGTRACE(m_log, StrAppend(L"LogFileProvider OpenStream save last pos = ", pos));
            }
        }

        m_Record->SetPos(pos);
        UpdateStatData();
    }

    /*----------------------------------------------------------------------------*/
    /**
        Return a stream pointing at the correct reading position.

        \returns       Handle to stream opened at the correct position.
    */
    SCXHandle<std::wfstream> LogFileReader::LogFileStreamPositioner::GetStream()
    {
        return m_Stream;
    }

    /*----------------------------------------------------------------------------*/
    /**
        Save the state of a logfile.
    */
    void LogFileReader::LogFileStreamPositioner::PersistState()
    {
        std::streamoff pos = m_Stream->tellg();
        SCX_LOGTRACE(m_log, StrAppend(L"LogFileProvider PersistState() - pos = ", pos));

        // Never persist -1, happens on some platforms (AIX) when reading past end of file.  It can
        // even happen on Linux platforms in certain cases (see unit test testTellgBehavior()).
        //
        // Real end of file position has been saved when opening the file.  If we can't get the true
        // current location, we fall back to the size of the file at open() time.
        //
        // Note: We could use the size from SCXFileSystem::Stat(), but this is non-atomic. Safer to
        // use the actual file size from the file at the time that we've opened it.

        if (pos > 0)
        {
            m_Record->SetPos(pos);
        }
        m_Record->Persist();
    }

    /*----------------------------------------------------------------------------*/
    /**
        Check if the log file is actually a new file with the same name.
        This will happen if the file has wrapped since last update for example.

        \returns true if the file is actually a new file.
        \throws SCXFilePathNotFoundException if log file does not exist.
    */
    bool LogFileReader::LogFileStreamPositioner::IsFileNew() const
    {
        SCXFileSystem::SCXStatStruct statstruct;
        SCXFileSystem::Stat(m_Record->GetLogFile(), &statstruct);
         
        // If inode has changed it is a new file
        if (statstruct.st_ino != m_Record->GetStatStIno())
        {
            SCX_LOGTRACE(m_log, L"IsNewFile - inode changed - new file");
            return true;
        }

        // If the new size is smaller it is a new file
        if ((scxulong) statstruct.st_size < m_Record->GetStatStSize())
        {
            SCX_LOGTRACE(m_log, L"IsNewFile - size smaller - new file");
            return true;
        }

        SCX_LOGTRACE(m_log, L"IsNewFile - not new file");

        return false;
    }

    /*----------------------------------------------------------------------------*/
    /**
        Update stat fields in record with values read from disk.
        \throws SCXFilePathNotFoundException if log file does not exist.
    */
    void LogFileReader::LogFileStreamPositioner::UpdateStatData()
    {
        SCXFileSystem::SCXStatStruct statstruct;
        SCXFileSystem::Stat(m_Record->GetLogFile(), &statstruct);
         
        m_Record->SetStatStIno(statstruct.st_ino);
        m_Record->SetStatStSize(statstruct.st_size);
    }

    /*----------------------------------------------------------------------------*/
    /* LogFileReader::LogFileReader                                               */
    /*----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------*/
    /**
        Constructor
        Creates a new LogFileReader class
    */
    LogFileReader::LogFileReader()
        : m_log(SCXLogHandleFactory::GetLogHandle(L"scx.core.providers.logfileprovider"))
    {
        m_persistMedia = GetPersistMedia();
        m_cqlPatterns.RegisterPattern(s_patternID, s_pattern);
    }

    /*----------------------------------------------------------------------------*/
    /**
       Get filename from query

       \param[in]     query        The CQL/WQL query to parse

       \returns       Log file name

       \throws        SCXNotSupportedException   If query not on recognized format
       \throws        SCXInternalErrorException  If an unexpected pattern was matched.

    */
    std::wstring LogFileReader::GetFileName(const std::wstring& query)
    {
        SCXCoreLib::SCXPatternFinder::SCXPatternCookie id = 0;
        SCXCoreLib::SCXPatternFinder::SCXPatternMatch param;
        if ( ! m_cqlPatterns.Match(query, id, param))
        {
            throw SCXNotSupportedException(L"LogFileProvider Query not on format: " + s_pattern, SCXSRCLOCATION);
        }
        if (id != s_patternID || param.end() == param.find(s_patternParameter))
        {
            throw SCXInternalErrorException(L"Wrong pattern matched!", SCXSRCLOCATION);
        }
        SCX_LOGTRACE(m_log, L"LogFileProvider GetFileName: " + param.find(s_patternParameter)->second);

        return param.find(s_patternParameter)->second;
    }

    /*----------------------------------------------------------------------------*/
    /**
       Get SCXLogFile class for a given log file name

       \param[in]     filename        Name of log file

       \returns       Pointer to corresponding SCXLogFile class or NULL if none found

    */
    LogFileReader::SCXLogFile* LogFileReader::GetLogFile(const std::wstring& filename)
    {
        for(size_t i=0; i<m_files.size(); i++)
        {
            if (m_files[i].name == filename)
            {
                SCX_LOGTRACE(m_log, StrAppend(L"LogFileProvider GetLogFile found filename - ", filename));
                return &m_files[i];
            }
        }

        SCX_LOGTRACE(m_log, StrAppend(L"LogFileProvider GetLogFile did NOT find filename - ", filename));
        return NULL;
    }


    /*----------------------------------------------------------------------------*/
    /**
       Check if the log file has wrapped and we need to start reading from line 1 again

       \param[in]     oldstatinfo     Old saved stat info
       \param[in]     newstatinfo     New stat info

       \returns       true if file has wrapped, otherwise false

    */
    bool LogFileReader::CheckFileWrap(const struct stat64& oldstatinfo, const struct stat64& newstatinfo)
    {
        // If inode has changed it is a new file
        if (oldstatinfo.st_ino != newstatinfo.st_ino)
        {
            SCX_LOGTRACE(m_log, L"LogFileProvider CheckFileWrap - inode changed - new file");
            return true;
        }
        
        // If the new size is smaller it is a new file
        if (oldstatinfo.st_size > newstatinfo.st_size)
        {
            SCX_LOGTRACE(m_log, L"LogFileProvider CheckFileWrap - size smaller - new file");
            return true;
        }

        SCX_LOGTRACE(m_log, L"LogFileProvider CheckFileWrap - inode not changed and size not smaller - not new file");
        return false;
    }

    /*----------------------------------------------------------------------------*/
    /**
        Set persist media to use.
        Only used for tests

        \param[in]     persistMedia  persistance media to use
    */
    void LogFileReader::SetPersistMedia(SCXCoreLib::SCXHandle<SCXCoreLib::SCXPersistMedia> persistMedia) 
    { 
        m_persistMedia = persistMedia; 
    }

    bool LogFileReader::ReadLogFile(
        const std::wstring& filename,
        const std::wstring& qid,
        const std::vector<SCXRegexWithIndex>& regexps,
        std::vector<std::wstring>& matchedLines)
    {
        LogFileStreamPositioner positioner(filename, qid, m_persistMedia);
        SCXHandle<std::wfstream> logfile = positioner.GetStream();

        bool partialRead = false;

        unsigned int rows = 0;
        unsigned int matched_rows = 0;
        size_t total_bytes = 0;

        // Read rows from log file
        while ((matched_rows < cMaxMatchedRows && total_bytes < cMaxTotalBytes)
               && SCXStream::IsGood(*logfile))
        {
            wstring line;
            SCXStream::NLF nlf;

            rows++;

            SCX_LOGHYSTERICAL(m_log, StrAppend(L"LogFileProvider DoInvokeMethod - Reading row: ", rows));

            SCXStream::ReadLine(*logfile, line, nlf);

            // Check line against regular expressions and add to result if any matches
            std::wstring res(L"");
            int matches = 0;

            for (size_t j=0; j<regexps.size(); j++)
            {
                if (regexps[j].regex->IsMatch(line))
                {
                    SCX_LOGHYSTERICAL(m_log, StrAppend(StrAppend(StrAppend(L"LogFileProvider DoInvokeMethod - row: ", rows), 
                                                                 L" Matched regexp: "), regexps[j].index));
                    matches++;
                    res = StrAppend(StrAppend(res, res.length()>0?L" ":L""), regexps[j].index);
                }
            }

            if (matches > 0)
            {
                wstring retEntry = StrAppend(StrAppend(res, L";"), line);
                matchedLines.push_back(retEntry);
                matched_rows++;
                total_bytes += retEntry.size();
            }
        }

        // Check if we read all rows, if not add special row to beginning of result
        if ((matched_rows >= cMaxMatchedRows || total_bytes >= cMaxTotalBytes)
            && SCXStream::IsGood(*logfile))
        {
//TODO: logging policy not set so by default may write into stdout and therefore interfere with the normal operation.
//          SCX_LOGINFO(m_log, StrAppend(L"LogFileProvider DoInvokeMethod - Breaking after matching max number of rows : ", cMaxMatchedRows));

            partialRead = true;
        }

        positioner.PersistState();
        return partialRead;
    }

    int LogFileReader::ResetLogFileState(
        const std::wstring& filename,
        const std::wstring& qid,
        bool resetOnRead)
    {
        wstringstream ss;
        ss << L"LogFileProvider ResetLogFileState: "
           << L"Filename: " << filename
           << L", qid: " << qid
           << L", resetOnRead: " << resetOnRead;
        SCX_LOGTRACE(m_log, ss.str())

        LogFileStreamPositioner positioner(filename, qid, m_persistMedia);
        SCXHandle<std::wfstream> logfile = positioner.GetStream();

        if (false == resetOnRead)
        {
            logfile->seekg(0, std::ios_base::end);
        }

        positioner.SetResetOnRead(resetOnRead);
        positioner.PersistState();
        return 0;
    }

    int LogFileReader::ResetAllLogFileStates(const std::wstring& path, bool resetOnRead)
    {
        int exitStatus = 0;

        SCX_LOGTRACE(m_log, L"LogFileProvider ResetAllLogFileStates - entry");

        // Determine the directory where the state files live
        SCXFilePath basePath(path);
        SCXUser user;

        if (!user.IsRoot())
        {
            basePath.AppendDirectory(user.GetName());
        }

        // Enumerate the state files
        // If exception occurs, items vector will be empty, so just fall through
        vector<SCXFilePath> items;
        try {
            items = SCXDirectory::GetFiles(basePath);
        }
        catch (SCXFilePathNotFoundException& e)
        {
            SCX_LOGWARNING(m_log, StrAppend(L"LogFileProvider ResetAllLogFileStates - Base path not found: ", basePath.Get()).append(L", exception: ").append(e.What()));

            // Return a special exit code so we know that the log file wasn't found
            exitStatus = ENOENT;
        }
        catch (SCXException &e)
        {
            SCX_LOGWARNING(m_log, StrAppend(L"LogFileProvider ResetAllLogFileStates - Unexpected exception: ", e.What()));

            // Return a special exit code so we know that an exception occurred
            exitStatus = EINTR;
        }

        for (vector<SCXFilePath>::iterator i(items.begin()); i != items.end(); ++i)
        {
            const std::wstring stateFilename = i->GetFilename();
            SCX_LOGHYSTERICAL(m_log, StrAppend(L"LogFileProvider ResetAllLogFileStates - Considering file: ", stateFilename));

            // If this isn't a state file, ignore it ...
            if (! StrIsPrefix(stateFilename, L"LogFileProvider_", true))
            {
                continue;
            }

            SCX_LOGTRACE(m_log, StrAppend(L"LogFileProvider ResetAllLogFileStates - Found file: ", stateFilename));

            SCXStream::NLFs nlfs;
            vector<wstring> lines;
            SCXFile::ReadAllLinesAsUTF8(*i, lines, nlfs);
            wstring filename, qid;

            SCX_LOGHYSTERICAL(m_log, L"LogFileProvider ResetAllLogFileStates - State file contents start");
            for (vector<wstring>::iterator l(lines.begin()); l != lines.end(); ++l)
            {
                SCX_LOGHYSTERICAL(m_log, StrAppend(L"LogFileProvider ResetAllLogFileStates - Line: ", *l));

                SCXRegex re(L"Value Name=\"(.*)\" Value=\"(.*)\"");
                vector<wstring> matches;

                if (re.ReturnMatch(*l, matches, 0))
                {
                    if ( L"Filename" == matches[1] )
                    {
                        filename = matches[2];
                    }
                    else if ( L"QID" == matches[1] )
                    {
                        qid = matches[2];
                    }
                }
            }
            SCX_LOGHYSTERICAL(m_log, L"LogFileProvider ResetAllLogFileStates - State file contents completed");

            // If we found the data that we needed, then reset the log file
            if (filename.length() && qid.length())
            {
                SCX_LOGTRACE(m_log, StrAppend(StrAppend(L"LogFileProvider ResetAllLogFileStates - Filename: ", filename).append(L", QID: "), qid));

                try
                {
                    int localStatus = ResetLogFileState(filename, qid, resetOnRead);

                    if (localStatus != 0)
                    {
                        exitStatus = localStatus;
                    }
                }
                catch (SCXFilePathNotFoundException& e)
                {
                    SCX_LOGWARNING(m_log, StrAppend(L"LogFileProvider ResetAllLogFileStates - File not found: ", filename).append(L", exception: ").append(e.What()));

                    // Return a special exit code so we know that the log file wasn't found
                    exitStatus = ENOENT;
                }
                catch (SCXException &e)
                {
                    SCX_LOGWARNING(m_log, StrAppend(L"LogFileProvider ResetAllLogFileStates - Unexpected exception: ", e.What()));

                    // Return a special exit code so we know that an exception occurred
                    exitStatus = EINTR;
                }
            }
        }

        SCX_LOGTRACE(m_log, L"LogFileProvider ResetAllLogFileStates - exit");
        return exitStatus;
    }
}

/*----------------------------E-N-D---O-F---F-I-L-E---------------------------*/
