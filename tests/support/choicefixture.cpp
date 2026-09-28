#include "choicefixture.h"

#include "attributeregistry.h"
#include "dataimporter.h"
#include "logbookmanager.h"
#include "logbookprobe.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"

using namespace FlySight;

namespace FlySightTest {

ChoiceFixture::ChoiceFixture(const QString &attributeKey)
    : m_key(attributeKey)
{
}

// The model goes before the columns change back: a live model would rebuild
ChoiceFixture::~ChoiceFixture()
{
    const bool started = m_model != nullptr;
    m_model.reset();
    if (started)
        LogbookColumnStore::instance().setColumns(m_columnsBefore);
}

QString ChoiceFixture::start(const QList<SessionData> &sessions, Rows rows)
{
    if (m_model)
        return QStringLiteral("start() was called twice");
    if (!definition())
        return QStringLiteral("no attribute definition for %1").arg(m_key);

    LogbookColumnStore &store = LogbookColumnStore::instance();
    m_columnsBefore = store.columns();
    store.setColumns({descriptionColumn(), choiceColumn()});

    m_model = std::make_unique<SessionModel>();
    const QList<MergeResult> results = m_model->mergeSessions(sessions);
    for (const MergeResult &result : results) {
        if (result.outcome != MergeResult::Outcome::Created)
            return QStringLiteral("session %1 was not created: %2").arg(result.sessionId, result.error);
    }
    if (!waitForIdle(*m_model))
        return QStringLiteral("the model did not become idle after the merge");
    return rows == Rows::Stubs ? restartAsStubs() : QString();
}

QString ChoiceFixture::restartAsStubs()
{
    if (!m_model)
        return QStringLiteral("restartAsStubs() before start()");
    if (!waitForIdle(*m_model))
        return QStringLiteral("the model did not become idle before the restart");

    // Every row comes back a stub with the column values index.json cached
    LogbookManager &logbook = LogbookManager::instance();
    m_model.reset();
    TestEnvironment::instance().reopenLogbook();
    logbook.initialize();
    m_model = std::make_unique<SessionModel>();
    m_model->populateFromIndex(logbook.cachedColumnValues(LogbookColumnStore::instance().enabledColumns()),
                               logbook.lastAccessedMap());
    for (int r = 0; r < m_model->rowCount(); ++r) {
        const SessionRow &row = std::as_const(*m_model).rowAt(r);
        if (row.isLoaded() || !row.cachedValues.contains(column()))
            return QStringLiteral("session %1 is not a stub with its value cached").arg(row.sessionId);
    }
    return QString();
}

const AttributeDefinition *ChoiceFixture::definition() const
{
    return AttributeRegistry::instance().findByKey(m_key);
}

LogbookColumn ChoiceFixture::choiceColumn() const
{
    return attributeColumn(m_key);
}

int ChoiceFixture::column() const
{
    if (!m_model)
        return -1;
    for (int c = 0; c < m_model->columnCount(); ++c) {
        const LogbookColumn &col = m_model->column(c);
        if (col.type == ColumnType::SessionAttribute && col.attributeKey == m_key)
            return c;
    }
    return -1;
}

int ChoiceFixture::row(const QString &sessionId) const
{
    return m_model ? m_model->getSessionRow(sessionId) : -1;
}

QModelIndex ChoiceFixture::cell(const QString &sessionId) const
{
    const int r = row(sessionId);
    return r < 0 ? QModelIndex() : m_model->index(r, column());
}

QString ChoiceFixture::displayText(const QString &sessionId) const
{
    return cell(sessionId).data(Qt::DisplayRole).toString();
}

std::optional<QString> ChoiceFixture::fileToken(const QString &sessionId) const
{
    const QString path = sessionFilePath(sessionId);
    if (path.isEmpty())
        return std::nullopt;
    return DataImporter::peekHeaderAttribute(path, m_key);
}

QJsonValue ChoiceFixture::indexValue(const QString &sessionId) const
{
    return FlySightTest::indexValue(sessionId, choiceColumn());
}

bool ChoiceFixture::setData(const QString &sessionId, const QVariant &value)
{
    const QModelIndex index = cell(sessionId);
    return index.isValid() && m_model->setData(index, value, Qt::EditRole);
}

bool ChoiceFixture::bulkEdit(const QStringList &sessionIds, const QVariant &value)
{
    if (!m_model)
        return false;
    QList<int> rows;
    for (const QString &sessionId : sessionIds) {
        const int r = row(sessionId);
        if (r < 0)
            return false;
        rows.append(r);
    }
    m_model->startBulkEdit(rows, column(), value);
    return waitForIdle(*m_model);
}

} // namespace FlySightTest
