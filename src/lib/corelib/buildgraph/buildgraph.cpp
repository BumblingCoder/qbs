/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qbs.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/
#include "buildgraph.h"

#include "artifact.h"
#include "artifactsscriptvalue.h"
#include "cycledetector.h"
#include "dependencyparametersscriptvalue.h"
#include "projectbuilddata.h"
#include "productbuilddata.h"
#include "rulenode.h"
#include "scriptclasspropertyiterator.h"
#include "transformer.h"

#include <jsextensions/jsextensions.h>
#include <jsextensions/moduleproperties.h>
#include <language/language.h>
#include <language/preparescriptobserver.h>
#include <language/propertymapinternal.h>
#include <language/resolvedfilecontext.h>
#include <language/scriptengine.h>
#include <logging/categories.h>
#include <logging/logger.h>
#include <language/property.h>
#include <logging/translator.h>
#include <tools/error.h>
#include <tools/fileinfo.h>
#include <tools/scripttools.h>
#include <tools/qbsassert.h>
#include <tools/qttools.h>
#include <tools/stringconstants.h>

#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtScript/qscriptclass.h>

#include <algorithm>
#include <vector>

namespace qbs {
namespace Internal {

// TODO: Introduce productscriptvalue.{h,cpp}.
static QScriptValue getDataForProductScriptValue(QScriptEngine *engine,
                                                 const ResolvedProduct *product)
{
    QScriptValue data = engine->newObject();
    QVariant v;
    v.setValue<quintptr>(reinterpret_cast<quintptr>(product));
    data.setProperty(ProductPtrKey, engine->newVariant(v));
    return data;
}

class ProductPropertyScriptClass : public QScriptClass
{
public:
    ProductPropertyScriptClass(QScriptEngine *engine) : QScriptClass(engine) { }

private:
    QueryFlags queryProperty(const QScriptValue &object, const QScriptString &name, QueryFlags,
                             uint *) override
    {
        if (name == StringConstants::parametersProperty()) {
            m_result = object.data().property(DependencyParametersKey);
            return HandlesReadAccess;
        }
        if (name == StringConstants::moduleNameProperty()) {
            m_result = object.data().property(ModuleNameKey);
            return HandlesReadAccess;
        }
        if (name == StringConstants::dependenciesProperty()
                || name == StringConstants::artifactsProperty()) {
            // The prototype is not backed by a QScriptClass.
            m_result = object.prototype().property(name);
            return HandlesReadAccess;
        }

        getProduct(object);
        QBS_ASSERT(m_product, return QueryFlags());

        const auto it = m_product->productProperties.find(name);

        // It is important that we reject unknown property names. Otherwise QtScript will forward
        // *everything* to us, including built-in stuff like the hasOwnProperty function.
        if (it == m_product->productProperties.cend())
            return QueryFlags();

        qbsEngine()->addPropertyRequestedInScript(Property(m_product->uniqueName(), QString(), name,
                it.value(), Property::PropertyInProduct));
        m_result = qbsEngine()->toScriptValue(it.value());
        return HandlesReadAccess;
    }

    QScriptValue property(const QScriptValue &, const QScriptString &, uint) override
    {
        return m_result;
    }

    QScriptClassPropertyIterator *newIterator(const QScriptValue &object) override
    {
        getProduct(object);
        QBS_ASSERT(m_product, return nullptr);

        // These two are in the prototype and are thus common to all product script values.
        std::vector<QString> additionalProperties({StringConstants::artifactsProperty(),
                                                  StringConstants::dependenciesProperty()});

        // The "moduleName" convenience property is only available for the "main product" in a rule,
        // and the "parameters" property exists only for elements of the "dependencies" array for
        // which dependency parameters are present.
        if (object.data().property(ModuleNameKey).isValid())
            additionalProperties.push_back(StringConstants::moduleNameProperty());
        else if (object.data().property(DependencyParametersKey).isValid())
            additionalProperties.push_back(StringConstants::parametersProperty());
        return new ScriptClassPropertyIterator(object, m_product->productProperties,
                                               additionalProperties);
    }

    void getProduct(const QScriptValue &object)
    {
        if (m_lastObjectId != object.objectId()) {
            m_lastObjectId = object.objectId();
            m_product = reinterpret_cast<const ResolvedProduct *>(
                        object.data().property(ProductPtrKey).toVariant().value<quintptr>());
        }
    }

    ScriptEngine *qbsEngine() const { return static_cast<ScriptEngine *>(engine()); }

    qint64 m_lastObjectId = 0;
    const ResolvedProduct *m_product = nullptr;
    QScriptValue m_result;
};

static QScriptValue setupProjectScriptValue(ScriptEngine *engine,
        const ResolvedProjectConstPtr &project)
{
    QScriptValue &obj = engine->projectScriptValue(project.get());
    if (obj.isValid())
        return obj;
    obj = engine->newObject();
    obj.setProperty(StringConstants::filePathProperty(), project->location.filePath());
    obj.setProperty(StringConstants::pathProperty(), FileInfo::path(project->location.filePath()));
    const QVariantMap &projectProperties = project->projectProperties();
    for (QVariantMap::const_iterator it = projectProperties.begin();
            it != projectProperties.end(); ++it) {
        engine->setObservedProperty(obj, it.key(), engine->toScriptValue(it.value()));
    }
    engine->observer()->addProjectObjectId(obj.objectId(), project->name);
    return obj;
}

static QScriptValue setupProductScriptValue(ScriptEngine *engine, const ResolvedProduct *product);

class DependenciesFunction
{
public:
    DependenciesFunction(ScriptEngine *engine)
        : m_engine(engine)
    {
    }

    void init(QScriptValue &productScriptValue, const ResolvedProduct *product)
    {
        QScriptValue depfunc = m_engine->newFunction(&js_productDependencies, product);
        productScriptValue.setProperty(StringConstants::dependenciesProperty(), depfunc,
                                       QScriptValue::ReadOnly | QScriptValue::Undeletable
                                       | QScriptValue::PropertyGetter);
    }

private:
    static QScriptValue js_productDependencies(QScriptContext *, ScriptEngine *engine,
                                               const ResolvedProduct * const product)
    {
        engine->addDependenciesArrayRequested(product);
        QScriptValue result = engine->newArray();
        quint32 idx = 0;
        QList<ResolvedProductPtr> productDeps = product->dependencies.toList();
        std::sort(productDeps.begin(), productDeps.end(),
                  [](const ResolvedProductPtr &p1, const ResolvedProductPtr &p2) {
                          return p1->name < p2->name;
                  });
        for (const ResolvedProductPtr &dependency : qAsConst(productDeps)) {
            QScriptValue obj = engine->newObject(engine->productPropertyScriptClass());
            obj.setPrototype(setupProductScriptValue(static_cast<ScriptEngine *>(engine),
                                                     dependency.get()));
            const QVariantMap &params = product->dependencyParameters.value(dependency);
            QScriptValue data = getDataForProductScriptValue(engine, dependency.get());
            data.setProperty(DependencyParametersKey, dependencyParametersValue(
                                 product->uniqueName(), dependency->name, params, engine));
            obj.setData(data);
            result.setProperty(idx++, obj);
        }
        for (const ResolvedModuleConstPtr &dependency : qAsConst(product->modules)) {
            if (dependency->isProduct)
                continue;
            QScriptValue obj = engine->newObject(engine->modulePropertyScriptClass());
            obj.setPrototype(engine->moduleScriptValuePrototype(dependency.get()));

            // The prototype must exist already, because we set it up for all modules
            // of the product in ModuleProperties::init().
            QBS_ASSERT(obj.prototype().isValid(), ;);

            const QVariantMap &params = product->moduleParameters.value(dependency);
            QScriptValue data = getDataForModuleScriptValue(engine, product, nullptr,
                                                            dependency.get());
            data.setProperty(DependencyParametersKey, dependencyParametersValue(
                                 product->uniqueName(), dependency->name, params, engine));
            obj.setData(data);
            result.setProperty(idx++, obj);
        }
        return result;
    }

    ScriptEngine *m_engine;
};

static QScriptValue setupProductScriptValue(ScriptEngine *engine, const ResolvedProduct *product)
{
    QScriptValue &productScriptValue = engine->productScriptValuePrototype(product);
    if (productScriptValue.isValid())
        return productScriptValue;
    productScriptValue = engine->newObject();
    ModuleProperties::init(productScriptValue, product);

    QScriptValue artifactsFunc = engine->newFunction(&artifactsScriptValueForProduct, product);
    productScriptValue.setProperty(StringConstants::artifactsProperty(), artifactsFunc,
                                   QScriptValue::ReadOnly | QScriptValue::Undeletable
                                   | QScriptValue::PropertyGetter);

    DependenciesFunction(engine).init(productScriptValue, product);
    return productScriptValue;
}

void setupScriptEngineForFile(ScriptEngine *engine, const FileContextBaseConstPtr &fileContext,
        QScriptValue targetObject, const ObserveMode &observeMode)
{
    engine->import(fileContext, targetObject, observeMode);
    JsExtensions::setupExtensions(fileContext->jsExtensions(), targetObject);
}

void setupScriptEngineForProduct(ScriptEngine *engine, ResolvedProduct *product,
                                 const ResolvedModule *module, QScriptValue targetObject,
                                 bool setBuildEnvironment)
{
    QScriptValue projectScriptValue = setupProjectScriptValue(engine, product->project.lock());
    targetObject.setProperty(StringConstants::projectVar(), projectScriptValue);

    if (setBuildEnvironment) {
        QVariant v;
        v.setValue<void*>(&product->buildEnvironment);
        engine->setProperty(StringConstants::qbsProcEnvVarInternal(), v);
    }
    QScriptClass *scriptClass = engine->productPropertyScriptClass();
    if (!scriptClass) {
        scriptClass = new ProductPropertyScriptClass(engine);
        engine->setProductPropertyScriptClass(scriptClass);
    }
    QScriptValue productScriptValue = engine->newObject(scriptClass);
    productScriptValue.setPrototype(setupProductScriptValue(engine, product));
    targetObject.setProperty(StringConstants::productVar(), productScriptValue);

    QScriptValue data = getDataForProductScriptValue(engine, product);
    // If the Rule is in a Module, set up the 'moduleName' property
    if (!module->name.isEmpty())
        data.setProperty(ModuleNameKey, module->name);
    productScriptValue.setData(data);
}

bool findPath(BuildGraphNode *u, BuildGraphNode *v, QList<BuildGraphNode *> &path)
{
    if (u == v) {
        path.push_back(v);
        return true;
    }

    for (BuildGraphNode * const childNode : qAsConst(u->children)) {
        if (findPath(childNode, v, path)) {
            path.prepend(u);
            return true;
        }
    }

    return false;
}

/*
 * Creates the build graph edge p -> c, which represents the dependency "c must be built before p".
 */
void connect(BuildGraphNode *p, BuildGraphNode *c)
{
    QBS_CHECK(p != c);
    qCDebug(lcBuildGraph) << "connect" << p->toString() << "->" << c->toString();
    if (c->type() == BuildGraphNode::ArtifactNodeType) {
        Artifact * const ac = static_cast<Artifact *>(c);
        for (const Artifact *child : filterByType<Artifact>(p->children)) {
            if (child == ac)
                return;
            const bool filePathsMustBeDifferent = child->artifactType == Artifact::Generated
                    || child->product == ac->product || child->artifactType != ac->artifactType;
            if (filePathsMustBeDifferent && child->filePath() == ac->filePath()) {
                throw ErrorInfo(QString::fromLatin1("%1 already has a child artifact %2 as "
                                                    "different object.").arg(p->toString(),
                                                                             ac->filePath()),
                                CodeLocation(), true);
            }
        }
    }
    p->children.insert(c);
    c->parents.insert(p);
    p->product->topLevelProject()->buildData->isDirty = true;
}

static bool existsPath_impl(BuildGraphNode *u, BuildGraphNode *v, NodeSet *seen)
{
    if (u == v)
        return true;

    if (!seen->insert(u).second)
        return false;

    for (BuildGraphNode * const childNode : qAsConst(u->children)) {
        if (existsPath_impl(childNode, v, seen))
            return true;
    }

    return false;
}

static bool existsPath(BuildGraphNode *u, BuildGraphNode *v)
{
    NodeSet seen;
    return existsPath_impl(u, v, &seen);
}

static QStringList toStringList(const QList<BuildGraphNode *> &path)
{
    QStringList lst;
    for (BuildGraphNode *node : path)
        lst << node->toString();
    return lst;
}

bool safeConnect(Artifact *u, Artifact *v)
{
    QBS_CHECK(u != v);
    qCDebug(lcBuildGraph) << "safeConnect:" << relativeArtifactFileName(u)
                          << "->" << relativeArtifactFileName(v);

    if (existsPath(v, u)) {
        QList<BuildGraphNode *> circle;
        findPath(v, u, circle);
        qCDebug(lcBuildGraph) << "safeConnect: circle detected " << toStringList(circle);
        return false;
    }

    connect(u, v);
    return true;
}

void disconnect(BuildGraphNode *u, BuildGraphNode *v)
{
    qCDebug(lcBuildGraph) << "disconnect:" << u->toString() << v->toString();
    u->children.remove(v);
    v->parents.remove(u);
    u->onChildDisconnected(v);
}

void removeGeneratedArtifactFromDisk(Artifact *artifact, const Logger &logger)
{
    if (artifact->artifactType != Artifact::Generated)
        return;
    removeGeneratedArtifactFromDisk(artifact->filePath(), logger);
}

void removeGeneratedArtifactFromDisk(const QString &filePath, const Logger &logger)
{
    QFile file(filePath);
    if (!file.exists())
        return;
    logger.qbsDebug() << "removing " << filePath;
    if (!file.remove())
        logger.qbsWarning() << QString::fromLatin1("Cannot remove '%1'.").arg(filePath);
}

QString relativeArtifactFileName(const Artifact *artifact)
{
    const QString &buildDir = artifact->product->topLevelProject()->buildDirectory;
    QString str = artifact->filePath();
    if (str.startsWith(buildDir))
        str.remove(0, buildDir.size());
    if (str.startsWith(QLatin1Char('/')))
        str.remove(0, 1);
    return str;
}

Artifact *lookupArtifact(const ResolvedProductConstPtr &product,
        const ProjectBuildData *projectBuildData, const QString &dirPath, const QString &fileName,
        bool compareByName)
{
    const QList<FileResourceBase *> lookupResults
            = projectBuildData->lookupFiles(dirPath, fileName);
    for (QList<FileResourceBase *>::const_iterator it = lookupResults.constBegin();
            it != lookupResults.constEnd(); ++it) {
        if ((*it)->fileType() != FileResourceBase::FileTypeArtifact)
            continue;
        Artifact *artifact = static_cast<Artifact *>(*it);
        if (compareByName
                ? artifact->product->uniqueName() == product->uniqueName()
                : artifact->product == product) {
            return artifact;
        }
    }
    return nullptr;
}

Artifact *lookupArtifact(const ResolvedProductConstPtr &product, const QString &dirPath,
                         const QString &fileName, bool compareByName)
{
    return lookupArtifact(product, product->topLevelProject()->buildData.get(), dirPath, fileName,
                          compareByName);
}

Artifact *lookupArtifact(const ResolvedProductConstPtr &product, const QString &filePath,
                         bool compareByName)
{
    QString dirPath, fileName;
    FileInfo::splitIntoDirectoryAndFileName(filePath, &dirPath, &fileName);
    return lookupArtifact(product, dirPath, fileName, compareByName);
}

Artifact *lookupArtifact(const ResolvedProductConstPtr &product, const ProjectBuildData *buildData,
                         const QString &filePath, bool compareByName)
{
    QString dirPath, fileName;
    FileInfo::splitIntoDirectoryAndFileName(filePath, &dirPath, &fileName);
    return lookupArtifact(product, buildData, dirPath, fileName, compareByName);
}

Artifact *lookupArtifact(const ResolvedProductConstPtr &product, const Artifact *artifact,
                         bool compareByName)
{
    return lookupArtifact(product, artifact->dirPath(), artifact->fileName(), compareByName);
}

Artifact *createArtifact(const ResolvedProductPtr &product,
                         const SourceArtifactConstPtr &sourceArtifact)
{
    auto artifact = new Artifact;
    artifact->artifactType = Artifact::SourceFile;
    artifact->targetOfModule = sourceArtifact->targetOfModule;
    artifact->setFilePath(sourceArtifact->absoluteFilePath);
    artifact->setFileTags(sourceArtifact->fileTags);
    artifact->properties = sourceArtifact->properties;
    insertArtifact(product, artifact);
    return artifact;
}

void insertArtifact(const ResolvedProductPtr &product, Artifact *artifact)
{
    qCDebug(lcBuildGraph) << "insert artifact" << artifact->filePath();
    QBS_CHECK(!artifact->product);
    QBS_CHECK(!artifact->filePath().isEmpty());
    artifact->product = product;
    product->topLevelProject()->buildData->insertIntoLookupTable(artifact);
    product->topLevelProject()->buildData->isDirty = true;
    product->buildData->addArtifact(artifact);
}

static void doSanityChecksForProduct(const ResolvedProductConstPtr &product,
        const Set<ResolvedProductPtr> &allProducts, const Logger &logger)
{
    qCDebug(lcBuildGraph) << "Sanity checking product" << product->uniqueName();
    CycleDetector cycleDetector(logger);
    cycleDetector.visitProduct(product);
    const ProductBuildData * const buildData = product->buildData.get();
    for (const ResolvedModuleConstPtr &m : qAsConst(product->modules))
        QBS_CHECK(m->product == product.get());
    qCDebug(lcBuildGraph) << "enabled:" << product->enabled << "build data:" << buildData;
    QBS_CHECK(!!product->enabled == !!buildData);
    if (!product->enabled)
        return;
    for (BuildGraphNode * const node : qAsConst(buildData->rootNodes())) {
        qCDebug(lcBuildGraph) << "Checking root node" << node->toString();
        QBS_CHECK(buildData->allNodes().contains(node));
    }
    Set<QString> filePaths;
    for (BuildGraphNode * const node : qAsConst(buildData->allNodes())) {
        qCDebug(lcBuildGraph) << "Sanity checking node" << node->toString();
        QBS_CHECK(node->product == product);
        for (const BuildGraphNode * const parent : qAsConst(node->parents))
            QBS_CHECK(parent->children.contains(node));
        for (BuildGraphNode * const child : qAsConst(node->children)) {
            QBS_CHECK(child->parents.contains(node));
            QBS_CHECK(!child->product.expired());
            QBS_CHECK(child->product->buildData);
            QBS_CHECK(child->product->buildData->allNodes().contains(child));
            QBS_CHECK(allProducts.contains(child->product.lock()));
        }

        Artifact * const artifact = node->type() == BuildGraphNode::ArtifactNodeType
                ? static_cast<Artifact *>(node) : nullptr;
        if (!artifact) {
            QBS_CHECK(node->type() == BuildGraphNode::RuleNodeType);
            RuleNode * const ruleNode = static_cast<RuleNode *>(node);
            QBS_CHECK(ruleNode->rule());
            QBS_CHECK(ruleNode->rule()->product);
            QBS_CHECK(ruleNode->rule()->product == ruleNode->product.get());
            QBS_CHECK(ruleNode->rule()->product == product.get());
            QBS_CHECK(product->rules.contains(std::const_pointer_cast<Rule>(ruleNode->rule())));
            continue;
        }

        QBS_CHECK(artifact->artifactType == Artifact::SourceFile ||
                  !filePaths.contains(artifact->filePath()));
        filePaths << artifact->filePath();

        for (Artifact * const child : qAsConst(artifact->childrenAddedByScanner))
            QBS_CHECK(artifact->children.contains(child));
        const TransformerConstPtr transformer = artifact->transformer;
        if (artifact->artifactType == Artifact::SourceFile)
            continue;

        QBS_CHECK(transformer);
        QBS_CHECK(transformer->rule);
        QBS_CHECK(transformer->rule->product);
        QBS_CHECK(transformer->rule->product == artifact->product.get());
        QBS_CHECK(transformer->rule->product == product.get());
        QBS_CHECK(transformer->outputs.contains(artifact));
        QBS_CHECK(product->rules.contains(std::const_pointer_cast<Rule>(transformer->rule)));
        qCDebug(lcBuildGraph)
                << "The transformer has" << transformer->outputs.size() << "outputs.";
        ArtifactSet transformerOutputChildren;
        for (const Artifact * const output : qAsConst(transformer->outputs)) {
            QBS_CHECK(output->transformer == transformer);
            transformerOutputChildren.unite(ArtifactSet::filtered(output->children));
            Set<QString> childFilePaths;
            for (const Artifact *a : filterByType<Artifact>(output->children)) {
                if (!childFilePaths.insert(a->filePath()).second) {
                    throw ErrorInfo(QString::fromLatin1("There is more than one artifact for "
                        "file '%1' in the child list for output '%2'.")
                        .arg(a->filePath(), output->filePath()), CodeLocation(), true);
                }
            }
        }
        if (lcBuildGraph().isDebugEnabled()) {
            qCDebug(lcBuildGraph) << "The transformer output children are:";
            for (const Artifact * const a : qAsConst(transformerOutputChildren))
                qCDebug(lcBuildGraph) << "\t" << a->fileName();
            qCDebug(lcBuildGraph) << "The transformer inputs are:";
            for (const Artifact * const a : qAsConst(transformer->inputs))
                qCDebug(lcBuildGraph) << "\t" << a->fileName();
        }
        QBS_CHECK(transformer->inputs.size() <= transformerOutputChildren.size());
        for (Artifact * const transformerInput : qAsConst(transformer->inputs))
            QBS_CHECK(transformerOutputChildren.contains(transformerInput));
    }
}

static void doSanityChecks(const ResolvedProjectPtr &project,
                           const Set<ResolvedProductPtr> &allProducts, Set<QString> &productNames,
                           const Logger &logger)
{
    logger.qbsDebug() << "Sanity checking project '" << project->name << "'";
    for (const ResolvedProjectPtr &subProject : qAsConst(project->subProjects))
        doSanityChecks(subProject, allProducts, productNames, logger);

    for (const ResolvedProductConstPtr &product : qAsConst(project->products)) {
        QBS_CHECK(product->project == project);
        QBS_CHECK(product->topLevelProject() == project->topLevelProject());
        doSanityChecksForProduct(product, allProducts, logger);
        QBS_CHECK(!productNames.contains(product->uniqueName()));
        productNames << product->uniqueName();
    }
}

void doSanityChecks(const ResolvedProjectPtr &project, const Logger &logger)
{
    if (qEnvironmentVariableIsEmpty("QBS_SANITY_CHECKS"))
        return;
    Set<QString> productNames;
    const Set<ResolvedProductPtr> allProducts
            = Set<ResolvedProductPtr>::fromList(project->allProducts());
    doSanityChecks(project, allProducts, productNames, logger);
}

} // namespace Internal
} // namespace qbs
