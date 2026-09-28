// The Choice attribute format type (AttributeFormatType::Choice) through the
// logbook: the session model's display, sort, edit and bulk edit of a Choice
// column, the in-place list editor of LogbookCellDelegate, and the list of the
// context menu's "Set ..." action in LogbookView, with a real SessionModel and
// logbook, offscreen. Nothing here is specific to any product attribute: the
// test registers Choice attributes of its own and drives them through
// ChoiceFixture (choicefixture.h), which a later suite reuses for a product
// attribute.
//
// The test attributes. _TEST_CHOICE, editable, with three choices in this
// definition order:
//
//   | token | label   |
//   |-------|---------|
//   | b,1   | Charlie |
//   | c     | Alpha   |
//   | a     | Bravo   |
//
// so that label order (c, a, b,1), token order (a, b,1, c) and definition
// order (b,1, c, a) are three different orders, and one token holds a comma.
// A calculation with no inputs gives it the default token "c" (Alpha), as
// tst_calcengine::constantCalculationIsADefault registers one: a recording
// with nothing stored shows "Alpha", and removing a stored value returns to
// it. _TEST_CHOICE_BARE has the same choices and no default, so that a
// recording without a stored token has no value at all.
//
// The world: four recordings s1..s4, s1 storing "b,1", s2 "a", s3 nothing and
// s4 the token "zz", which is outside the list: it is planted with
// SessionModel::updateAttribute(), the programmatic writer, standing in for a
// hand-edited file.
//
// The menu's nested event loop is skipped: the test calls
// LogbookView::askAndSetAttribute() with what the menu captures. The dialog's
// is driven by a timer that waits for the modal QInputDialog, records it and
// accepts or rejects it.

#include <functional>
#include <memory>
#include <optional>

#include <QApplication>
#include <QComboBox>
#include <QHashFunctions>
#include <QInputDialog>
#include <QLineEdit>
#include <QSignalSpy>
#include <QStyleFactory>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtTest>

#include "attributeregistry.h"
#include "choicefixture.h"
#include "engine/calculationdescriptor.h"
#include "engine/calculationregistry.h"
#include "fixturebuilder.h"
#include "idlescheduler.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "logbookprobe.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "ui/docks/logbook/LogbookCellDelegate.h"
#include "ui/docks/logbook/LogbookView.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

constexpr char kChoice[] = "_TEST_CHOICE";
constexpr char kBare[] = "_TEST_CHOICE_BARE";
constexpr char kDefaultId[] = "test.choice.default";

QVector<AttributeChoice> testChoices()
{
    return {{QStringLiteral("b,1"), QStringLiteral("Charlie")},
            {QStringLiteral("c"), QStringLiteral("Alpha")},
            {QStringLiteral("a"), QStringLiteral("Bravo")}};
}

/// A recording with only header attributes (the ones the logbook's load would
/// otherwise backfill included), and `token` stored under `key` when given.
SessionData recording(const QString &id, const QString &key, const std::optional<QString> &token)
{
    SessionData s;
    s.setAttribute(QStringLiteral("SESSION_ID"), id);
    s.setAttribute(QStringLiteral("DEVICE_ID"), QStringLiteral("test-device"));
    s.setAttribute(QStringLiteral("_DESCRIPTION"), QStringLiteral("Jump ") + id);
    s.setAttribute(QStringLiteral("_JUMPER_MASS"), 80.0);
    s.setAttribute(QStringLiteral("_PLANFORM_AREA"), 2.0);
    if (token.has_value())
        s.setAttribute(key, *token);
    return s;
}

/// The bulk edit task's share of the scheduler's signals (as tst_column_cache)
struct BulkEditSignals {
    explicit BulkEditSignals(SessionModel &model)
        : active(&model.scheduler(), &IdleScheduler::activeTaskChanged)
    {}
    int activations() const
    {
        int count = 0;
        for (const QList<QVariant> &args : active)
            count += args.at(0).toInt() == SessionModel::BulkEditTask ? 1 : 0;
        return count;
    }
    QSignalSpy active;
};

/// What the "Set ..." dialog showed.
struct DialogSeen {
    bool seen = false;
    QString title;
    QInputDialog::InputMode mode = QInputDialog::IntInput;
    QStringList items;
    bool comboEditable = true;
    QString opening;            ///< the text value it opened on
    bool comboShown = false;    ///< a visible QComboBox
    bool lineEditShown = false; ///< a visible QLineEdit (an editable combo box has one)
};

const QStringList kIds{QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3"), QStringLiteral("s4")};

} // namespace

class ChoiceAttributeTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void choiceShowsTheLabelOfTheEffectiveToken_data();
    void choiceShowsTheLabelOfTheEffectiveToken();
    void choiceSortsByLabel_data();
    void choiceSortsByLabel();
    void choiceEditStoresAToken_data();
    void choiceEditStoresAToken();
    void choiceEditRefusesATokenOutsideTheList_data();
    void choiceEditRefusesATokenOutsideTheList();
    void choiceDefaultRemovesTheStoredValue_data();
    void choiceDefaultRemovesTheStoredValue();

    void bulkEditSetsAToken_data();
    void bulkEditSetsAToken();
    void bulkEditDefaultRemovesTheStoredValue_data();
    void bulkEditDefaultRemovesTheStoredValue();
    void bulkEditRefusesATokenOutsideTheList_data();
    void bulkEditRefusesATokenOutsideTheList();

    void cellEditorOffersTheList_data();
    void cellEditorOffersTheList();

    void setDialogOffersTheList_data();
    void setDialogOffersTheList();

private:
    static void addRowKinds()
    {
        QTest::addColumn<bool>("stubs");
        QTest::newRow("loaded") << false;
        QTest::newRow("stubs") << true;
    }
    /// The world of the file comment over `key`, loaded or as stubs after a
    /// restart. Empty on success.
    [[nodiscard]] QString startWorld(bool stubs, const QString &k = QString::fromLatin1(kChoice))
    {
        m_fixture = std::make_unique<ChoiceFixture>(k);
        const QString error = m_fixture->start({recording(QStringLiteral("s1"), k, QStringLiteral("b,1")),
                                                recording(QStringLiteral("s2"), k, QStringLiteral("a")),
                                                recording(QStringLiteral("s3"), k, std::nullopt),
                                                recording(QStringLiteral("s4"), k, std::nullopt)});
        if (!error.isEmpty())
            return error;
        if (!model().updateAttribute(QStringLiteral("s4"), k, QStringLiteral("zz")))
            return QStringLiteral("the token outside the list was not planted");
        if (!waitForIdle(model()))
            return QStringLiteral("the model did not become idle after the planting");
        return stubs ? m_fixture->restartAsStubs() : QString();
    }
    SessionModel &model() { return m_fixture->model(); }
    const SessionRow &rowOf(const QString &id) { return std::as_const(model()).rowAt(m_fixture->row(id)); }
    /// The session file's bytes on disk.
    static QByteArray fileBytes(const QString &id) { return readFileBytes(sessionFilePath(id)); }
    QMap<QString, QByteArray> allFileBytes() const
    {
        QMap<QString, QByteArray> bytes;
        for (const QString &id : kIds)
            bytes[id] = fileBytes(id);
        return bytes;
    }
    /// The rows' session ids, in row order.
    QStringList rowOrder()
    {
        QStringList ids;
        for (int r = 0; r < model().rowCount(); ++r)
            ids.append(std::as_const(model()).rowAt(r).sessionId);
        return ids;
    }
    /// True when every row is a stub.
    bool allStubs()
    {
        for (int r = 0; r < model().rowCount(); ++r) {
            if (std::as_const(model()).rowAt(r).isLoaded())
                return false;
        }
        return true;
    }

    /// A LogbookView over the fixture's model, without a demand layer, in a
    /// shown window. False when the window was never exposed.
    [[nodiscard]] bool buildUi()
    {
        m_window = std::make_unique<QWidget>();
        auto *layout = new QVBoxLayout(m_window.get());
        m_logbook = new LogbookView(&model(), nullptr, m_window.get());
        layout->addWidget(m_logbook);
        m_window->resize(640, 320);
        m_window->show();
        return QTest::qWaitForWindowExposed(m_window.get());
    }
    QTreeView *tree() const { return m_logbook->findChild<QTreeView *>(); }
    /// The editors the view shows now, of type T.
    template <typename T>
    QList<T *> shownEditors() const
    {
        QList<T *> shown;
        for (T *editor : tree()->viewport()->findChildren<T *>()) {
            if (editor->isVisible())
                shown.append(editor);
        }
        return shown;
    }
    /// Opens the editor of `index` with QAbstractItemView::edit(), as F2 does;
    /// the choice editor, or nullptr when there is not exactly one.
    QComboBox *openChoiceEditor(const QModelIndex &index)
    {
        tree()->setCurrentIndex(index);
        tree()->edit(index);
        const QList<QComboBox *> editors = shownEditors<QComboBox>();
        return editors.size() == 1 ? editors.first() : nullptr;
    }
    /// Enter on the editor: the base class commits and closes it. True once
    /// no editor is shown any more and the model is idle (the edit saved).
    [[nodiscard]] bool commitWithEnter(QWidget *editor)
    {
        QTest::keyClick(editor, Qt::Key_Return);
        return QTest::qWaitFor([this] { return shownEditors<QWidget>().isEmpty(); }, 5000)
            && waitForIdle(model());
    }

    /// Calls askAndSetAttribute(key, its column label, ids) as the menu does,
    /// with a timer that waits for the modal QInputDialog, records it and then
    /// hands it to `act` (which accepts or rejects it).
    DialogSeen askWith(const char *key, const QStringList &ids, const std::function<void(QInputDialog *)> &act)
    {
        DialogSeen seen;
        QTimer poll;
        poll.setInterval(10);
        QObject scope;      // after what the slots capture (tests/README.md section 8)
        QObject::connect(&poll, &QTimer::timeout, &scope, [&seen, &poll, &act] {
            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (!dialog || seen.seen)
                return;
            poll.stop();
            seen.seen = true;
            seen.title = dialog->windowTitle();
            seen.mode = dialog->inputMode();
            seen.items = dialog->comboBoxItems();
            seen.comboEditable = dialog->isComboBoxEditable();
            seen.opening = dialog->textValue();
            for (const QComboBox *combo : dialog->findChildren<QComboBox *>())
                seen.comboShown = seen.comboShown || combo->isVisible();
            for (const QLineEdit *edit : dialog->findChildren<QLineEdit *>())
                seen.lineEditShown = seen.lineEditShown || edit->isVisible();
            act(dialog);
        });
        // A defect fails the test instead of hanging it: whatever dialog is
        // still modal after ten seconds is rejected
        QTimer::singleShot(10000, &scope, [] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        poll.start();
        const QString k = QString::fromLatin1(key);
        m_logbook->askAndSetAttribute(k, logbookColumnLabel(attributeColumn(k)), ids);
        return seen;
    }

    std::unique_ptr<ChoiceFixture> m_fixture;
    std::unique_ptr<QWidget> m_window;
    LogbookView *m_logbook = nullptr;       // a child of the window
    QStringList m_registryBefore;
};

// The attributes are registered once per process: the attribute registry
// cannot remove a definition. The default calculation is registered before
// the logbook is initialized, as the application's are.
void ChoiceAttributeTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumnStore::instance().setColumns({descriptionColumn()});

    AttributeRegistry &attributes = AttributeRegistry::instance();
    attributes.registerAttribute({QStringLiteral("Test"), QStringLiteral("Test choice"), QString::fromLatin1(kChoice),
                                  AttributeFormatType::Choice, true, QString(), testChoices()});
    attributes.registerAttribute({QStringLiteral("Test"), QStringLiteral("Test choice without default"),
                                  QString::fromLatin1(kBare), AttributeFormatType::Choice, true, QString(),
                                  testChoices()});

    CalculationDescriptor constant;
    constant.id = QString::fromLatin1(kDefaultId);
    constant.outputs = {DependencyKey::attribute(QString::fromLatin1(kChoice))};
    constant.compute = [](const EvaluationContext &) {
        return CalculationResult().setAttribute(QString::fromLatin1(kChoice), QStringLiteral("c"));
    };
    QVERIFY(constant.inputs.isEmpty());
    QVERIFY(CalculationRegistry::instance().registerCalculation(constant));
}

void ChoiceAttributeTest::cleanupTestCase()
{
    CalculationRegistry::instance().unregister(QString::fromLatin1(kDefaultId), CalculationRegistry::Removal::Change);
}

void ChoiceAttributeTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
    m_registryBefore = CalculationRegistry::instance().registeredIds();
}

// The view goes before the model it shows, the model before the checks
void ChoiceAttributeTest::cleanup()
{
    m_logbook = nullptr;
    m_window.reset();
    m_fixture.reset();
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

// ---- The model ---------------------------------------------------------------------------

void ChoiceAttributeTest::choiceShowsTheLabelOfTheEffectiveToken_data() { addRowKinds(); }

// Criterion 2: the label of the effective token, stored or calculated, on
// loaded and stub rows; a token without a label as its raw text; the edit
// role the same text.
void ChoiceAttributeTest::choiceShowsTheLabelOfTheEffectiveToken()
{
    QFETCH(bool, stubs);
    QCOMPARE(startWorld(stubs), QString());

    const QStringList expected{QStringLiteral("Charlie"), QStringLiteral("Bravo"), QStringLiteral("Alpha"),
                               QStringLiteral("zz")};
    for (int i = 0; i < kIds.size(); ++i) {
        QCOMPARE(m_fixture->displayText(kIds[i]), expected[i]);
        QCOMPARE(m_fixture->cell(kIds[i]).data(Qt::EditRole).toString(), expected[i]);
    }
    QCOMPARE(allStubs(), stubs);

    // s3's value is the default: nothing is stored, and the index caches the token
    QCOMPARE(m_fixture->fileToken(QStringLiteral("s3")), std::optional<QString>());
    QCOMPARE(m_fixture->fileToken(QStringLiteral("s1")), std::optional<QString>(QStringLiteral("b,1")));
    QCOMPARE(m_fixture->fileToken(QStringLiteral("s4")), std::optional<QString>(QStringLiteral("zz")));
    QCOMPARE(m_fixture->indexValue(QStringLiteral("s3")), QJsonValue(QStringLiteral("c")));
    QCOMPARE(m_fixture->indexValue(QStringLiteral("s1")), QJsonValue(QStringLiteral("b,1")));
}

void ChoiceAttributeTest::choiceSortsByLabel_data()
{
    QTest::addColumn<bool>("stubs");
    QTest::addColumn<QString>("key");
    QTest::addColumn<QStringList>("ascending");
    QTest::addColumn<QStringList>("descending");

    // Alpha (s3, the default), Bravo (s2), Charlie (s1), then the raw "zz"
    // (s4). By token it would be s2, s1, s3, s4; by definition order s1, s3,
    // s2, s4.
    const QStringList withDefault{QStringLiteral("s3"), QStringLiteral("s2"), QStringLiteral("s1"),
                                  QStringLiteral("s4")};
    const QStringList withDefaultDown{QStringLiteral("s4"), QStringLiteral("s1"), QStringLiteral("s2"),
                                      QStringLiteral("s3")};
    // Without a default s3 has no value: last both ways
    const QStringList bare{QStringLiteral("s2"), QStringLiteral("s1"), QStringLiteral("s4"), QStringLiteral("s3")};
    const QStringList bareDown{QStringLiteral("s4"), QStringLiteral("s1"), QStringLiteral("s2"),
                               QStringLiteral("s3")};
    for (bool stubs : {false, true}) {
        const char *kind = stubs ? "stubs" : "loaded";
        QTest::addRow("%s", kind) << stubs << QString::fromLatin1(kChoice) << withDefault << withDefaultDown;
        QTest::addRow("%s, no value", kind) << stubs << QString::fromLatin1(kBare) << bare << bareDown;
    }
}

// Criterion 3: sorted by the text the cell displays, case-insensitively,
// missing values last, on loaded rows (the effective token) and stubs (the
// cached token), which stay stubs.
void ChoiceAttributeTest::choiceSortsByLabel()
{
    QFETCH(bool, stubs);
    QFETCH(QString, key);
    QFETCH(QStringList, ascending);
    QFETCH(QStringList, descending);
    QCOMPARE(startWorld(stubs, key), QString());
    QCOMPARE(rowOrder(), kIds);
    if (key == QLatin1String(kBare))
        QCOMPARE(m_fixture->displayText(QStringLiteral("s3")), QString());

    model().sort(m_fixture->column(), Qt::AscendingOrder);
    QCOMPARE(rowOrder(), ascending);
    model().sort(m_fixture->column(), Qt::DescendingOrder);
    QCOMPARE(rowOrder(), descending);
    QCOMPARE(allStubs(), stubs);
}

void ChoiceAttributeTest::choiceEditStoresAToken_data() { addRowKinds(); }

// Criterion 4: a token of the list is stored, written verbatim (its comma
// included), shown by its label and published as a direct edit is; the same
// token again changes nothing. Decision 2: the stored value is what is
// compared, so a recording showing the default can store its token.
void ChoiceAttributeTest::choiceEditStoresAToken()
{
    QFETCH(bool, stubs);
    QCOMPARE(startWorld(stubs), QString());
    QSignalSpy dependencySpy(&model(), &SessionModel::dependencyChanged);
    QSignalSpy dataSpy(&model(), &QAbstractItemModel::dataChanged);

    QVERIFY(m_fixture->setData(QStringLiteral("s2"), QStringLiteral("b,1")));
    QCOMPARE(m_fixture->displayText(QStringLiteral("s2")), QStringLiteral("Charlie"));
    QVERIFY(dataSpy.count() > 0);
    QVERIFY(spyHasAttribute(dependencySpy, QStringLiteral("s2"), QString::fromLatin1(kChoice)));
    QVERIFY(waitForIdle(model()));
    QVERIFY(fileBytes(QStringLiteral("s2")).contains("\n$VAR,_TEST_CHOICE,b,1\n"));
    QCOMPARE(m_fixture->fileToken(QStringLiteral("s2")), std::optional<QString>(QStringLiteral("b,1")));
    QCOMPARE(m_fixture->indexValue(QStringLiteral("s2")), QJsonValue(QStringLiteral("b,1")));

    // The same token again: nothing to do
    const QByteArray bytes = fileBytes(QStringLiteral("s2"));
    dataSpy.clear();
    QVERIFY(!m_fixture->setData(QStringLiteral("s2"), QStringLiteral("b,1")));
    QCOMPARE(dataSpy.count(), 0);
    QVERIFY(!rowOf(QStringLiteral("s2")).dirty);
    QVERIFY(waitForIdle(model()));
    QCOMPARE(fileBytes(QStringLiteral("s2")), bytes);

    // The default's own token, pinned: s3 shows "Alpha" with nothing stored
    QCOMPARE(m_fixture->fileToken(QStringLiteral("s3")), std::optional<QString>());
    QVERIFY(m_fixture->setData(QStringLiteral("s3"), QStringLiteral("c")));
    QCOMPARE(m_fixture->displayText(QStringLiteral("s3")), QStringLiteral("Alpha"));
    QVERIFY(waitForIdle(model()));
    QCOMPARE(m_fixture->fileToken(QStringLiteral("s3")), std::optional<QString>(QStringLiteral("c")));
    QVERIFY(!m_fixture->setData(QStringLiteral("s3"), QStringLiteral("c")));
}

void ChoiceAttributeTest::choiceEditRefusesATokenOutsideTheList_data() { addRowKinds(); }

// Criterion 5: a token outside the list, a label, an empty string and a value
// of another type are refused: false, nothing changed, nothing emitted, a stub
// left a stub, the file untouched.
void ChoiceAttributeTest::choiceEditRefusesATokenOutsideTheList()
{
    QFETCH(bool, stubs);
    QCOMPARE(startWorld(stubs), QString());
    const QMap<QString, QByteArray> before = allFileBytes();
    QSignalSpy dataSpy(&model(), &QAbstractItemModel::dataChanged);
    QSignalSpy dependencySpy(&model(), &SessionModel::dependencyChanged);

    for (const QVariant &value : {QVariant(QStringLiteral("zz")), QVariant(QStringLiteral("yy")),
                                  QVariant(QStringLiteral("Charlie")), QVariant(QStringLiteral("Alpha")),
                                  QVariant(QString()), QVariant(QStringLiteral("")), QVariant(2.0)}) {
        for (const QString &id : kIds)
            QVERIFY2(!m_fixture->setData(id, value), qPrintable(id + QLatin1Char(' ') + value.toString()));
    }
    QCOMPARE(dataSpy.count(), 0);
    QCOMPARE(dependencySpy.count(), 0);
    QCOMPARE(allStubs(), stubs);
    for (const QString &id : kIds)
        QVERIFY2(!rowOf(id).dirty, qPrintable(id));
    QCOMPARE(m_fixture->displayText(QStringLiteral("s1")), QStringLiteral("Charlie"));
    QCOMPARE(m_fixture->displayText(QStringLiteral("s4")), QStringLiteral("zz"));
    QVERIFY(waitForIdle(model()));
    QCOMPARE(allFileBytes(), before);
}

void ChoiceAttributeTest::choiceDefaultRemovesTheStoredValue_data() { addRowKinds(); }

// Criterion 6: an invalid value removes the stored attribute: no line in the
// file, the default's label again, dependents notified. With nothing stored
// it is refused and nothing is saved.
void ChoiceAttributeTest::choiceDefaultRemovesTheStoredValue()
{
    QFETCH(bool, stubs);
    QCOMPARE(startWorld(stubs), QString());
    QSignalSpy dependencySpy(&model(), &SessionModel::dependencyChanged);

    QVERIFY(m_fixture->setData(QStringLiteral("s1"), QVariant()));
    QCOMPARE(m_fixture->displayText(QStringLiteral("s1")), QStringLiteral("Alpha"));
    QVERIFY(spyHasAttribute(dependencySpy, QStringLiteral("s1"), QString::fromLatin1(kChoice)));
    QVERIFY(waitForIdle(model()));
    QCOMPARE(m_fixture->fileToken(QStringLiteral("s1")), std::optional<QString>());
    QVERIFY(!fileBytes(QStringLiteral("s1")).contains("$VAR,_TEST_CHOICE,"));
    QVERIFY(fileBytes(QStringLiteral("s1")).contains("$VAR,_DESCRIPTION,Jump s1\n"));
    QCOMPARE(m_fixture->indexValue(QStringLiteral("s1")), QJsonValue(QStringLiteral("c")));

    // The raw token too
    QVERIFY(m_fixture->setData(QStringLiteral("s4"), QVariant()));
    QCOMPARE(m_fixture->displayText(QStringLiteral("s4")), QStringLiteral("Alpha"));
    QVERIFY(waitForIdle(model()));
    QCOMPARE(m_fixture->fileToken(QStringLiteral("s4")), std::optional<QString>());

    // Nothing stored: nothing to remove, nothing saved
    const QMap<QString, QByteArray> before = allFileBytes();
    QSignalSpy dataSpy(&model(), &QAbstractItemModel::dataChanged);
    dependencySpy.clear();
    for (const char *id : {"s1", "s3"})
        QVERIFY2(!m_fixture->setData(QString::fromLatin1(id), QVariant()), id);
    QCOMPARE(dataSpy.count(), 0);
    QCOMPARE(dependencySpy.count(), 0);
    QVERIFY(!rowOf(QStringLiteral("s3")).dirty);
    QVERIFY(waitForIdle(model()));
    QCOMPARE(allFileBytes(), before);
}

// ---- The bulk edit -------------------------------------------------------------------------

void ChoiceAttributeTest::bulkEditSetsAToken_data() { addRowKinds(); }

// Criterion 7: the token for every selected session, loaded or stub (which
// stays a stub); files, index.json and cells agree, and each session's change
// is published.
void ChoiceAttributeTest::bulkEditSetsAToken()
{
    QFETCH(bool, stubs);
    QCOMPARE(startWorld(stubs), QString());
    QSignalSpy dependencySpy(&model(), &SessionModel::dependencyChanged);
    const QStringList edited{QStringLiteral("s1"), QStringLiteral("s3"), QStringLiteral("s4")};

    QVERIFY(m_fixture->bulkEdit(edited, QStringLiteral("a")));
    for (const QString &id : kIds) {
        QCOMPARE(m_fixture->fileToken(id), std::optional<QString>(QStringLiteral("a")));
        QCOMPARE(m_fixture->indexValue(id), QJsonValue(QStringLiteral("a")));
        QCOMPARE(m_fixture->displayText(id), QStringLiteral("Bravo"));
    }
    for (const QString &id : edited)
        QVERIFY2(spyHasAttribute(dependencySpy, id, QString::fromLatin1(kChoice)), qPrintable(id));
    QCOMPARE(allStubs(), stubs);

    // The token with a comma, written verbatim
    QVERIFY(m_fixture->bulkEdit({QStringLiteral("s2")}, QStringLiteral("b,1")));
    QVERIFY(fileBytes(QStringLiteral("s2")).contains("\n$VAR,_TEST_CHOICE,b,1\n"));
    QCOMPARE(m_fixture->indexValue(QStringLiteral("s2")), QJsonValue(QStringLiteral("b,1")));
    QCOMPARE(m_fixture->displayText(QStringLiteral("s2")), QStringLiteral("Charlie"));
}

void ChoiceAttributeTest::bulkEditDefaultRemovesTheStoredValue_data() { addRowKinds(); }

// Criterion 8: an invalid value removes the stored attribute on loaded and
// stub rows alike: no file keeps the line, and the cells and the index show
// the default.
void ChoiceAttributeTest::bulkEditDefaultRemovesTheStoredValue()
{
    QFETCH(bool, stubs);
    QCOMPARE(startWorld(stubs), QString());
    QSignalSpy dependencySpy(&model(), &SessionModel::dependencyChanged);
    const QStringList edited{QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s4")};

    QVERIFY(m_fixture->bulkEdit(edited, QVariant()));
    for (const QString &id : kIds) {
        QCOMPARE(m_fixture->fileToken(id), std::optional<QString>());
        QVERIFY2(!fileBytes(id).contains("$VAR,_TEST_CHOICE,"), qPrintable(id));
        QCOMPARE(m_fixture->displayText(id), QStringLiteral("Alpha"));
        QCOMPARE(m_fixture->indexValue(id), QJsonValue(QStringLiteral("c")));
    }
    for (const QString &id : edited)
        QVERIFY2(spyHasAttribute(dependencySpy, id, QString::fromLatin1(kChoice)), qPrintable(id));
    QCOMPARE(allStubs(), stubs);
}

void ChoiceAttributeTest::bulkEditRefusesATokenOutsideTheList_data() { addRowKinds(); }

// Criterion 9: a value outside the list queues nothing: the bulk edit task
// never becomes active, no file changes, and a stub keeps its cached value.
void ChoiceAttributeTest::bulkEditRefusesATokenOutsideTheList()
{
    QFETCH(bool, stubs);
    QCOMPARE(startWorld(stubs), QString());
    const QMap<QString, QByteArray> before = allFileBytes();
    const QList<int> rows{0, 1, 2, 3};

    BulkEditSignals bulk(model());
    QSignalSpy dataSpy(&model(), &QAbstractItemModel::dataChanged);
    for (const QVariant &value : {QVariant(QStringLiteral("zz")), QVariant(QStringLiteral("Bravo")),
                                  QVariant(QString()), QVariant(2.0)})
        model().startBulkEdit(rows, m_fixture->column(), value);
    for (int r : rows)
        QVERIFY(std::as_const(model()).rowAt(r).cachedValues.contains(m_fixture->column()));
    QVERIFY(waitForIdle(model()));

    QCOMPARE(bulk.activations(), 0);
    QCOMPARE(dataSpy.count(), 0);
    QCOMPARE(allFileBytes(), before);
    QCOMPARE(allStubs(), stubs);
    for (int r : rows) {
        const SessionRow &row = std::as_const(model()).rowAt(r);
        QVERIFY2(row.cachedValues.contains(m_fixture->column()), qPrintable(row.sessionId));
        QVERIFY2(!row.dirty, qPrintable(row.sessionId));
    }
    QCOMPARE(m_fixture->displayText(QStringLiteral("s1")), QStringLiteral("Charlie"));
    QCOMPARE(m_fixture->displayText(QStringLiteral("s3")), QStringLiteral("Alpha"));
}

// ---- The in-place editor -------------------------------------------------------------------

void ChoiceAttributeTest::cellEditorOffersTheList_data() { addRowKinds(); }

// Criterion 10: a non-editable combo box of "Default" then the labels in
// definition order, opening on the cell's label (on no entry for a raw
// token); a label stores its token, "Default" removes the stored value, and
// closing on the opening entry writes nothing. A Text cell keeps the base
// class's line edit.
void ChoiceAttributeTest::cellEditorOffersTheList()
{
    QFETCH(bool, stubs);
    QCOMPARE(startWorld(stubs), QString());
    QVERIFY(buildUi());
    QVERIFY(tree());
    QVERIFY(tree()->model() == &model());

    // The list, opening on s1's label
    QComboBox *editor = openChoiceEditor(m_fixture->cell(QStringLiteral("s1")));
    QVERIFY(editor);
    QVERIFY(!editor->isEditable());
    QVERIFY(!editor->lineEdit());
    QStringList labels;
    QVariantList values;
    for (int i = 0; i < editor->count(); ++i) {
        labels.append(editor->itemText(i));
        values.append(editor->itemData(i));
    }
    QCOMPARE(labels, QStringList({QStringLiteral("Default"), QStringLiteral("Charlie"), QStringLiteral("Alpha"),
                                  QStringLiteral("Bravo")}));
    QCOMPARE(values, QVariantList({QVariant(), QVariant(QStringLiteral("b,1")), QVariant(QStringLiteral("c")),
                                   QVariant(QStringLiteral("a"))}));
    QCOMPARE(editor->currentIndex(), 1);

    // Closed on the entry it opened on: nothing is written
    const QMap<QString, QByteArray> before = allFileBytes();
    QSignalSpy dependencySpy(&model(), &SessionModel::dependencyChanged);
    QVERIFY(commitWithEnter(editor));
    QCOMPARE(dependencySpy.count(), 0);
    QCOMPARE(allFileBytes(), before);
    QCOMPARE(allStubs(), stubs);

    // s3 shows the default: opens on "Alpha" and writes nothing left there ...
    editor = openChoiceEditor(m_fixture->cell(QStringLiteral("s3")));
    QVERIFY(editor);
    QCOMPARE(editor->currentIndex(), 2);
    QVERIFY(commitWithEnter(editor));
    QCOMPARE(m_fixture->fileToken(QStringLiteral("s3")), std::optional<QString>());
    QCOMPARE(allFileBytes(), before);

    // ... and a label stores its token
    editor = openChoiceEditor(m_fixture->cell(QStringLiteral("s3")));
    QVERIFY(editor);
    editor->setCurrentIndex(3);
    QVERIFY(commitWithEnter(editor));
    QCOMPARE(m_fixture->fileToken(QStringLiteral("s3")), std::optional<QString>(QStringLiteral("a")));
    QCOMPARE(m_fixture->displayText(QStringLiteral("s3")), QStringLiteral("Bravo"));

    // "Default" removes s1's stored value
    editor = openChoiceEditor(m_fixture->cell(QStringLiteral("s1")));
    QVERIFY(editor);
    QCOMPARE(editor->currentIndex(), 1);
    editor->setCurrentIndex(0);
    QVERIFY(commitWithEnter(editor));
    QCOMPARE(m_fixture->fileToken(QStringLiteral("s1")), std::optional<QString>());
    QCOMPARE(m_fixture->displayText(QStringLiteral("s1")), QStringLiteral("Alpha"));

    // A raw token opens on no entry; "Default" is then a change
    editor = openChoiceEditor(m_fixture->cell(QStringLiteral("s4")));
    QVERIFY(editor);
    QCOMPARE(editor->currentIndex(), -1);
    editor->setCurrentIndex(0);
    QVERIFY(commitWithEnter(editor));
    QCOMPARE(m_fixture->fileToken(QStringLiteral("s4")), std::optional<QString>());
    QCOMPARE(m_fixture->displayText(QStringLiteral("s4")), QStringLiteral("Alpha"));

    // A Text cell: the base class's line edit, and no list
    int description = -1;
    for (int c = 0; c < model().columnCount(); ++c) {
        if (model().column(c).attributeKey == QLatin1String(SessionKeys::Description))
            description = c;
    }
    QVERIFY(description >= 0);
    const QModelIndex text = model().index(m_fixture->row(QStringLiteral("s2")), description);
    tree()->setCurrentIndex(text);
    tree()->edit(text);
    QCOMPARE(shownEditors<QComboBox>().size(), 0);
    const QList<QLineEdit *> lineEdits = shownEditors<QLineEdit>();
    QCOMPARE(lineEdits.size(), 1);
    QCOMPARE(lineEdits.first()->text(), QStringLiteral("Jump s2"));
    QTest::keyClick(lineEdits.first(), Qt::Key_Escape);
    QTRY_VERIFY(shownEditors<QWidget>().isEmpty());
}

// ---- The context menu's "Set ..." ----------------------------------------------------------

void ChoiceAttributeTest::setDialogOffersTheList_data() { addRowKinds(); }

// Criterion 11: the dialog of a Choice attribute is a list, not editable,
// "Default" then the labels, opening on "Default"; a label bulk-edits its
// token onto the captured sessions, "Default" removes the stored value, and
// rejecting changes nothing. A Text attribute keeps the text prompt.
void ChoiceAttributeTest::setDialogOffersTheList()
{
    QFETCH(bool, stubs);
    QCOMPARE(startWorld(stubs), QString());
    QVERIFY(buildUi());
    const QStringList list{QStringLiteral("Default"), QStringLiteral("Charlie"), QStringLiteral("Alpha"),
                           QStringLiteral("Bravo")};

    // A label: its token onto s1 and s3
    DialogSeen seen = askWith(kChoice, {QStringLiteral("s1"), QStringLiteral("s3")}, [](QInputDialog *dialog) {
        dialog->setTextValue(QStringLiteral("Bravo"));
        dialog->accept();
    });
    QVERIFY(seen.seen);
    QCOMPARE(seen.title, QStringLiteral("Set Test choice"));
    QCOMPARE(seen.mode, QInputDialog::TextInput);     // the combo box form of the text input
    QCOMPARE(seen.items, list);
    QVERIFY(!seen.comboEditable);
    QVERIFY(seen.comboShown);
    QVERIFY(!seen.lineEditShown);
    QCOMPARE(seen.opening, QStringLiteral("Default"));
    QVERIFY(waitForIdle(model()));
    for (const char *id : {"s1", "s3"}) {
        QCOMPARE(m_fixture->fileToken(QString::fromLatin1(id)), std::optional<QString>(QStringLiteral("a")));
        QCOMPARE(m_fixture->displayText(QString::fromLatin1(id)), QStringLiteral("Bravo"));
    }
    QCOMPARE(m_fixture->fileToken(QStringLiteral("s4")), std::optional<QString>(QStringLiteral("zz")));
    QCOMPARE(allStubs(), stubs);

    // "Default": s1's and s4's stored values removed
    seen = askWith(kChoice, {QStringLiteral("s1"), QStringLiteral("s4")}, [](QInputDialog *dialog) {
        dialog->setTextValue(QStringLiteral("Default"));
        dialog->accept();
    });
    QVERIFY(seen.seen);
    QCOMPARE(seen.items, list);
    QVERIFY(waitForIdle(model()));
    for (const char *id : {"s1", "s4"}) {
        QCOMPARE(m_fixture->fileToken(QString::fromLatin1(id)), std::optional<QString>());
        QCOMPARE(m_fixture->displayText(QString::fromLatin1(id)), QStringLiteral("Alpha"));
    }
    QCOMPARE(m_fixture->fileToken(QStringLiteral("s3")), std::optional<QString>(QStringLiteral("a")));

    // Rejected: nothing happens
    const QMap<QString, QByteArray> before = allFileBytes();
    {
        BulkEditSignals bulk(model());
        seen = askWith(kChoice, kIds, [](QInputDialog *dialog) {
            dialog->setTextValue(QStringLiteral("Charlie"));
            dialog->reject();
        });
        QVERIFY(seen.seen);
        QVERIFY(waitForIdle(model()));
        QCOMPARE(bulk.activations(), 0);
    }
    QCOMPARE(allFileBytes(), before);

    // A Text attribute: the text prompt, as before
    seen = askWith(SessionKeys::Description, {QStringLiteral("s2")}, [](QInputDialog *dialog) {
        dialog->setTextValue(QStringLiteral("Renamed"));
        dialog->accept();
    });
    QVERIFY(seen.seen);
    QCOMPARE(seen.mode, QInputDialog::TextInput);
    QCOMPARE(seen.items, QStringList());
    QVERIFY(seen.lineEditShown);
    QVERIFY(!seen.comboShown);
    QCOMPARE(seen.opening, QString());
    QVERIFY(waitForIdle(model()));
    QVERIFY(fileBytes(QStringLiteral("s2")).contains("\n$VAR,_DESCRIPTION,Renamed\n"));
}

// A Widgets test writes its own main() (tests/README.md section 8): the same
// order as FLYSIGHT_TEST_MAIN, with a QApplication and the application's style.
int main(int argc, char **argv)
{
    QHashSeed::setDeterministicGlobalSeed();
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    FlySightTest::TestEnvironment env(QStringLiteral("ChoiceAttributeTest"));
    ChoiceAttributeTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_choice_attribute.moc"
