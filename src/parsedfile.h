#ifndef PARSEDFILE_H
#define PARSEDFILE_H

#include <QString>

#include "sessiondata.h"

namespace FlySight {

/// What one imported file says, and nothing else: the recorded header
/// attributes and the source data exactly as parsed. Viewer's import-time
/// defaults are NOT part of it; they are applied by
/// DataImporter::applyCreationDefaults(), and only when the model decides that
/// the file creates a new session (a merge never applies them).
struct ParsedFile {
    QString     filePath;                       ///< empty for sessions built in memory
    SessionData data;                           ///< recorded header attributes + source data, exactly as parsed
    /// Match id: the recorded SESSION_ID, else the MD5 (lower-case hex) of the
    /// file bytes. The synthesized id is never written into `data`.
    QString     sessionId;
    bool        sessionIdRecorded = false;
    bool        applyCreationDefaults = true;   ///< false: adopt `data` as the new session unchanged

    /// Wraps an in-memory session (tests, tools): sessionId = the stored
    /// SESSION_ID ("" if none), sessionIdRecorded = !sessionId.isEmpty(),
    /// applyCreationDefaults = false.
    static ParsedFile fromSession(const SessionData &session)
    {
        ParsedFile file;
        file.data = session;
        file.sessionId = session.storedAttribute(QLatin1String(SessionKeys::SessionId)).toString();
        file.sessionIdRecorded = !file.sessionId.isEmpty();
        file.applyCreationDefaults = false;
        return file;
    }
};

} // namespace FlySight

#endif // PARSEDFILE_H
