#ifndef FLYSIGHTTEST_STOREDRESULTS_H
#define FLYSIGHTTEST_STOREDRESULTS_H

#include <QByteArray>
#include <QString>

#include "engine/storedcalculationresult.h"
#include "plugincodeidentity.h"

/// Values the stored-results suites build by hand: a resolution of a snapshot,
/// and the plug-in code identity of a stand-in plug-in folder (one process
/// cannot boot the Python interpreter twice, so the suites register C++
/// calculations that declare a real identity, as the plug-in host would).
namespace FlySightTest {

/// A StoredResolution with every member given (instance id and result version
/// empty unless the provider is Calculation).
FlySight::StoredResolution storedResolution(const FlySight::DependencyKey &name,
                                            FlySight::StoredResolution::Provider provider,
                                            const QString &instanceId = QString(),
                                            const QString &resultVersion = QString());

/// The ingredients of a stand-in plug-in folder: a_plugin.py holding `aPlugin`,
/// helper.py holding "y = 2\n", the SDK bytes "sdk", Python 3.13.3 and numpy
/// 2.2.4. A suite varies one ingredient from here.
FlySight::PluginCodeIngredients standInPluginIngredients(const QByteArray &aPlugin = QByteArray("x = 1\n"));

/// pluginCodeIdentity(standInPluginIngredients(aPlugin)).
QString standInPluginIdentity(const QByteArray &aPlugin = QByteArray("x = 1\n"));

} // namespace FlySightTest

#endif // FLYSIGHTTEST_STOREDRESULTS_H
