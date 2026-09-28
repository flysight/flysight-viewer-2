#ifndef FLYSIGHTTEST_CHOICEFIXTURE_H
#define FLYSIGHTTEST_CHOICEFIXTURE_H

#include <memory>
#include <optional>

#include <QJsonValue>
#include <QList>
#include <QModelIndex>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include "logbookcolumn.h"

namespace FlySight {
struct AttributeDefinition;
class SessionData;
class SessionModel;
}

namespace FlySightTest {

/// A SessionModel over the test logbook with one Choice attribute column, for
/// tests of a Choice attribute through the model: its display, its edits and
/// its bulk edits, on loaded rows and on stubs. Parametrized by the attribute
/// key, whose definition it reads from the attribute registry: it knows
/// nothing about any particular attribute, and registers nothing. Widget-free
/// and fusion-free (tests/README.md section 8).
///
/// The caller provides the logbook (TestEnvironment::useFreshLogbook(),
/// LogbookManager::initialize()) and registers
/// PreferenceKeys::LogbookColumnsVersion, as every suite that sets columns
/// does. start() sets the logbook columns to the description column plus
/// attributeColumn(key); the destructor destroys the model first and then
/// puts the columns back as they were.
class ChoiceFixture {
public:
    enum class Rows {
        Loaded,     ///< the sessions merged, saved and indexed, their rows loaded
        Stubs       ///< the same after an application restart: every row a stub, its column values cached
    };

    explicit ChoiceFixture(const QString &attributeKey);
    ~ChoiceFixture();
    ChoiceFixture(const ChoiceFixture &) = delete;
    ChoiceFixture &operator=(const ChoiceFixture &) = delete;

    /// Adopts `sessions` into a fresh SessionModel (mergeSessions(): no
    /// import-time defaults), waits until they are saved and their columns
    /// cached, and for Rows::Stubs restarts as stubs (restartAsStubs()). Empty
    /// on success, otherwise what went wrong. Called once.
    [[nodiscard]] QString start(const QList<FlySight::SessionData> &sessions, Rows rows = Rows::Loaded);
    /// An application restart: waits until the model is idle (everything
    /// saved and cached), destroys it, reopens the logbook and populates a new
    /// model from index.json, as tst_column_cache's restartAsStubs() does.
    /// Every row is then a stub with its Choice value cached. Empty on
    /// success, otherwise what went wrong. A test that edits the loaded rows
    /// first (a token planted with SessionModel::updateAttribute(), say) calls
    /// it after start().
    [[nodiscard]] QString restartAsStubs();

    FlySight::SessionModel &model() { return *m_model; }
    QString attributeKey() const { return m_key; }
    /// The attribute's definition in the registry; nullptr when it has none.
    const FlySight::AttributeDefinition *definition() const;
    /// The logbook column over the attribute: attributeColumn(key).
    FlySight::LogbookColumn choiceColumn() const;
    /// The model column of the Choice attribute; -1 before start().
    int column() const;
    /// The row of a session now; -1 when none.
    int row(const QString &sessionId) const;
    /// The session's Choice cell; invalid when the session has no row.
    QModelIndex cell(const QString &sessionId) const;

    /// The text the session's Choice cell displays; empty for no value.
    QString displayText(const QString &sessionId) const;
    /// The token stored in the session's file on disk, read from its header
    /// without parsing the data; nullopt when the file has no `$VAR,<key>`
    /// line (or the session has no file).
    std::optional<QString> fileToken(const QString &sessionId) const;
    /// The value index.json on disk caches for the session's Choice column:
    /// Undefined when absent. Read it after waitForIdle(), which ends with the
    /// index written.
    QJsonValue indexValue(const QString &sessionId) const;

    /// SessionModel::setData() on the session's Choice cell, Qt::EditRole.
    bool setData(const QString &sessionId, const QVariant &value);
    /// SessionModel::startBulkEdit() of the Choice column for these sessions,
    /// then waitForIdle(). False when a session has no row or the model did
    /// not become idle.
    [[nodiscard]] bool bulkEdit(const QStringList &sessionIds, const QVariant &value);

private:
    QString m_key;
    QVector<FlySight::LogbookColumn> m_columnsBefore;
    std::unique_ptr<FlySight::SessionModel> m_model;
};

} // namespace FlySightTest

#endif // FLYSIGHTTEST_CHOICEFIXTURE_H
