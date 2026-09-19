#include "calculationregistry.h"

#include <algorithm>

#include <QDebug>
#include <QMap>
#include <QSet>

#include "calculationengine.h"

namespace FlySight {

CalculationRegistry &CalculationRegistry::instance()
{
    static CalculationRegistry registry;
    return registry;
}

CalculationRegistry::CalculationRegistry() = default;

CalculationRegistry::~CalculationRegistry()
{
    Q_ASSERT_X(m_engines.isEmpty(), "CalculationRegistry",
               "a registry must outlive the engines bound to it");
    // Release builds: leave any surviving engine in a safe, empty state rather
    // than holding a dangling pointer.
    const QList<CalculationEngine *> engines = m_engines;
    m_engines.clear();
    for (CalculationEngine *e : engines)
        e->registryDestroyed();
}

// ---------------------------------------------------------------- registration

bool CalculationRegistry::checkMutable(const char *what, const CalculationId &id) const
{
    if (m_activeEvaluations > 0) {
        qWarning().noquote() << "CalculationRegistry:" << what << id
                             << "rejected: the registry cannot change during an evaluation";
        Q_ASSERT_X(false, "CalculationRegistry", "registry change during an evaluation");
        return false;
    }
    return true;
}

bool CalculationRegistry::validate(const CalculationDescriptor &d, bool sourceInputsPermitted,
                                   const QString &label) const
{
    if (d.outputs.isEmpty()) {
        qWarning().noquote() << "CalculationRegistry:" << label << "declares no outputs";
        return false;
    }
    QSet<DependencyKey> seen;
    for (const DependencyKey &out : d.outputs) {
        if (seen.contains(out)) {
            qWarning().noquote() << "CalculationRegistry:" << label
                                 << "declares output" << describe(out) << "twice";
            return false;
        }
        seen.insert(out);
    }
    if (!d.compute) {
        qWarning().noquote() << "CalculationRegistry:" << label << "has no compute function";
        return false;
    }
    for (const CalcInput &in : d.inputs) {
        if (in.isSourceKind()) {
            if (!sourceInputsPermitted) {
                qWarning().noquote() << "CalculationRegistry:" << label << "declares" << describe(in)
                                     << "- only source conversions (and plugin calculations that opt in)"
                                     << "may read the source layer";
                return false;
            }
            continue;
        }
        const bool ownOutput =
            (in.kind == CalcInput::Kind::Attribute && seen.contains(DependencyKey::attribute(in.key)))
            || (in.kind == CalcInput::Kind::Measurement
                && seen.contains(DependencyKey::measurement(in.sensor, in.name)));
        if (ownOutput) {
            qWarning().noquote() << "CalculationRegistry:" << label
                                 << "declares its own output as an input:" << describe(in);
            return false;
        }
    }
    return true;
}

bool CalculationRegistry::registerCalculation(const CalculationDescriptor &d)
{
    if (!checkMutable("registration of", d.id))
        return false;
    if (d.id.isEmpty() || d.id.contains(QLatin1Char('#'))) {
        qWarning().noquote() << "CalculationRegistry: invalid calculation id" << d.id;
        return false;
    }
    if (contains(d.id)) {
        qWarning().noquote() << "CalculationRegistry: id already registered:" << d.id;
        return false;
    }
    // Source inputs: only through registerSourceConversion, or by the descriptor's
    // explicit opt-in (set only by the Python plugin host).
    if (!validate(d, d.allowSourceInputs, d.id))
        return false;

    Entry entry;
    entry.kind = EntryKind::Calculation;
    entry.id = d.id;
    entry.descriptor = std::make_shared<const CalculationDescriptor>(d);
    m_entries.append(entry);

    RegistryChange change;
    change.registrationId = d.id;
    change.added = true;
    change.kind = RegistryChange::Kind::Calculation;
    change.outputs = d.outputs;
    broadcast(change);
    return true;
}

bool CalculationRegistry::registerFamily(const CalculationFamily &f)
{
    return addFamily(f, EntryKind::Family);
}

bool CalculationRegistry::registerSourceConversion(const CalculationFamily &f)
{
    return addFamily(f, EntryKind::SourceConversion);
}

bool CalculationRegistry::addFamily(const CalculationFamily &f, EntryKind kind)
{
    if (!checkMutable("registration of", f.id))
        return false;
    if (f.id.isEmpty() || f.id.contains(QLatin1Char('#'))) {
        qWarning().noquote() << "CalculationRegistry: invalid family id" << f.id;
        return false;
    }
    if (contains(f.id)) {
        qWarning().noquote() << "CalculationRegistry: id already registered:" << f.id;
        return false;
    }
    if (!f.instantiate) {
        qWarning().noquote() << "CalculationRegistry: family" << f.id << "has no instantiate function";
        return false;
    }

    Entry entry;
    entry.kind = kind;
    entry.id = f.id;
    entry.family = f;
    m_entries.append(entry);

    RegistryChange change;
    change.registrationId = f.id;
    change.added = true;
    change.kind = kind == EntryKind::SourceConversion ? RegistryChange::Kind::SourceConversion
                                                      : RegistryChange::Kind::Family;
    change.instantiate = f.instantiate;
    broadcast(change);
    return true;
}

bool CalculationRegistry::unregister(const CalculationId &id)
{
    if (!checkMutable("removal of", id))
        return false;

    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [&id](const Entry &e) { return e.id == id; });
    if (it == m_entries.end())
        return false;

    RegistryChange change;
    change.registrationId = id;
    change.added = false;
    switch (it->kind) {
    case EntryKind::Calculation:
        change.kind = RegistryChange::Kind::Calculation;
        change.outputs = it->descriptor->outputs;
        break;
    case EntryKind::Family:
        change.kind = RegistryChange::Kind::Family;
        change.instantiate = it->family.instantiate;
        break;
    case EntryKind::SourceConversion:
        change.kind = RegistryChange::Kind::SourceConversion;
        change.instantiate = it->family.instantiate;
        break;
    }

    m_entries.erase(it);    // drops the family's memo with it
    broadcast(change);
    return true;
}

// --------------------------------------------------------------------- queries

bool CalculationRegistry::contains(const CalculationId &id) const
{
    return std::any_of(m_entries.cbegin(), m_entries.cend(),
                       [&id](const Entry &e) { return e.id == id; });
}

QList<CalculationId> CalculationRegistry::registeredIds() const
{
    QList<CalculationId> ids;
    ids.reserve(m_entries.size());
    for (const Entry &e : m_entries)    // m_entries is kept in sequence order
        ids.append(e.id);
    return ids;
}

bool CalculationRegistry::isFamily(const CalculationId &id) const
{
    return std::any_of(m_entries.cbegin(), m_entries.cend(), [&id](const Entry &e) {
        return e.id == id && e.kind != EntryKind::Calculation;
    });
}

std::optional<CalculationInstance> CalculationRegistry::instantiate(const Entry &entry,
                                                                   const DependencyKey &name) const
{
    if (entry.kind == EntryKind::Calculation) {
        CalculationInstance inst;
        inst.instanceId = entry.id;
        inst.registrationId = entry.id;
        inst.descriptor = entry.descriptor;
        return inst;
    }

    std::optional<CalculationDescriptor> d;
    try {
        d = entry.family.instantiate(name);
    } catch (const std::exception &ex) {
        qWarning().noquote() << "CalculationRegistry: family" << entry.id << "threw while instantiating"
                             << describe(name) << ":" << ex.what();
        return std::nullopt;
    } catch (...) {
        qWarning().noquote() << "CalculationRegistry: family" << entry.id << "threw while instantiating"
                             << describe(name);
        return std::nullopt;
    }
    if (!d)
        return std::nullopt;

    const QString instanceKey = d->id;
    const auto memoized = entry.memo.constFind(instanceKey);
    if (memoized != entry.memo.constEnd()) {
        if (!memoized->descriptor->outputs.contains(name)) {
            qWarning().noquote() << "CalculationRegistry: family" << entry.id << "instance" << instanceKey
                                 << "does not declare" << describe(name) << "- treated as no match";
            return std::nullopt;
        }
        return *memoized;
    }

    // First instantiation: same rules as a plain registration.
    const QString instanceId = entry.id + QLatin1Char('#') + instanceKey;
    if (instanceKey.isEmpty() || instanceKey.contains(QLatin1Char('#'))) {
        qWarning().noquote() << "CalculationRegistry: family" << entry.id
                             << "returned an invalid instance key" << instanceKey << "for" << describe(name);
        return std::nullopt;
    }
    if (!d->outputs.contains(name)) {
        qWarning().noquote() << "CalculationRegistry:" << instanceId << "does not declare the name it was"
                             << "instantiated for:" << describe(name);
        return std::nullopt;
    }
    if (!validate(*d, entry.kind == EntryKind::SourceConversion, instanceId))
        return std::nullopt;

    d->id = instanceId;
    d->policy = entry.family.policy;

    CalculationInstance inst;
    inst.instanceId = instanceId;
    inst.registrationId = entry.id;
    inst.descriptor = std::make_shared<const CalculationDescriptor>(std::move(*d));
    inst.sourceConversion = entry.kind == EntryKind::SourceConversion;
    entry.memo.insert(instanceKey, inst);
    return inst;
}

QList<CalculationInstance> CalculationRegistry::candidatesFor(const DependencyKey &name) const
{
    QList<CalculationInstance> result;
    for (const Entry &entry : m_entries) {
        if (entry.kind == EntryKind::SourceConversion)
            continue;
        if (entry.kind == EntryKind::Calculation && !entry.descriptor->outputs.contains(name))
            continue;
        if (auto inst = instantiate(entry, name))
            result.append(*inst);
    }
    return result;
}

bool CalculationRegistry::hasCandidateFor(const DependencyKey &name) const
{
    return !candidatesFor(name).isEmpty();
}

QList<CalculationInstance> CalculationRegistry::sourceConversionsFor(const QString &sensor,
                                                                    const QString &name) const
{
    QList<CalculationInstance> result;
    const DependencyKey key = DependencyKey::measurement(sensor, name);
    for (const Entry &entry : m_entries) {
        if (entry.kind != EntryKind::SourceConversion)
            continue;
        if (auto inst = instantiate(entry, key))
            result.append(*inst);
    }
    return result;
}

bool CalculationRegistry::hasSourceConversions() const
{
    return std::any_of(m_entries.cbegin(), m_entries.cend(),
                       [](const Entry &e) { return e.kind == EntryKind::SourceConversion; });
}

std::optional<CalculationInstance> CalculationRegistry::instance(const CalculationId &id,
                                                                 const DependencyKey &instanceOutput) const
{
    for (const Entry &entry : m_entries) {
        if (entry.id != id)
            continue;
        if (entry.kind == EntryKind::Calculation)
            return instantiate(entry, instanceOutput);
        if (isEmptyName(instanceOutput))
            return std::nullopt;    // a family needs a name to select the instance
        return instantiate(entry, instanceOutput);
    }
    return std::nullopt;
}

int CalculationRegistry::memoizedInstanceCount(const CalculationId &familyId) const
{
    for (const Entry &entry : m_entries) {
        if (entry.id == familyId)
            return int(entry.memo.size());
    }
    return 0;
}

// ------------------------------------------------------ preferences and engines

void CalculationRegistry::setPreferenceProvider(const IPreferenceProvider *p)
{
    m_preferenceProvider = p;
}

const IPreferenceProvider *CalculationRegistry::preferenceProvider() const
{
    return m_preferenceProvider;
}

void CalculationRegistry::notifyPreferenceChanged(const QString &key)
{
    const QList<CalculationEngine *> engines = m_engines;
    for (CalculationEngine *e : engines) {
        if (m_engines.contains(e))      // a listener may have destroyed a session
            e->onPreferenceChanged(key);
    }
}

void CalculationRegistry::broadcast(const RegistryChange &change)
{
    const QList<CalculationEngine *> engines = m_engines;
    for (CalculationEngine *e : engines) {
        if (m_engines.contains(e))
            e->onRegistryChanged(change);
    }

    // Every successful register* / unregister ends here.
    registrationsChanged();
}

void CalculationRegistry::enrol(CalculationEngine *e)
{
    if (!m_engines.contains(e))
        m_engines.append(e);
}

void CalculationRegistry::withdraw(CalculationEngine *e)
{
    m_engines.removeAll(e);
}

// ------------------------------------------ registration-derived queries
// (logbook column cache: which columns can an edit affect, and when did the
// calculation environment change). Nothing below touches an engine or a
// session, or runs a calculation.

StaticDependencies CalculationRegistry::staticDependencies(const DependencyKey &name) const
{
    const auto memoized = m_staticDependencyMemo.constFind(name);
    if (memoized != m_staticDependencyMemo.constEnd())
        return *memoized;

    StaticDependencies result;
    QSet<DependencyKey> visited;    // expanded names; result.names also holds source-layer leaves
    QList<DependencyKey> worklist{name};

    while (!worklist.isEmpty()) {
        const DependencyKey current = worklist.takeLast();
        if (visited.contains(current))
            continue;       // the visited set also ends cycles
        visited.insert(current);
        result.names.insert(current);

        // Every candidate, not only the one that would win
        QList<CalculationInstance> candidates = candidatesFor(current);
        if (current.type == DependencyKey::Type::Measurement) {
            candidates += sourceConversionsFor(current.measurementKey.first,
                                               current.measurementKey.second);
        }

        for (const CalculationInstance &candidate : std::as_const(candidates)) {
            for (const CalcInput &in : candidate.descriptor->inputs) {
                switch (in.kind) {
                case CalcInput::Kind::Attribute:
                    worklist.append(DependencyKey::attribute(in.key));
                    break;
                case CalcInput::Kind::Measurement:
                    worklist.append(DependencyKey::measurement(in.sensor, in.name));
                    break;
                case CalcInput::Kind::Preference:
                    result.preferences.insert(in.key);
                    break;
                case CalcInput::Kind::SourceMeasurement:
                case CalcInput::Kind::SourceUnit:
                    // A leaf: the source layer behind a public measurement name
                    result.names.insert(DependencyKey::measurement(in.sensor, in.name));
                    break;
                }
            }
        }
    }

    m_staticDependencyMemo.insert(name, result);
    return result;
}

QStringList CalculationRegistry::declaredPreferenceKeys() const
{
    QSet<QString> keys;
    for (const Entry &entry : m_entries) {
        if (entry.kind != EntryKind::Calculation)
            continue;       // family instances are not enumerable (see the header)
        for (const CalcInput &in : entry.descriptor->inputs) {
            if (in.kind == CalcInput::Kind::Preference)
                keys.insert(in.key);
        }
    }

    QStringList sorted(keys.cbegin(), keys.cend());
    sorted.sort();
    return sorted;
}

CandidateOrder CalculationRegistry::candidateOrder() const
{
    CandidateOrder order;

    // Same walk as candidatesFor(): m_entries is in sequence order, and a
    // family takes its place among the plain candidates of every name.
    QMap<DependencyKey, QList<CalculationId>> byOutput;
    for (const Entry &entry : m_entries) {
        if (entry.kind != EntryKind::Calculation)
            continue;
        for (const DependencyKey &out : entry.descriptor->outputs)
            byOutput.insert(out, {});
    }

    for (const Entry &entry : m_entries) {
        switch (entry.kind) {
        case EntryKind::SourceConversion:
            order.sourceConversions.append(entry.id);
            break;
        case EntryKind::Family:
            order.families.append(entry.id);
            for (auto it = byOutput.begin(); it != byOutput.end(); ++it)
                it->append(entry.id);
            break;
        case EntryKind::Calculation:
            for (const DependencyKey &out : entry.descriptor->outputs)
                byOutput[out].append(entry.id);
            break;
        }
    }

    order.byOutput.reserve(byOutput.size());
    for (auto it = byOutput.cbegin(); it != byOutput.cend(); ++it)
        order.byOutput.append({it.key(), it.value()});
    return order;
}

int CalculationRegistry::addObserver(std::function<void()> observer)
{
    const int token = m_nextObserverToken++;
    m_observers.append({token, std::move(observer)});
    return token;
}

void CalculationRegistry::removeObserver(int token)
{
    m_observers.removeIf([token](const std::pair<int, std::function<void()>> &o) {
        return o.first == token;
    });
}

void CalculationRegistry::registrationsChanged()
{
    m_staticDependencyMemo.clear();

    // A copy: an observer may remove itself (or another observer).
    const auto observers = m_observers;
    for (const auto &observer : observers) {
        if (observer.second)
            observer.second();
    }
}

} // namespace FlySight
