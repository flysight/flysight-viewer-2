#ifndef FLYSIGHT_ENGINE_CALCULATIONREGISTRY_H
#define FLYSIGHT_ENGINE_CALCULATIONREGISTRY_H

#include <functional>
#include <memory>
#include <optional>

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include "calctypes.h"
#include "calculationdescriptor.h"
#include "sessionstate.h"

namespace FlySight {

class CalculationEngine;

/// What the engine consumes: one concrete calculation, either a plain
/// registration or one instance of a family.
struct CalculationInstance {
    QString       instanceId;       ///< id, or "<familyId>#<instanceKey>"
    CalculationId registrationId;   ///< id or familyId (run counters, unregister)
    std::shared_ptr<const CalculationDescriptor> descriptor;   ///< descriptor->id == instanceId
    bool          sourceConversion = false;
};

/// Describes one successful register / unregister to the enrolled engines.
struct RegistryChange {
    enum class Kind { Calculation, Family, SourceConversion };

    CalculationId registrationId;
    bool added = true;
    Kind kind = Kind::Calculation;
    QList<DependencyKey> outputs;   ///< Calculation: its explicit output names
    /// Family / SourceConversion: the family's instantiate function
    std::function<std::optional<CalculationDescriptor>(const DependencyKey &name)> instantiate;
};

/// Everything a public name can depend on according to the registrations
/// alone (see CalculationRegistry::staticDependencies).
struct StaticDependencies {
    QSet<DependencyKey> names;      ///< every public name reachable through declared inputs, INCLUDING the queried name
    QSet<QString> preferences;      ///< every declared preference key reachable
};

/// The part of the registration order that resolution can observe (see
/// CalculationRegistry::candidateOrder).
struct CandidateOrder {
    /// Per output name declared by a plain calculation, sorted by name: what is
    /// tried for it, in the order it is tried - the plain calculations
    /// declaring the name interleaved with EVERY family (which names a family
    /// accepts is not enumerable, so each one may be a candidate).
    QList<std::pair<DependencyKey, QList<CalculationId>>> byOutput;
    QList<CalculationId> families;              ///< what is tried for any other name
    QList<CalculationId> sourceConversions;     ///< what is tried for a measurement with source data
};

/// Global, session-free registrations in deterministic order.
///
/// The registry holds no per-session data and never runs a calculation. It
/// knows which engines (one per loaded session) are using it so that registry
/// and preference changes can invalidate affected results in every session.
///
/// Order: every successful registration takes the next value of an increasing
/// sequence; candidates for a name are tried in that order. There are no
/// priorities. Re-registering after unregister() goes to the end.
///
/// A registry must outlive the engines bound to it. Single-threaded.
class CalculationRegistry {
public:
    /// Process-wide registry used by the application.
    static CalculationRegistry &instance();

    CalculationRegistry();      ///< tests construct private registries
    ~CalculationRegistry();     ///< asserts that no engine is still enrolled
    Q_DISABLE_COPY_MOVE(CalculationRegistry)

    // Each returns false, warns, and registers nothing when the registration is
    // invalid: empty id; '#' in the id; id already registered (as any kind);
    // no outputs; duplicate outputs; null compute / instantiate; an output that
    // is also one of the calculation's own Attribute / Measurement inputs; a
    // SourceMeasurement / SourceUnit input on anything that is not a source
    // conversion; or a call made while an engine is evaluating.
    bool registerCalculation(const CalculationDescriptor &d);
    bool registerFamily(const CalculationFamily &f);
    /// Source conversions are the ordered candidates for a measurement that has
    /// source data. `instantiate` receives DependencyKey::measurement(sensor, name).
    bool registerSourceConversion(const CalculationFamily &f);
    bool unregister(const CalculationId &id);   ///< calculation, family, or conversion family

    bool contains(const CalculationId &id) const;
    /// Interface text for a registration: the descriptor's title for a plain
    /// calculation; the id when that title is empty or when `id` is a family
    /// (an instance carries its own title); an empty string for an unknown id.
    QString title(const CalculationId &id) const;
    /// Every registration (plain calculations, families, and conversion
    /// families) in sequence order.
    QList<CalculationId> registeredIds() const;
    /// True when `id` is registered as a family or a source-conversion family.
    bool isFamily(const CalculationId &id) const;
    bool hasCandidateFor(const DependencyKey &name) const;
    /// Plain calculations declaring `name` and family instances accepting it,
    /// in registration order. Source conversions are not included.
    QList<CalculationInstance> candidatesFor(const DependencyKey &name) const;
    QList<CalculationInstance> sourceConversionsFor(const QString &sensor, const QString &name) const;
    bool hasSourceConversions() const;
    /// A plain calculation by id, or - with `instanceOutput` - the family
    /// instance that produces that name.
    std::optional<CalculationInstance> instance(const CalculationId &id,
                                                const DependencyKey &instanceOutput = DependencyKey::attribute(QString())) const;

    void setPreferenceProvider(const IPreferenceProvider *p);   ///< not owned; may be null
    const IPreferenceProvider *preferenceProvider() const;
    /// Broadcast to every enrolled engine. Call after the preference changed.
    void notifyPreferenceChanged(const QString &key);

    // Introspection used by tests
    int enrolledEngineCount() const { return int(m_engines.size()); }
    int memoizedInstanceCount(const CalculationId &familyId) const;

    // ---- Registration-derived queries for the logbook column cache ---------
    // Pure functions of the registrations: none of them touches an engine, a
    // session, or runs a calculation.

    /// The closure of `name` over declared inputs, following EVERY candidate
    /// for every name (not only the one that would win - which one wins
    /// depends on session state) and, for measurements, every source
    /// conversion. Attribute / Measurement inputs are followed; a Preference
    /// input contributes its key; a SourceMeasurement / SourceUnit input
    /// contributes DependencyKey::measurement(sensor, name), the public name
    /// whose source layer it reads.
    ///
    /// The result is a superset of any dynamic dependency set an engine can
    /// record for `name`, independent of session state and of what is cached.
    /// That makes it safe for deciding which logbook columns an edit can affect
    /// in rows whose engine is cold or which are not loaded at all. Memoized
    /// per name; the memo is dropped by every successful register* / unregister.
    StaticDependencies staticDependencies(const DependencyKey &name) const;

    /// Preference keys declared as inputs, sorted and unique. Plain
    /// calculations only: family instances are not enumerable. No family
    /// declares a preference today; one that does has to be added here
    /// explicitly, or changes of that preference will not reach the cached
    /// logbook columns of unloaded sessions.
    QStringList declaredPreferenceKeys() const;

    /// Registration order as far as it can decide which candidate wins, and
    /// nothing more: two plain calculations that share no output are never
    /// candidates for the same name, so their relative order is not part of
    /// the result. Registries with equal results try the same candidates in
    /// the same order for every name.
    CandidateOrder candidateOrder() const;

    /// `observer` is called after every successful register* / unregister,
    /// after the enrolled engines were notified. Plain callbacks (this library
    /// has no QObject), invoked synchronously; an observer must not register or
    /// unregister. Returns a token for removeObserver().
    int addObserver(std::function<void()> observer);
    void removeObserver(int token);

private:
    friend class CalculationEngine;

    enum class EntryKind { Calculation, Family, SourceConversion };

    struct Entry {
        EntryKind kind = EntryKind::Calculation;
        CalculationId id;
        std::shared_ptr<const CalculationDescriptor> descriptor;    // Calculation
        CalculationFamily family;                                   // Family / SourceConversion
        // Registration-derived, not session state: instances by instance key.
        mutable QHash<QString, CalculationInstance> memo;
    };

    void enrol(CalculationEngine *e);       // called by the engine constructor
    void withdraw(CalculationEngine *e);    // called by the engine destructor
    void evaluationStarted() { ++m_activeEvaluations; }
    void evaluationFinished() { --m_activeEvaluations; }

    bool checkMutable(const char *what, const CalculationId &id) const;
    bool validate(const CalculationDescriptor &d, bool sourceInputsPermitted, const QString &label) const;
    bool addFamily(const CalculationFamily &f, EntryKind kind);
    std::optional<CalculationInstance> instantiate(const Entry &entry, const DependencyKey &name) const;
    void broadcast(const RegistryChange &change);

    QList<Entry> m_entries;                 // in registration order
    QList<CalculationEngine *> m_engines;
    const IPreferenceProvider *m_preferenceProvider = nullptr;
    int m_activeEvaluations = 0;

    // Registration-derived queries (see above)
    void registrationsChanged();            // drops the memo, then calls the observers
    mutable QHash<DependencyKey, StaticDependencies> m_staticDependencyMemo;
    QList<std::pair<int, std::function<void()>>> m_observers;
    int m_nextObserverToken = 1;
};

} // namespace FlySight

#endif // FLYSIGHT_ENGINE_CALCULATIONREGISTRY_H
