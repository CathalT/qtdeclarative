/****************************************************************************
**
** Copyright (C) 2012 Nokia Corporation and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this
** file. Please review the following information to ensure the GNU Lesser
** General Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU General
** Public License version 3.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of this
** file. Please review the following information to ensure the GNU General
** Public License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** Other Usage
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qqmlcompiler_p.h"

#include "qqmlpropertyvaluesource.h"
#include "qqmlcomponent.h"
#include <private/qmetaobjectbuilder_p.h>
#include <private/qfastmetabuilder_p.h>
#include "qqmlstringconverters_p.h"
#include "qqmlengine_p.h"
#include "qqmlengine.h"
#include "qqmlcontext.h"
#include "qqmlmetatype_p.h"
#include "qqmlcustomparser_p_p.h"
#include "qqmlcontext_p.h"
#include "qqmlcomponent_p.h"
#include <private/qqmljsast_p.h>
#include "qqmlvmemetaobject_p.h"
#include "qqmlexpression_p.h"
#include "qqmlproperty_p.h"
#include "qqmlrewrite_p.h"
#include "qqmlscriptstring.h"
#include "qqmlglobal_p.h"
#include "qqmlbinding_p.h"
#include <private/qv4compiler_p.h>

#include <QDebug>
#include <QPointF>
#include <QSizeF>
#include <QRectF>
#include <QAtomicInt>
#include <QtCore/qdebug.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qvarlengtharray.h>

Q_DECLARE_METATYPE(QList<int>)
Q_DECLARE_METATYPE(QList<qreal>)
Q_DECLARE_METATYPE(QList<bool>)
Q_DECLARE_METATYPE(QList<QString>)
Q_DECLARE_METATYPE(QList<QUrl>)

QT_BEGIN_NAMESPACE

DEFINE_BOOL_CONFIG_OPTION(compilerDump, QML_COMPILER_DUMP);
DEFINE_BOOL_CONFIG_OPTION(compilerStatDump, QML_COMPILER_STATS);

using namespace QQmlJS;
using namespace QQmlScript;
using namespace QQmlCompilerTypes;

static QString id_string(QLatin1String("id"));
static QString on_string(QLatin1String("on"));
static QString Changed_string(QLatin1String("Changed"));
static QString Component_import_string(QLatin1String("QML/Component"));
static QString qsTr_string(QLatin1String("qsTr"));
static QString qsTrId_string(QLatin1String("qsTrId"));

/*!
    Instantiate a new QQmlCompiler.
*/
QQmlCompiler::QQmlCompiler(QQmlPool *pool)
: pool(pool), output(0), engine(0), unitRoot(0), unit(0), cachedComponentTypeRef(-1),
  cachedTranslationContextIndex(-1), componentStats(0)
{
    if (compilerStatDump()) 
        componentStats = pool->New<ComponentStats>();
}

/*!
    Returns true if the last call to compile() caused errors.

    \sa errors()
*/
bool QQmlCompiler::isError() const
{
    return !exceptions.isEmpty();
}

/*!
    Return the list of errors from the last call to compile(), or an empty list
    if there were no errors.
*/
QList<QQmlError> QQmlCompiler::errors() const
{
    return exceptions;
}

/*!
    Returns true if \a name refers to an attached property, false otherwise.

    Attached property names are those that start with a capital letter.
*/
bool QQmlCompiler::isAttachedPropertyName(const QString &name)
{
    return isAttachedPropertyName(QHashedStringRef(&name));
}

bool QQmlCompiler::isAttachedPropertyName(const QHashedStringRef &name)
{
    return !name.isEmpty() && name.at(0).isUpper();
}

/*!
    Returns true if \a name refers to a signal property, false otherwise.

    Signal property names are those that start with "on", followed by a first
    character which is either a capital letter or one or more underscores followed
    by a capital letter, which is then followed by other allowed characters.

    Note that although ECMA-262r3 supports dollarsigns and escaped unicode
    character codes in property names, for simplicity and performance reasons
    QML only supports letters, numbers and underscores.
*/
bool QQmlCompiler::isSignalPropertyName(const QString &name)
{
    return isSignalPropertyName(QStringRef(&name));
}

bool QQmlCompiler::isSignalPropertyName(const QHashedStringRef &name)
{
    if (name.length() < 3) return false;
    if (!name.startsWith(on_string)) return false;
    int ns = name.length();
    for (int i = 2; i < ns; ++i) {
        const QChar curr = name.at(i);
        if (curr.unicode() == '_') continue;
        if (curr.isUpper()) return true;
        return false;
    }
    return false; // consists solely of underscores - invalid.
}

/*!
    \macro COMPILE_EXCEPTION
    \internal
    Inserts an error into the QQmlCompiler error list, and returns false
    (failure).

    \a token is used to source the error line and column, and \a desc is the
    error itself.  \a desc can be an expression that can be piped into QDebug.

    For example:

    \code
    COMPILE_EXCEPTION(property, tr("Error for property \"%1\"").arg(property->name));
    \endcode
*/
#define COMPILE_EXCEPTION_LOCATION(line, column, desc) \
    {  \
        QQmlError error; \
        error.setUrl(output->url); \
        error.setLine(line); \
        error.setColumn(column); \
        error.setDescription(desc.trimmed()); \
        exceptions << error; \
        return false; \
    }

#define COMPILE_EXCEPTION(token, desc) \
    COMPILE_EXCEPTION_LOCATION((token)->location.start.line, (token)->location.start.column, desc)

/*!
    \macro COMPILE_CHECK
    \internal
    Returns false if \a is false, otherwise does nothing.
*/
#define COMPILE_CHECK(a) \
    { \
        if (!a) return false; \
    }

/*!
    Returns true if literal \a v can be assigned to property \a prop, otherwise
    false.

    This test corresponds to action taken by genLiteralAssignment().  Any change
    made here, must have a corresponding action in genLiteralAssigment().
*/
bool QQmlCompiler::testLiteralAssignment(QQmlScript::Property *prop,
                                                 QQmlScript::Value *v)
{
    const QQmlScript::Variant &value = v->value;

    if (!prop->core.isWritable() && !prop->isReadOnlyDeclaration)
        COMPILE_EXCEPTION(v, tr("Invalid property assignment: \"%1\" is a read-only property").arg(prop->name().toString()));

    if (prop->core.isEnum()) {
        QMetaProperty p = prop->parent->metaObject()->property(prop->index);
        int enumValue;
        bool ok;
        if (p.isFlagType()) {
            enumValue = p.enumerator().keysToValue(value.asString().toUtf8().constData(), &ok);
        } else
            enumValue = p.enumerator().keyToValue(value.asString().toUtf8().constData(), &ok);

        if (!ok)
            COMPILE_EXCEPTION(v, tr("Invalid property assignment: unknown enumeration"));

        v->value = QQmlScript::Variant((double)enumValue);
        return true;
    }

    int type = prop->type;

    switch(type) {
        case QMetaType::QVariant:
            break;
        case QVariant::String:
            if (!v->value.isString()) COMPILE_EXCEPTION(v, tr("Invalid property assignment: string expected"));
            break;
        case QVariant::StringList: // we expect a string literal.  A string list is not a literal assignment.
            if (!v->value.isString()) COMPILE_EXCEPTION(v, tr("Invalid property assignment: string or string list expected"));
            break;
        case QVariant::ByteArray:
            if (!v->value.isString()) COMPILE_EXCEPTION(v, tr("Invalid property assignment: byte array expected"));
            break;
        case QVariant::Url:
            if (!v->value.isString()) COMPILE_EXCEPTION(v, tr("Invalid property assignment: url expected"));
            break;
        case QVariant::RegExp:
            COMPILE_EXCEPTION(v, tr("Invalid property assignment: regular expression expected; use /pattern/ syntax"));
            break;
        case QVariant::UInt:
            {
            bool ok = v->value.isNumber();
            if (ok) {
                double n = v->value.asNumber();
                if (double(uint(n)) != n)
                    ok = false;
            }
            if (!ok) COMPILE_EXCEPTION(v, tr("Invalid property assignment: unsigned int expected"));
            }
            break;
        case QVariant::Int:
            {
            bool ok = v->value.isNumber();
            if (ok) {
                double n = v->value.asNumber();
                if (double(int(n)) != n)
                    ok = false;
            }
            if (!ok) COMPILE_EXCEPTION(v, tr("Invalid property assignment: int expected"));
            }
            break;
        case QMetaType::Float:
            if (!v->value.isNumber()) COMPILE_EXCEPTION(v, tr("Invalid property assignment: number expected"));
            break;
        case QVariant::Double:
            if (!v->value.isNumber()) COMPILE_EXCEPTION(v, tr("Invalid property assignment: number expected"));
            break;
        case QVariant::Color:
            {
            bool ok;
            QQmlStringConverters::colorFromString(value.asString(), &ok);
            if (!ok) COMPILE_EXCEPTION(v, tr("Invalid property assignment: color expected"));
            }
            break;
#ifndef QT_NO_DATESTRING
        case QVariant::Date:
            {
            bool ok;
            QQmlStringConverters::dateFromString(value.asString(), &ok);
            if (!ok) COMPILE_EXCEPTION(v, tr("Invalid property assignment: date expected"));
            }
            break;
        case QVariant::Time:
            {
            bool ok;
            QQmlStringConverters::timeFromString(value.asString(), &ok);
            if (!ok) COMPILE_EXCEPTION(v, tr("Invalid property assignment: time expected"));
            }
            break;
        case QVariant::DateTime:
            {
            bool ok;
            QQmlStringConverters::dateTimeFromString(value.asString(), &ok);
            if (!ok) COMPILE_EXCEPTION(v, tr("Invalid property assignment: datetime expected"));
            }
            break;
#endif // QT_NO_DATESTRING
        case QVariant::Point:
        case QVariant::PointF:
            {
            bool ok;
            QQmlStringConverters::pointFFromString(value.asString(), &ok);
            if (!ok) COMPILE_EXCEPTION(v, tr("Invalid property assignment: point expected"));
            }
            break;
        case QVariant::Size:
        case QVariant::SizeF:
            {
            bool ok;
            QQmlStringConverters::sizeFFromString(value.asString(), &ok);
            if (!ok) COMPILE_EXCEPTION(v, tr("Invalid property assignment: size expected"));
            }
            break;
        case QVariant::Rect:
        case QVariant::RectF:
            {
            bool ok;
            QQmlStringConverters::rectFFromString(value.asString(), &ok);
            if (!ok) COMPILE_EXCEPTION(v, tr("Invalid property assignment: rect expected"));
            }
            break;
        case QVariant::Bool:
            {
            if (!v->value.isBoolean()) COMPILE_EXCEPTION(v, tr("Invalid property assignment: boolean expected"));
            }
            break;
        case QVariant::Vector3D:
            {
            QQmlInstruction::instr_storeVector3D::QVector3D v3;
            if (!QQmlStringConverters::createFromString(QMetaType::QVector3D, value.asString(), &v3, sizeof(v3)))
                COMPILE_EXCEPTION(v, tr("Invalid property assignment: 3D vector expected"));
            }
            break;
        case QVariant::Vector4D:
            {
            QQmlInstruction::instr_storeVector4D::QVector4D v4;
            if (!QQmlStringConverters::createFromString(QMetaType::QVector4D, value.asString(), &v4, sizeof(v4)))
                COMPILE_EXCEPTION(v, tr("Invalid property assignment: 4D vector expected"));
            }
            break;
        default:
            {
            // check if assigning a literal value to a list property.
            // in each case, check the singular, since an Array of the specified type
            // will not go via this literal assignment codepath.
            if (type == qMetaTypeId<QList<qreal> >()) {
                if (!v->value.isNumber()) {
                    COMPILE_EXCEPTION(v, tr("Invalid property assignment: real or array of reals expected"));
                }
                break;
            } else if (type == qMetaTypeId<QList<int> >()) {
                bool ok = v->value.isNumber();
                if (ok) {
                    double n = v->value.asNumber();
                    if (double(int(n)) != n)
                        ok = false;
                }
                if (!ok) COMPILE_EXCEPTION(v, tr("Invalid property assignment: int or array of ints expected"));
                break;
            } else if (type == qMetaTypeId<QList<bool> >()) {
                if (!v->value.isBoolean()) {
                    COMPILE_EXCEPTION(v, tr("Invalid property assignment: bool or array of bools expected"));
                }
                break;
            } else if (type == qMetaTypeId<QList<QString> >()) { // we expect a string literal.  A string list is not a literal assignment.
                if (!v->value.isString()) {
                    COMPILE_EXCEPTION(v, tr("Invalid property assignment: string or array of strings expected"));
                }
                break;
            } else if (type == qMetaTypeId<QList<QUrl> >()) {
                if (!v->value.isString()) {
                    COMPILE_EXCEPTION(v, tr("Invalid property assignment: url or array of urls expected"));
                }
                break;
            }

            // otherwise, check for existence of string converter to custom type
            QQmlMetaType::StringConverter converter = QQmlMetaType::customStringConverter(type);
            if (!converter)
                COMPILE_EXCEPTION(v, tr("Invalid property assignment: unsupported type \"%1\"").arg(QString::fromLatin1(QVariant::typeToName((QVariant::Type)type))));
            }
            break;
    }
    return true;
}

/*!
    Generate a store instruction for assigning literal \a v to property \a prop.

    Any literal assignment that is approved in testLiteralAssignment() must have
    a corresponding action in this method.
*/
void QQmlCompiler::genLiteralAssignment(QQmlScript::Property *prop,
                                                QQmlScript::Value *v)
{
    if (prop->core.isEnum()) {
        Q_ASSERT(v->value.isNumber());
        // Preresolved value
        int value = (int)v->value.asNumber();

        Instruction::StoreInteger instr;
        instr.propertyIndex = prop->index;
        instr.value = value;
        output->addInstruction(instr);
        return;
    }

    int type = prop->type;
    switch(type) {
        case QMetaType::QVariant:
            {
            if (v->value.isNumber()) {
                double n = v->value.asNumber();
                if (double(int(n)) == n) {
                    if (prop->core.isVMEProperty()) {
                        Instruction::StoreVarInteger instr;
                        instr.propertyIndex = prop->index;
                        instr.value = int(n);
                        output->addInstruction(instr);
                    } else {
                        Instruction::StoreVariantInteger instr;
                        instr.propertyIndex = prop->index;
                        instr.value = int(n);
                        output->addInstruction(instr);
                    }
                } else {
                    if (prop->core.isVMEProperty()) {
                        Instruction::StoreVarDouble instr;
                        instr.propertyIndex = prop->index;
                        instr.value = n;
                        output->addInstruction(instr);
                    } else {
                        Instruction::StoreVariantDouble instr;
                        instr.propertyIndex = prop->index;
                        instr.value = n;
                        output->addInstruction(instr);
                    }
                }
            } else if (v->value.isBoolean()) {
                if (prop->core.isVMEProperty()) {
                    Instruction::StoreVarBool instr;
                    instr.propertyIndex = prop->index;
                    instr.value = v->value.asBoolean();
                    output->addInstruction(instr);
                } else {
                    Instruction::StoreVariantBool instr;
                    instr.propertyIndex = prop->index;
                    instr.value = v->value.asBoolean();
                    output->addInstruction(instr);
                }
            } else {
                if (prop->core.isVMEProperty()) {
                    Instruction::StoreVar instr;
                    instr.propertyIndex = prop->index;
                    instr.value = output->indexForString(v->value.asString());
                    output->addInstruction(instr);
                } else {
                    Instruction::StoreVariant instr;
                    instr.propertyIndex = prop->index;
                    instr.value = output->indexForString(v->value.asString());
                    output->addInstruction(instr);
                }
            }
            }
            break;
        case QVariant::String:
            {
            Instruction::StoreString instr;
            instr.propertyIndex = prop->index;
            instr.value = output->indexForString(v->value.asString());
            output->addInstruction(instr);
            }
            break;
        case QVariant::StringList:
            {
            Instruction::StoreStringList instr;
            instr.propertyIndex = prop->index;
            instr.value = output->indexForString(v->value.asString());
            output->addInstruction(instr);
            }
            break;
        case QVariant::ByteArray:
            {
            Instruction::StoreByteArray instr;
            instr.propertyIndex = prop->index;
            instr.value = output->indexForByteArray(v->value.asString().toLatin1());
            output->addInstruction(instr);
            }
            break;
        case QVariant::Url:
            {
            Instruction::StoreUrl instr;
            QString string = v->value.asString();
            QUrl u = string.isEmpty() ? QUrl() : output->url.resolved(QUrl(string));
            instr.propertyIndex = prop->index;
            instr.value = output->indexForUrl(u);
            output->addInstruction(instr);
            }
            break;
        case QVariant::UInt:
            {
            Instruction::StoreInteger instr;
            instr.propertyIndex = prop->index;
            instr.value = uint(v->value.asNumber());
            output->addInstruction(instr);
            }
            break;
        case QVariant::Int:
            {
            Instruction::StoreInteger instr;
            instr.propertyIndex = prop->index;
            instr.value = int(v->value.asNumber());
            output->addInstruction(instr);
            }
            break;
        case QMetaType::Float:
            {
            Instruction::StoreFloat instr;
            instr.propertyIndex = prop->index;
            instr.value = float(v->value.asNumber());
            output->addInstruction(instr);
            }
            break;
        case QVariant::Double:
            {
            Instruction::StoreDouble instr;
            instr.propertyIndex = prop->index;
            instr.value = v->value.asNumber();
            output->addInstruction(instr);
            }
            break;
        case QVariant::Color:
            {
            Instruction::StoreColor instr;
            instr.propertyIndex = prop->index;
            instr.value = QQmlStringConverters::rgbaFromString(v->value.asString());
            output->addInstruction(instr);
            }
            break;
#ifndef QT_NO_DATESTRING
        case QVariant::Date:
            {
            Instruction::StoreDate instr;
            QDate d = QQmlStringConverters::dateFromString(v->value.asString());
            instr.propertyIndex = prop->index;
            instr.value = d.toJulianDay();
            output->addInstruction(instr);
            }
            break;
        case QVariant::Time:
            {
            Instruction::StoreTime instr;
            QTime time = QQmlStringConverters::timeFromString(v->value.asString());
            instr.propertyIndex = prop->index;
            Q_ASSERT(sizeof(instr.time) == sizeof(QTime));
            ::memcpy(&instr.time, &time, sizeof(QTime));
            output->addInstruction(instr);
            }
            break;
        case QVariant::DateTime:
            {
            Instruction::StoreDateTime instr;
            QDateTime dateTime = QQmlStringConverters::dateTimeFromString(v->value.asString());
            QTime time = dateTime.time();
            instr.propertyIndex = prop->index;
            instr.date = dateTime.date().toJulianDay();
            Q_ASSERT(sizeof(instr.time) == sizeof(QTime));
            ::memcpy(&instr.time, &time, sizeof(QTime));
            output->addInstruction(instr);
            }
            break;
#endif // QT_NO_DATESTRING
        case QVariant::Point:
            {
            Instruction::StorePoint instr;
            bool ok;
            QPoint point = QQmlStringConverters::pointFFromString(v->value.asString(), &ok).toPoint();
            instr.propertyIndex = prop->index;
            instr.point.xp = point.x();
            instr.point.yp = point.y();
            output->addInstruction(instr);
            }
            break;
        case QVariant::PointF:
            {
            Instruction::StorePointF instr;
            bool ok;
            QPointF point = QQmlStringConverters::pointFFromString(v->value.asString(), &ok);
            instr.propertyIndex = prop->index;
            instr.point.xp = point.x();
            instr.point.yp = point.y();
            output->addInstruction(instr);
            }
            break;
        case QVariant::Size:
            {
            Instruction::StoreSize instr;
            bool ok;
            QSize size = QQmlStringConverters::sizeFFromString(v->value.asString(), &ok).toSize();
            instr.propertyIndex = prop->index;
            instr.size.wd = size.width();
            instr.size.ht = size.height();
            output->addInstruction(instr);
            }
            break;
        case QVariant::SizeF:
            {
            Instruction::StoreSizeF instr;
            bool ok;
            QSizeF size = QQmlStringConverters::sizeFFromString(v->value.asString(), &ok);
            instr.propertyIndex = prop->index;
            instr.size.wd = size.width();
            instr.size.ht = size.height();
            output->addInstruction(instr);
            }
            break;
        case QVariant::Rect:
            {
            Instruction::StoreRect instr;
            bool ok;
            QRect rect = QQmlStringConverters::rectFFromString(v->value.asString(), &ok).toRect();
            instr.propertyIndex = prop->index;
            instr.rect.x1 = rect.left();
            instr.rect.y1 = rect.top();
            instr.rect.x2 = rect.right();
            instr.rect.y2 = rect.bottom();
            output->addInstruction(instr);
            }
            break;
        case QVariant::RectF:
            {
            Instruction::StoreRectF instr;
            bool ok;
            QRectF rect = QQmlStringConverters::rectFFromString(v->value.asString(), &ok);
            instr.propertyIndex = prop->index;
            instr.rect.xp = rect.left();
            instr.rect.yp = rect.top();
            instr.rect.w = rect.width();
            instr.rect.h = rect.height();
            output->addInstruction(instr);
            }
            break;
        case QVariant::Bool:
            {
            Instruction::StoreBool instr;
            bool b = v->value.asBoolean();
            instr.propertyIndex = prop->index;
            instr.value = b;
            output->addInstruction(instr);
            }
            break;
        case QVariant::Vector3D:
            {
            Instruction::StoreVector3D instr;
            instr.propertyIndex = prop->index;
            QQmlStringConverters::createFromString(QMetaType::QVector3D, v->value.asString(), &instr.vector, sizeof(instr.vector));
            output->addInstruction(instr);
            }
            break;
    case QVariant::Vector4D:
            {
            Instruction::StoreVector4D instr;
            instr.propertyIndex = prop->index;
            QQmlStringConverters::createFromString(QMetaType::QVector4D, v->value.asString(), &instr.vector, sizeof(instr.vector));
            output->addInstruction(instr);
            }
            break;
        default:
            {
            // generate single literal value assignment to a list property if required
            if (type == qMetaTypeId<QList<qreal> >()) {
                Instruction::StoreDoubleQList instr;
                instr.propertyIndex = prop->index;
                instr.value = v->value.asNumber();
                output->addInstruction(instr);
                break;
            } else if (type == qMetaTypeId<QList<int> >()) {
                Instruction::StoreIntegerQList instr;
                instr.propertyIndex = prop->index;
                instr.value = int(v->value.asNumber());
                output->addInstruction(instr);
                break;
            } else if (type == qMetaTypeId<QList<bool> >()) {
                Instruction::StoreBoolQList instr;
                bool b = v->value.asBoolean();
                instr.propertyIndex = prop->index;
                instr.value = b;
                output->addInstruction(instr);
                break;
            } else if (type == qMetaTypeId<QList<QUrl> >()) {
                Instruction::StoreUrlQList instr;
                QString string = v->value.asString();
                QUrl u = string.isEmpty() ? QUrl() : output->url.resolved(QUrl(string));
                instr.propertyIndex = prop->index;
                instr.value = output->indexForUrl(u);
                output->addInstruction(instr);
                break;
            } else if (type == qMetaTypeId<QList<QString> >()) {
                Instruction::StoreStringQList instr;
                instr.propertyIndex = prop->index;
                instr.value = output->indexForString(v->value.asString());
                output->addInstruction(instr);
                break;
            }

            // otherwise, generate custom type literal assignment
            Instruction::AssignCustomType instr;
            instr.propertyIndex = prop->index;
            instr.primitive = output->indexForString(v->value.asString());
            instr.type = type;
            output->addInstruction(instr);
            }
            break;
    }
}

/*!
    Resets data by clearing the lists that the QQmlCompiler modifies.
*/
void QQmlCompiler::reset(QQmlCompiledData *data)
{
    data->types.clear();
    data->primitives.clear();
    data->datas.clear();
    data->bytecode.resize(0);
}

/*!
    Compile \a unit, and store the output in \a out.  \a engine is the QQmlEngine
    with which the QQmlCompiledData will be associated.

    Returns true on success, false on failure.  On failure, the compile errors
    are available from errors().

    If the environment variant QML_COMPILER_DUMP is set
    (eg. QML_COMPILER_DUMP=1) the compiled instructions will be dumped to stderr
    on a successful compiler.
*/
bool QQmlCompiler::compile(QQmlEngine *engine,
                                   QQmlTypeData *unit,
                                   QQmlCompiledData *out)
{
    exceptions.clear();

    Q_ASSERT(out);
    reset(out);

    QQmlScript::Object *root = unit->parser().tree();
    Q_ASSERT(root);

    this->engine = engine;
    this->enginePrivate = QQmlEnginePrivate::get(engine);
    this->unit = unit;
    this->unitRoot = root;
    this->output = out;

    // Compile types
    const QList<QQmlTypeData::TypeReference>  &resolvedTypes = unit->resolvedTypes();
    QList<QQmlScript::TypeReference *> referencedTypes = unit->parser().referencedTypes();

    for (int ii = 0; ii < resolvedTypes.count(); ++ii) {
        QQmlCompiledData::TypeReference ref;

        const QQmlTypeData::TypeReference &tref = resolvedTypes.at(ii);
        QQmlScript::TypeReference *parserRef = referencedTypes.at(ii);

        if (tref.type) {
            ref.type = tref.type;
            if (!ref.type->isCreatable()) {
                QString err = ref.type->noCreationReason();
                if (err.isEmpty())
                    err = tr( "Element is not creatable.");
                COMPILE_EXCEPTION(parserRef->firstUse, err);
            }
            
            if (ref.type->containsRevisionedAttributes()) {
                QQmlError cacheError;
                ref.typePropertyCache = enginePrivate->cache(ref.type,
                                                             resolvedTypes.at(ii).minorVersion,
                                                             cacheError);
                if (!ref.typePropertyCache) 
                    COMPILE_EXCEPTION(parserRef->firstUse, cacheError.description());
                ref.typePropertyCache->addref();
            }

        } else if (tref.typeData) {
            ref.component = tref.typeData->compiledData();
            ref.component->addref();
        }
        out->types << ref;
    }

    compileTree(root);

    if (!isError()) {
        if (compilerDump())
            out->dumpInstructions();
        if (componentStats)
            dumpStats();
        Q_ASSERT(out->rootPropertyCache);
    } else {
        reset(out);
    }

    compileState = 0;
    output = 0;
    this->engine = 0;
    this->enginePrivate = 0;
    this->unit = 0;
    this->cachedComponentTypeRef = -1;
    this->cachedTranslationContextIndex = -1;
    this->unitRoot = 0;

    return !isError();
}

void QQmlCompiler::compileTree(QQmlScript::Object *tree)
{
    compileState = pool->New<ComponentCompileState>();

    compileState->root = tree;
    if (componentStats)
        componentStats->componentStat.lineNumber = tree->location.start.line;

    // We generate the importCache before we build the tree so that
    // it can be used in the binding compiler.  Given we "expect" the
    // QML compilation to succeed, this isn't a waste.
    output->importCache = new QQmlTypeNameCache();
    foreach (const QString &ns, unit->namespaces()) {
        output->importCache->add(ns);
    }

    int scriptIndex = 0;
    foreach (const QQmlTypeData::ScriptReference &script, unit->resolvedScripts()) {
        QString qualifier = script.qualifier;
        QString enclosingNamespace;

        const int lastDotIndex = qualifier.lastIndexOf(QLatin1Char('.'));
        if (lastDotIndex != -1) {
            enclosingNamespace = qualifier.left(lastDotIndex);
            qualifier = qualifier.mid(lastDotIndex+1);
        }

        output->importCache->add(qualifier, scriptIndex++, enclosingNamespace);
    }

    unit->imports().populateCache(output->importCache, engine);

    if (!buildObject(tree, BindingContext()) || !completeComponentBuild())
        return;

    Instruction::Init init;
    init.bindingsSize = compileState->totalBindingsCount;
    init.parserStatusSize = compileState->parserStatusCount;
    init.contextCache = genContextCache();
    init.objectStackSize = compileState->objectDepth.maxDepth();
    init.listStackSize = compileState->listDepth.maxDepth();
    if (compileState->compiledBindingData.isEmpty())
        init.compiledBinding = -1;
    else
        init.compiledBinding = output->indexForByteArray(compileState->compiledBindingData);
    output->addInstruction(init);

    foreach (const QQmlTypeData::ScriptReference &script, unit->resolvedScripts()) {
        Instruction::StoreImportedScript import;
        import.value = output->scripts.count();

        QQmlScriptData *scriptData = script.script->scriptData();
        scriptData->addref();
        output->scripts << scriptData;
        output->addInstruction(import);
    }

    if (!compileState->v8BindingProgram.isEmpty()) {
        Instruction::InitV8Bindings bindings;
        int index = output->programs.count();

        typedef QQmlCompiledData::V8Program V8Program;
        output->programs.append(V8Program(compileState->v8BindingProgram, output));

        bindings.programIndex = index;
        bindings.line = compileState->v8BindingProgramLine;
        output->addInstruction(bindings);
    }

    genObject(tree);

    Instruction::SetDefault def;
    output->addInstruction(def);

    Instruction::Done done;
    output->addInstruction(done);

    Q_ASSERT(tree->metatype);

    if (tree->metadata.isEmpty()) {
        output->root = tree->metatype;
    } else {
        static_cast<QMetaObject &>(output->rootData) = *tree->metaObject();
        output->root = &output->rootData;
    }
    if (!tree->metadata.isEmpty()) 
        enginePrivate->registerCompositeType(output->root);
}

static bool QStringList_contains(const QStringList &list, const QHashedStringRef &string)
{
    for (int ii = 0; ii < list.count(); ++ii)
        if (string == list.at(ii))
            return true;

    return false;
}

bool QQmlCompiler::buildObject(QQmlScript::Object *obj, const BindingContext &ctxt)
{
    if (componentStats)
        componentStats->componentStat.objects++;

    Q_ASSERT (obj->type != -1);
    const QQmlCompiledData::TypeReference &tr = output->types.at(obj->type);
    obj->metatype = tr.metaObject();

    // This object is a "Component" element
    if (tr.type && obj->metatype == &QQmlComponent::staticMetaObject) {
        COMPILE_CHECK(buildComponent(obj, ctxt));
        return true;
    } 

    if (tr.component) {
        typedef QQmlInstruction I; 
        const I *init = ((const I *)tr.component->bytecode.constData());
        Q_ASSERT(init && tr.component->instructionType(init) == QQmlInstruction::Init);
 
        // Adjust stack depths to include nested components
        compileState->objectDepth.pushPop(init->init.objectStackSize);
        compileState->listDepth.pushPop(init->init.listStackSize);
        compileState->parserStatusCount += init->init.parserStatusSize;
        compileState->totalBindingsCount += init->init.bindingsSize;
    }

    compileState->objectDepth.push();

    // Object instantiations reset the binding context
    BindingContext objCtxt(obj);

    // Create the synthesized meta object, ignoring aliases
    COMPILE_CHECK(checkDynamicMeta(obj)); 
    COMPILE_CHECK(mergeDynamicMetaProperties(obj));
    COMPILE_CHECK(buildDynamicMeta(obj, IgnoreAliases));

    // Find the native type and check for the QQmlParserStatus interface
    QQmlType *type = toQmlType(obj);
    Q_ASSERT(type);
    obj->parserStatusCast = type->parserStatusCast();
    if (obj->parserStatusCast != -1)
        compileState->parserStatusCount++;

    // Check if this is a custom parser type.  Custom parser types allow
    // assignments to non-existent properties.  These assignments are then
    // compiled by the type.
    bool isCustomParser = output->types.at(obj->type).type &&
                          output->types.at(obj->type).type->customParser() != 0;
    QList<QQmlCustomParserProperty> customProps;

    // Fetch the list of deferred properties
    QStringList deferredList = deferredProperties(obj);

    // Must do id property first.  This is to ensure that the id given to any
    // id reference created matches the order in which the objects are
    // instantiated
    for (Property *prop = obj->properties.first(); prop; prop = obj->properties.next(prop)) {
        if (prop->name() == id_string) {
            COMPILE_CHECK(buildProperty(prop, obj, objCtxt));
            break;
        }
    }

    // Merge 
    Property *defaultProperty = 0;
    Property *skipProperty = 0;
    if (obj->defaultProperty) {
        defaultProperty = obj->defaultProperty;

        Property *explicitProperty = 0;

        const QMetaObject *mo = obj->metatype;
        int idx = mo->indexOfClassInfo("DefaultProperty"); 
        if (idx != -1) {
            QMetaClassInfo info = mo->classInfo(idx);
            const char *p = info.value();
            if (p) {
                int plen = 0;
                char ord = 0;
                while (char c = p[plen++]) { ord |= c; };
                --plen;

                if (ord & 0x80) {
                    // Utf8 - unoptimal, but seldom hit
                    QString *s = pool->NewString(QString::fromUtf8(p, plen));
                    QHashedStringRef r(*s);

                    if (obj->propertiesHashField.test(r.hash())) {
                        for (Property *ep = obj->properties.first(); ep; ep = obj->properties.next(ep)) {
                            if (ep->name() == r) {
                                explicitProperty = ep;
                                break;
                            }
                        }
                    }

                    if (!explicitProperty)
                        defaultProperty->setName(r);

                } else {
                    QHashedCStringRef r(p, plen); 

                    if (obj->propertiesHashField.test(r.hash())) {
                        for (Property *ep = obj->properties.first(); ep; ep = obj->properties.next(ep)) {
                            if (ep->name() == r) {
                                explicitProperty = ep;
                                break;
                            }
                        }
                    }

                    if (!explicitProperty) {
                        // Set the default property name
                        QChar *buffer = pool->NewRawArray<QChar>(r.length());
                        r.writeUtf16(buffer);
                        defaultProperty->setName(QHashedStringRef(buffer, r.length(), r.hash()));
                    }
                }
            }
        }

        if (explicitProperty && !explicitProperty->value && !explicitProperty->values.isEmpty()) {

            skipProperty = explicitProperty; // We merge the values into defaultProperty

            // Find the correct insertion point
            Value *insertPos = 0;

            for (Value *v = defaultProperty->values.first(); v; v = Property::ValueList::next(v)) {
                if (!(v->location.start < explicitProperty->values.first()->location.start))
                    break;
                insertPos = v;
            }

            defaultProperty->values.insertAfter(insertPos, explicitProperty->values);
        } 
    }

    QQmlCustomParser *cp = 0;
    if (isCustomParser)
        cp = output->types.at(obj->type).type->customParser();

    // Build all explicit properties specified
    for (Property *prop = obj->properties.first(); prop; prop = obj->properties.next(prop)) {

        if (prop == skipProperty)
            continue;
        if (prop->name() == id_string)
            continue;

        bool canDefer = false;
        if (isCustomParser) {
            if (doesPropertyExist(prop, obj) &&
                (!(cp->flags() & QQmlCustomParser::AcceptsAttachedProperties) ||
                 !isAttachedPropertyName(prop->name()))) {
                int ids = compileState->ids.count();
                COMPILE_CHECK(buildProperty(prop, obj, objCtxt));
                canDefer = ids == compileState->ids.count();
            } else if (isSignalPropertyName(prop->name()) &&
                    (cp->flags() & QQmlCustomParser::AcceptsSignalHandlers)) {
                COMPILE_CHECK(buildSignal(prop,obj,objCtxt));
            } else {
                customProps << QQmlCustomParserNodePrivate::fromProperty(prop);
            }
        } else {
            if (isSignalPropertyName(prop->name())) {
                COMPILE_CHECK(buildSignal(prop,obj,objCtxt));
            } else {
                int ids = compileState->ids.count();
                COMPILE_CHECK(buildProperty(prop, obj, objCtxt));
                canDefer = ids == compileState->ids.count();
            }
        }

        if (canDefer && !deferredList.isEmpty() && QStringList_contains(deferredList, prop->name()))
            prop->isDeferred = true;

    }

    // Build the default property
    if (defaultProperty)  {
        Property *prop = defaultProperty;

        bool canDefer = false;
        if (isCustomParser) {
            if (doesPropertyExist(prop, obj)) {
                int ids = compileState->ids.count();
                COMPILE_CHECK(buildProperty(prop, obj, objCtxt));
                canDefer = ids == compileState->ids.count();
            } else {
                customProps << QQmlCustomParserNodePrivate::fromProperty(prop);
            }
        } else {
            int ids = compileState->ids.count();
            COMPILE_CHECK(buildProperty(prop, obj, objCtxt));
            canDefer = ids == compileState->ids.count();
        }

        if (canDefer && !deferredList.isEmpty() && QStringList_contains(deferredList, prop->name()))
            prop->isDeferred = true;
    }

    // Compile custom parser parts
    if (isCustomParser && !customProps.isEmpty()) {
        cp->clearErrors();
        cp->compiler = this;
        cp->object = obj;
        obj->custom = cp->compile(customProps);
        cp->compiler = 0;
        cp->object = 0;
        foreach (QQmlError err, cp->errors()) {
            err.setUrl(output->url);
            exceptions << err;
        }
    }

    compileState->objectDepth.pop();

    return true;
}

void QQmlCompiler::genObject(QQmlScript::Object *obj)
{
    QQmlCompiledData::TypeReference &tr = output->types[obj->type];
    if (tr.type && obj->metatype == &QQmlComponent::staticMetaObject) {
        genComponent(obj);
        return;
    }

    // Create the object
    if (obj->custom.isEmpty() && output->types.at(obj->type).type &&
        !output->types.at(obj->type).type->isExtendedType() && obj != compileState->root) {

        Instruction::CreateSimpleObject create;
        create.create = output->types.at(obj->type).type->createFunction();
        create.typeSize = output->types.at(obj->type).type->createSize();
        create.type = obj->type;
        create.line = obj->location.start.line;
        create.column = obj->location.start.column;
        output->addInstruction(create);

    } else {

        if (output->types.at(obj->type).type) {
            Instruction::CreateCppObject create;
            create.line = obj->location.start.line;
            create.column = obj->location.start.column;
            create.data = -1;
            if (!obj->custom.isEmpty())
                create.data = output->indexForByteArray(obj->custom);
            create.type = obj->type;
            create.isRoot = (compileState->root == obj);
            output->addInstruction(create);
        } else {
            Instruction::CreateQMLObject create;
            create.type = obj->type;
            create.isRoot = (compileState->root == obj);

            if (!obj->bindingBitmask.isEmpty()) {
                Q_ASSERT(obj->bindingBitmask.size() % 4 == 0);
                create.bindingBits = output->indexForByteArray(obj->bindingBitmask);
            } else {
                create.bindingBits = -1;
            }
            output->addInstruction(create);

            Instruction::CompleteQMLObject complete;
            complete.line = obj->location.start.line;
            complete.column = obj->location.start.column;
            complete.isRoot = (compileState->root == obj);
            output->addInstruction(complete);
        }
    }

    // Setup the synthesized meta object if necessary
    if (!obj->metadata.isEmpty()) {
        Instruction::StoreMetaObject meta;
        meta.data = output->indexForByteArray(obj->metadata);
        meta.aliasData = output->indexForByteArray(obj->synthdata);
        meta.propertyCache = output->propertyCaches.count();

        QQmlPropertyCache *propertyCache = obj->synthCache;
        Q_ASSERT(propertyCache);
        propertyCache->addref();

        // Add flag for alias properties
        if (!obj->synthdata.isEmpty()) {
            const QQmlVMEMetaData *vmeMetaData = 
                reinterpret_cast<const QQmlVMEMetaData *>(obj->synthdata.constData());
            for (int ii = 0; ii < vmeMetaData->aliasCount; ++ii) {
                int index = obj->metaObject()->propertyOffset() + vmeMetaData->propertyCount + ii;
                QQmlPropertyData *data = propertyCache->property(index);
                data->setFlags(data->getFlags() | QQmlPropertyData::IsAlias);
            }
        }

        if (obj == unitRoot) {
            propertyCache->addref();
            output->rootPropertyCache = propertyCache;
        }

        output->propertyCaches << propertyCache;
        output->addInstruction(meta);
    } else if (obj == unitRoot) {
        output->rootPropertyCache = tr.createPropertyCache(engine);
        output->rootPropertyCache->addref();
    }

    // Set the object id
    if (!obj->id.isEmpty()) {
        Instruction::SetId id;
        id.value = output->indexForString(obj->id);
        id.index = obj->idIndex;
        output->addInstruction(id);
    }

    // Begin the class
    if (tr.type && obj->parserStatusCast != -1) {
        Instruction::BeginObject begin;
        begin.castValue = obj->parserStatusCast;
        output->addInstruction(begin);
    }

    genObjectBody(obj);
}

void QQmlCompiler::genObjectBody(QQmlScript::Object *obj)
{
    for (Property *prop = obj->scriptStringProperties.first(); prop; prop = Object::PropertyList::next(prop)) {
        Q_ASSERT(prop->scriptStringScope != -1);
        const QString &script = prop->values.first()->value.asScript();
        Instruction::StoreScriptString ss;
        ss.propertyIndex = prop->index;
        ss.value = output->indexForString(script);
        ss.scope = prop->scriptStringScope;
//        ss.bindingId = rewriteBinding(script, prop->name());
        ss.bindingId = rewriteBinding(prop->values.first()->value, QString()); // XXX
        ss.line = prop->location.start.line;
        ss.column = prop->location.start.column;
        output->addInstruction(ss);
    }

    bool seenDefer = false;
    for (Property *prop = obj->valueProperties.first(); prop; prop = Object::PropertyList::next(prop)) {
        if (prop->isDeferred) {
            seenDefer = true;
            continue;
        }
        if (!prop->isAlias)
            genValueProperty(prop, obj);
    }
    if (seenDefer) {
        Instruction::Defer defer;
        defer.deferCount = 0;
        int deferIdx = output->addInstruction(defer);
        int nextInstructionIndex = output->nextInstructionIndex();

        Instruction::DeferInit dinit;
        // XXX - these are now massive over allocations
        dinit.bindingsSize = compileState->totalBindingsCount;
        dinit.parserStatusSize = compileState->parserStatusCount; 
        dinit.objectStackSize = compileState->objectDepth.maxDepth(); 
        dinit.listStackSize = compileState->listDepth.maxDepth(); 
        output->addInstruction(dinit);

        for (Property *prop = obj->valueProperties.first(); prop; prop = Object::PropertyList::next(prop)) {
            if (!prop->isDeferred)
                continue;
            genValueProperty(prop, obj);
        }

        Instruction::Done done;
        output->addInstruction(done);

        output->instruction(deferIdx)->defer.deferCount = output->nextInstructionIndex() - nextInstructionIndex;
    }

    for (Property *prop = obj->signalProperties.first(); prop; prop = Object::PropertyList::next(prop)) {

        QQmlScript::Value *v = prop->values.first();

        if (v->type == Value::SignalObject) {

            genObject(v->object);

            Instruction::AssignSignalObject assign;
            assign.line = v->location.start.line;
            assign.signal = output->indexForString(prop->name().toString());
            output->addInstruction(assign);

        } else if (v->type == Value::SignalExpression) {

            Instruction::StoreSignal store;
            store.signalIndex = prop->index;
            const QString &rewrite = rewriteSignalHandler(v->value, prop->name().toString());
            store.value = output->indexForByteArray(rewrite.toUtf8());
            store.context = v->signalExpressionContextStack;
            store.line = v->location.start.line;
            store.column = v->location.start.column;
            output->addInstruction(store);

        }

    }

    for (Property *prop = obj->attachedProperties.first(); prop; prop = Object::PropertyList::next(prop)) {
        Instruction::FetchAttached fetch;
        fetch.id = prop->index;
        fetch.line = prop->location.start.line;
        output->addInstruction(fetch);

        genObjectBody(prop->value);

        Instruction::PopFetchedObject pop;
        output->addInstruction(pop);
    }

    for (Property *prop = obj->groupedProperties.first(); prop; prop = Object::PropertyList::next(prop)) {
        Instruction::FetchObject fetch;
        fetch.property = prop->index;
        fetch.line = prop->location.start.line;
        output->addInstruction(fetch);

        if (!prop->value->metadata.isEmpty()) {
            Instruction::StoreMetaObject meta;
            meta.data = output->indexForByteArray(prop->value->metadata);
            meta.aliasData = output->indexForByteArray(prop->value->synthdata);
            meta.propertyCache = -1;
            output->addInstruction(meta);
        }

        genObjectBody(prop->value);

        Instruction::PopFetchedObject pop;
        output->addInstruction(pop);
    }

    for (Property *prop = obj->valueTypeProperties.first(); prop; prop = Object::PropertyList::next(prop)) {
        if (!prop->isAlias)
            genValueTypeProperty(obj, prop);
    }

    for (Property *prop = obj->valueProperties.first(); prop; prop = Object::PropertyList::next(prop)) {
        if (prop->isDeferred) 
            continue;
        if (prop->isAlias)
            genValueProperty(prop, obj);
    }

    for (Property *prop = obj->valueTypeProperties.first(); prop; prop = Object::PropertyList::next(prop)) {
        if (prop->isAlias)
            genValueTypeProperty(obj, prop);
    }
}

void QQmlCompiler::genValueTypeProperty(QQmlScript::Object *obj,QQmlScript::Property *prop)
{
    Instruction::FetchValueType fetch;
    fetch.property = prop->index;
    fetch.type = prop->type;
    fetch.bindingSkipList = 0;

    if (obj->type == -1 || output->types.at(obj->type).component) {
        // We only have to do this if this is a composite type.  If it is a builtin
        // type it can't possibly already have bindings that need to be cleared.
        for (Property *vprop = prop->value->valueProperties.first(); vprop; vprop = Object::PropertyList::next(vprop)) {
            if (!vprop->values.isEmpty()) {
                Q_ASSERT(vprop->index >= 0 && vprop->index < 32);
                fetch.bindingSkipList |= (1 << vprop->index);
            }
        }
    }

    output->addInstruction(fetch);

    for (Property *vprop = prop->value->valueProperties.first(); vprop; vprop = Object::PropertyList::next(vprop)) {
        genPropertyAssignment(vprop, prop->value, prop);
    }

    Instruction::PopValueType pop;
    pop.property = prop->index;
    pop.type = prop->type;
    pop.bindingSkipList = 0;
    output->addInstruction(pop);
}

void QQmlCompiler::genComponent(QQmlScript::Object *obj)
{
    QQmlScript::Object *root = obj->defaultProperty->values.first()->object;
    Q_ASSERT(root);

    Instruction::CreateComponent create;
    create.line = root->location.start.line;
    create.column = root->location.start.column;
    create.endLine = root->location.end.line;
    create.isRoot = (compileState->root == obj);
    int createInstruction = output->addInstruction(create);
    int nextInstructionIndex = output->nextInstructionIndex();

    ComponentCompileState *oldCompileState = compileState;
    compileState = componentState(root);

    Instruction::Init init;
    init.bindingsSize = compileState->totalBindingsCount;
    init.parserStatusSize = compileState->parserStatusCount;
    init.contextCache = genContextCache();
    init.objectStackSize = compileState->objectDepth.maxDepth();
    init.listStackSize = compileState->listDepth.maxDepth();
    if (compileState->compiledBindingData.isEmpty())
        init.compiledBinding = -1;
    else
        init.compiledBinding = output->indexForByteArray(compileState->compiledBindingData);
    output->addInstruction(init);

    if (!compileState->v8BindingProgram.isEmpty()) {
        Instruction::InitV8Bindings bindings;
        int index = output->programs.count();

        typedef QQmlCompiledData::V8Program V8Program;
        output->programs.append(V8Program(compileState->v8BindingProgram, output));

        bindings.programIndex = index;
        bindings.line = compileState->v8BindingProgramLine;
        output->addInstruction(bindings);
    }

    genObject(root);

    Instruction::SetDefault def;
    output->addInstruction(def);

    Instruction::Done done;
    output->addInstruction(done);

    output->instruction(createInstruction)->createComponent.count = 
        output->nextInstructionIndex() - nextInstructionIndex;

    compileState = oldCompileState;

    if (!obj->id.isEmpty()) {
        Instruction::SetId id;
        id.value = output->indexForString(obj->id);
        id.index = obj->idIndex;
        output->addInstruction(id);
    }

    if (obj == unitRoot) {
        output->rootPropertyCache = output->types[obj->type].createPropertyCache(engine);
        output->rootPropertyCache->addref();
    }
}

bool QQmlCompiler::buildComponent(QQmlScript::Object *obj,
                                 const BindingContext &ctxt)
{
    // The special "Component" element can only have the id property and a
    // default property, that actually defines the component's tree

    compileState->objectDepth.push();

    // Find, check and set the "id" property (if any)
    Property *idProp = 0;
    if (obj->properties.isMany() ||
       (obj->properties.isOne() && obj->properties.first()->name() != id_string))
        COMPILE_EXCEPTION(obj->properties.first(), tr("Component elements may not contain properties other than id"));
       
    if (!obj->properties.isEmpty())
        idProp = obj->properties.first();

    if (idProp) {
       if (idProp->value || idProp->values.isMany() || idProp->values.first()->object) 
           COMPILE_EXCEPTION(idProp, tr("Invalid component id specification"));
       COMPILE_CHECK(checkValidId(idProp->values.first(), idProp->values.first()->primitive()))

        QString idVal = idProp->values.first()->primitive();

        if (compileState->ids.value(idVal))
            COMPILE_EXCEPTION(idProp, tr("id is not unique"));

        obj->id = idVal;
        addId(idVal, obj);
    }

    // Check the Component tree is well formed
    if (obj->defaultProperty &&
       (obj->defaultProperty->value || obj->defaultProperty->values.isMany() ||
        (obj->defaultProperty->values.isOne() && !obj->defaultProperty->values.first()->object)))
        COMPILE_EXCEPTION(obj, tr("Invalid component body specification"));

    if (!obj->dynamicProperties.isEmpty())
        COMPILE_EXCEPTION(obj, tr("Component objects cannot declare new properties."));
    if (!obj->dynamicSignals.isEmpty())
        COMPILE_EXCEPTION(obj, tr("Component objects cannot declare new signals."));
    if (!obj->dynamicSlots.isEmpty())
        COMPILE_EXCEPTION(obj, tr("Component objects cannot declare new functions."));

    QQmlScript::Object *root = 0;
    if (obj->defaultProperty && !obj->defaultProperty->values.isEmpty())
        root = obj->defaultProperty->values.first()->object;

    if (!root)
        COMPILE_EXCEPTION(obj, tr("Cannot create empty component specification"));

    // Build the component tree
    COMPILE_CHECK(buildComponentFromRoot(root, ctxt));

    compileState->objectDepth.pop();

    return true;
}

bool QQmlCompiler::buildComponentFromRoot(QQmlScript::Object *obj,
                                         const BindingContext &ctxt)
{
    ComponentCompileState *oldComponentCompileState = compileState;
    compileState = pool->New<ComponentCompileState>();
    compileState->root = obj;
    compileState->nested = true;

    if (componentStats) {
        ComponentStat oldComponentStat = componentStats->componentStat;

        componentStats->componentStat = ComponentStat();
        componentStats->componentStat.lineNumber = obj->location.start.line;

        if (obj)
            COMPILE_CHECK(buildObject(obj, ctxt));

        COMPILE_CHECK(completeComponentBuild());

        componentStats->componentStat = oldComponentStat;
    } else {
        if (obj)
            COMPILE_CHECK(buildObject(obj, ctxt));

        COMPILE_CHECK(completeComponentBuild());
    }

    compileState = oldComponentCompileState;

    return true;
}


// Build a sub-object.  A sub-object is one that was not created directly by
// QML - such as a grouped property object, or an attached object.  Sub-object's
// can't have an id, involve a custom parser, have attached properties etc.
bool QQmlCompiler::buildSubObject(QQmlScript::Object *obj, const BindingContext &ctxt)
{
    Q_ASSERT(obj->metatype);
    Q_ASSERT(!obj->defaultProperty);
    Q_ASSERT(ctxt.isSubContext()); // sub-objects must always be in a binding
                                   // sub-context

    for (Property *prop = obj->properties.first(); prop; prop = obj->properties.next(prop)) {
        if (isSignalPropertyName(prop->name())) {
            COMPILE_CHECK(buildSignal(prop, obj, ctxt));
        } else {
            COMPILE_CHECK(buildProperty(prop, obj, ctxt));
        }
    }

    return true;
}

int QQmlCompiler::componentTypeRef()
{
    if (cachedComponentTypeRef == -1) {
        QQmlType *t = QQmlMetaType::qmlType(Component_import_string,1,0);
        for (int ii = output->types.count() - 1; ii >= 0; --ii) {
            if (output->types.at(ii).type == t) {
                cachedComponentTypeRef = ii;
                return ii;
            }
        }
        QQmlCompiledData::TypeReference ref;
        ref.type = t;
        output->types << ref;
        cachedComponentTypeRef = output->types.count() - 1;
    }
    return cachedComponentTypeRef;
}

int QQmlCompiler::translationContextIndex()
{
    if (cachedTranslationContextIndex == -1) {
        // This code must match that in the qsTr() implementation
        const QString &path = output->name;
        int lastSlash = path.lastIndexOf(QLatin1Char('/'));
        QString context = (lastSlash > -1) ? path.mid(lastSlash + 1, path.length()-lastSlash-5) :
                                             QString();
        QByteArray contextUtf8 = context.toUtf8();
        cachedTranslationContextIndex = output->indexForByteArray(contextUtf8);
    }
    return cachedTranslationContextIndex;
}

bool QQmlCompiler::buildSignal(QQmlScript::Property *prop, QQmlScript::Object *obj,
                                       const BindingContext &ctxt)
{
    Q_ASSERT(obj->metaObject());

    const QHashedStringRef &propName = prop->name();

    Q_ASSERT(propName.startsWith(on_string));
    QString name = propName.mid(2, -1).toString();

    // Note that the property name could start with any alpha or '_' or '$' character,
    // so we need to do the lower-casing of the first alpha character.
    for (int firstAlphaIndex = 0; firstAlphaIndex < name.size(); ++firstAlphaIndex) {
        if (name.at(firstAlphaIndex).isUpper()) {
            name[firstAlphaIndex] = name.at(firstAlphaIndex).toLower();
            break;
        }
    }

    bool notInRevision = false;

    QQmlPropertyData *sig = signal(obj, QStringRef(&name), &notInRevision);

    if (sig == 0) {

        if (notInRevision && 0 == property(obj, propName, 0)) {
            Q_ASSERT(obj->type != -1);
            const QList<QQmlTypeData::TypeReference>  &resolvedTypes = unit->resolvedTypes();
            const QQmlTypeData::TypeReference &type = resolvedTypes.at(obj->type);
            if (type.type) {
                COMPILE_EXCEPTION(prop, tr("\"%1.%2\" is not available in %3 %4.%5.").arg(elementName(obj)).arg(prop->name().toString()).arg(type.type->module()).arg(type.majorVersion).arg(type.minorVersion));
            } else {
                COMPILE_EXCEPTION(prop, tr("\"%1.%2\" is not available due to component versioning.").arg(elementName(obj)).arg(prop->name().toString()));
            }
        }

        // If the "on<Signal>" name doesn't resolve into a signal, try it as a
        // property.
        COMPILE_CHECK(buildProperty(prop, obj, ctxt));

    }  else {

        if (prop->value || !prop->values.isOne())
            COMPILE_EXCEPTION(prop, tr("Incorrectly specified signal assignment"));

        prop->index = sig->coreIndex;
        prop->core = *sig;

        obj->addSignalProperty(prop);

        if (prop->values.first()->object) {
            COMPILE_CHECK(buildObject(prop->values.first()->object, ctxt));
            prop->values.first()->type = Value::SignalObject;
        } else {
            prop->values.first()->type = Value::SignalExpression;

            if (!prop->values.first()->value.isScript())
                COMPILE_EXCEPTION(prop, tr("Cannot assign a value to a signal (expecting a script to be run)"));

            QString script = prop->values.first()->value.asScript().trimmed();
            if (script.isEmpty())
                COMPILE_EXCEPTION(prop, tr("Empty signal assignment"));

            prop->values.first()->signalExpressionContextStack = ctxt.stack;
        }
    }

    return true;
}


/*!
    Returns true if (value) property \a prop exists on obj, false otherwise.
*/
bool QQmlCompiler::doesPropertyExist(QQmlScript::Property *prop,
                                             QQmlScript::Object *obj)
{
    if (prop->name().isEmpty())
        return false;
    if(isAttachedPropertyName(prop->name()) || prop->name() == id_string)
        return true;

    return property(obj, prop->name()) != 0;
}

bool QQmlCompiler::buildProperty(QQmlScript::Property *prop,
                                         QQmlScript::Object *obj,
                                         const BindingContext &ctxt)
{
    if (prop->isEmpty()) 
        COMPILE_EXCEPTION(prop, tr("Empty property assignment"));

    const QMetaObject *metaObject = obj->metaObject();
    Q_ASSERT(metaObject);

    if (isAttachedPropertyName(prop->name())) {
        // Setup attached property data

        if (ctxt.isSubContext()) {
            // Attached properties cannot be used on sub-objects.  Sub-objects
            // always exist in a binding sub-context, which is what we test
            // for here.
            COMPILE_EXCEPTION(prop, tr("Attached properties cannot be used here"));
        }

        QQmlType *type = 0;
        QQmlImportNamespace *typeNamespace = 0;
        unit->imports().resolveType(prop->name().toString(), &type, 0, 0, 0, &typeNamespace);

        if (typeNamespace) {
            COMPILE_CHECK(buildPropertyInNamespace(typeNamespace, prop, obj, 
                                                   ctxt));
            return true;
        } else if (!type || !type->attachedPropertiesType())  {
            COMPILE_EXCEPTION(prop, tr("Non-existent attached object"));
        }

        if (!prop->value)
            COMPILE_EXCEPTION(prop, tr("Invalid attached object assignment"));

        Q_ASSERT(type->attachedPropertiesFunction());
        prop->index = type->attachedPropertiesId();
        prop->value->metatype = type->attachedPropertiesType();
    } else {
        // Setup regular property data
        bool notInRevision = false;
        QQmlPropertyData *d =
            prop->name().isEmpty()?0:property(obj, prop->name(), &notInRevision);

        if (d == 0 && notInRevision) {
            const QList<QQmlTypeData::TypeReference>  &resolvedTypes = unit->resolvedTypes();
            const QQmlTypeData::TypeReference &type = resolvedTypes.at(obj->type);
            if (type.type) {
                COMPILE_EXCEPTION(prop, tr("\"%1.%2\" is not available in %3 %4.%5.").arg(elementName(obj)).arg(prop->name().toString()).arg(type.type->module()).arg(type.majorVersion).arg(type.minorVersion));
            } else {
                COMPILE_EXCEPTION(prop, tr("\"%1.%2\" is not available due to component versioning.").arg(elementName(obj)).arg(prop->name().toString()));
            }
        } else if (d) {
            prop->index = d->coreIndex;
            prop->core = *d;
        } else if (prop->isDefault) {
            QMetaProperty p = QQmlMetaType::defaultProperty(metaObject);
            QQmlPropertyData defaultPropertyData;
            defaultPropertyData.load(p, engine);
            if (p.name())
                prop->setName(QLatin1String(p.name()));
            prop->core = defaultPropertyData;
            prop->index = prop->core.coreIndex;
        }

        // We can't error here as the "id" property does not require a
        // successful index resolution
        if (prop->index != -1) 
            prop->type = prop->core.propType;

        // Check if this is an alias
        if (prop->index != -1 && 
            prop->parent && 
            prop->parent->type != -1 && 
            output->types.at(prop->parent->type).component) {

            QQmlPropertyCache *cache = output->types.at(prop->parent->type).component->rootPropertyCache;
            if (cache && cache->property(prop->index) && cache->property(prop->index)->isAlias())
                prop->isAlias = true;
        }

        if (prop->index != -1 && !prop->values.isEmpty()) 
            prop->parent->setBindingBit(prop->index);
    }

    if (!prop->isDefault && prop->name() == id_string && !ctxt.isSubContext()) {

        // The magic "id" behavior doesn't apply when "id" is resolved as a
        // default property or to sub-objects (which are always in binding
        // sub-contexts)
        COMPILE_CHECK(buildIdProperty(prop, obj));
        if (prop->type == QVariant::String &&
            prop->values.first()->value.isString())
            COMPILE_CHECK(buildPropertyAssignment(prop, obj, ctxt));

    } else if (isAttachedPropertyName(prop->name())) {

        COMPILE_CHECK(buildAttachedProperty(prop, obj, ctxt));

    } else if (prop->index == -1) {

        if (prop->isDefault) {
            COMPILE_EXCEPTION(prop->values.first(), tr("Cannot assign to non-existent default property"));
        } else {
            COMPILE_EXCEPTION(prop, tr("Cannot assign to non-existent property \"%1\"").arg(prop->name().toString()));
        }

    } else if (prop->value) {

        COMPILE_CHECK(buildGroupedProperty(prop, obj, ctxt));

    } else if (prop->core.isQList()) {

        COMPILE_CHECK(buildListProperty(prop, obj, ctxt));

    } else if (prop->type == qMetaTypeId<QQmlScriptString>()) {

        COMPILE_CHECK(buildScriptStringProperty(prop, obj, ctxt));

    } else {

        COMPILE_CHECK(buildPropertyAssignment(prop, obj, ctxt));

    }

    return true;
}

bool QQmlCompiler::buildPropertyInNamespace(QQmlImportNamespace *ns,
                                            QQmlScript::Property *nsProp,
                                            QQmlScript::Object *obj,
                                            const BindingContext &ctxt)
{
    if (!nsProp->value)
        COMPILE_EXCEPTION(nsProp, tr("Invalid use of namespace"));

    for (Property *prop = nsProp->value->properties.first(); prop; prop = nsProp->value->properties.next(prop)) {

        if (!isAttachedPropertyName(prop->name()))
            COMPILE_EXCEPTION(prop, tr("Not an attached property name"));

        // Setup attached property data

        QQmlType *type = 0;
        unit->imports().resolveType(ns, prop->name().toString(), &type, 0, 0, 0);

        if (!type || !type->attachedPropertiesType()) 
            COMPILE_EXCEPTION(prop, tr("Non-existent attached object"));

        if (!prop->value)
            COMPILE_EXCEPTION(prop, tr("Invalid attached object assignment"));

        Q_ASSERT(type->attachedPropertiesFunction());
        prop->index = type->index();
        prop->value->metatype = type->attachedPropertiesType();

        COMPILE_CHECK(buildAttachedProperty(prop, obj, ctxt));
    }

    return true;
}

void QQmlCompiler::genValueProperty(QQmlScript::Property *prop,
                                   QQmlScript::Object *obj)
{
    if (prop->core.isQList()) {
        genListProperty(prop, obj);
    } else {
        genPropertyAssignment(prop, obj);
    }
}

void QQmlCompiler::genListProperty(QQmlScript::Property *prop,
                                  QQmlScript::Object *obj)
{
    int listType = enginePrivate->listType(prop->type);

    Instruction::FetchQList fetch;
    fetch.property = prop->index;
    bool listTypeIsInterface = QQmlMetaType::isInterface(listType);
    fetch.type = listType;
    output->addInstruction(fetch);

    for (Value *v = prop->values.first(); v; v = Property::ValueList::next(v)) {

        if (v->type == Value::CreatedObject) {

            genObject(v->object);
            if (listTypeIsInterface) {
                Instruction::AssignObjectList assign;
                assign.line = prop->location.start.line;
                output->addInstruction(assign);
            } else {
                Instruction::StoreObjectQList store;
                output->addInstruction(store);
            }

        } else if (v->type == Value::PropertyBinding) {

            genBindingAssignment(v, prop, obj);

        }

    }

    Instruction::PopQList pop;
    output->addInstruction(pop);
}

void QQmlCompiler::genPropertyAssignment(QQmlScript::Property *prop,
                                        QQmlScript::Object *obj,
                                        QQmlScript::Property *valueTypeProperty)
{
    for (Value *v = prop->values.first(); v; v = Property::ValueList::next(v)) {

        Q_ASSERT(v->type == Value::CreatedObject ||
                 v->type == Value::PropertyBinding ||
                 v->type == Value::Literal);

        if (v->type == Value::CreatedObject) {

            genObject(v->object);

            if (QQmlMetaType::isInterface(prop->type)) {

                Instruction::StoreInterface store;
                store.line = v->object->location.start.line;
                store.propertyIndex = prop->index;
                output->addInstruction(store);

            } else if (prop->type == QMetaType::QVariant) {

                if (prop->core.isVMEProperty()) {
                    Instruction::StoreVarObject store;
                    store.line = v->object->location.start.line;
                    store.propertyIndex = prop->index;
                    output->addInstruction(store);
                } else {
                    Instruction::StoreVariantObject store;
                    store.line = v->object->location.start.line;
                    store.propertyIndex = prop->index;
                    output->addInstruction(store);
                }


            } else {

                Instruction::StoreObject store;
                store.line = v->object->location.start.line;
                store.propertyIndex = prop->index;
                output->addInstruction(store);

            }
        } else if (v->type == Value::PropertyBinding) {

            genBindingAssignment(v, prop, obj, valueTypeProperty);

        } else if (v->type == Value::Literal) {

            genLiteralAssignment(prop, v);

        }

    }

    for (Value *v = prop->onValues.first(); v; v = Property::ValueList::next(v)) {

        Q_ASSERT(v->type == Value::ValueSource ||
                 v->type == Value::ValueInterceptor);

        if (v->type == Value::ValueSource) {
            genObject(v->object);

            Instruction::StoreValueSource store;
            if (valueTypeProperty) {
                store.property = genValueTypeData(prop, valueTypeProperty);
                store.owner = 1;
            } else {
                store.property = prop->core;
                store.owner = 0;
            }
            QQmlType *valueType = toQmlType(v->object);
            store.castValue = valueType->propertyValueSourceCast();
            output->addInstruction(store);

        } else if (v->type == Value::ValueInterceptor) {
            genObject(v->object);

            Instruction::StoreValueInterceptor store;
            if (valueTypeProperty) {
                store.property = genValueTypeData(prop, valueTypeProperty);
                store.owner = 1;
            } else {
                store.property = prop->core;
                store.owner = 0;
            }
            QQmlType *valueType = toQmlType(v->object);
            store.castValue = valueType->propertyValueInterceptorCast();
            output->addInstruction(store);
        }

    }
}

bool QQmlCompiler::buildIdProperty(QQmlScript::Property *prop,
                                  QQmlScript::Object *obj)
{
    if (prop->value ||
        prop->values.isMany() ||
        prop->values.first()->object)
        COMPILE_EXCEPTION(prop, tr("Invalid use of id property"));

    QQmlScript::Value *idValue = prop->values.first();
    QString val = idValue->primitive();

    COMPILE_CHECK(checkValidId(idValue, val));

    if (compileState->ids.value(val))
        COMPILE_EXCEPTION(prop, tr("id is not unique"));

    prop->values.first()->type = Value::Id;

    obj->id = val;
    addId(val, obj);

    return true;
}

void QQmlCompiler::addId(const QString &id, QQmlScript::Object *obj)
{
    Q_UNUSED(id);
    Q_ASSERT(!compileState->ids.value(id));
    Q_ASSERT(obj->id == id);
    obj->idIndex = compileState->ids.count();
    compileState->ids.append(obj);
}

void QQmlCompiler::addBindingReference(JSBindingReference *ref)
{
    Q_ASSERT(ref->value && !ref->value->bindingReference);
    ref->value->bindingReference = ref;
    compileState->totalBindingsCount++;
    compileState->bindings.prepend(ref);
}

void QQmlCompiler::saveComponentState()
{
    Q_ASSERT(compileState->root);
    Q_ASSERT(compileState->root->componentCompileState == 0);

    compileState->root->componentCompileState = compileState;

    if (componentStats) 
        componentStats->savedComponentStats.append(componentStats->componentStat);
}

QQmlCompilerTypes::ComponentCompileState *
QQmlCompiler::componentState(QQmlScript::Object *obj)
{
    Q_ASSERT(obj->componentCompileState);
    return obj->componentCompileState;
}

// Build attached property object.  In this example,
// Text {
//    GridView.row: 10
// }
// GridView is an attached property object.
bool QQmlCompiler::buildAttachedProperty(QQmlScript::Property *prop,
                                        QQmlScript::Object *obj,
                                        const BindingContext &ctxt)
{
    Q_ASSERT(prop->value);
    Q_ASSERT(prop->index != -1); // This is set in buildProperty()

    compileState->objectDepth.push();

    obj->addAttachedProperty(prop);

    COMPILE_CHECK(buildSubObject(prop->value, ctxt.incr()));

    compileState->objectDepth.pop();

    return true;
}


// Build "grouped" properties. In this example:
// Text {
//     font.pointSize: 12
//     font.family: "Helvetica"
// }
// font is a nested property.  pointSize and family are not.
bool QQmlCompiler::buildGroupedProperty(QQmlScript::Property *prop,
                                                QQmlScript::Object *obj,
                                                const BindingContext &ctxt)
{
    Q_ASSERT(prop->type != 0);
    Q_ASSERT(prop->index != -1);

    if (QQmlValueTypeFactory::isValueType(prop->type)) {
        if (prop->type >= 0 && enginePrivate->valueTypes[prop->type]) {

            if (!prop->values.isEmpty()) {
                if (prop->values.first()->location < prop->value->location) {
                    COMPILE_EXCEPTION(prop->value, tr( "Property has already been assigned a value"));
                } else {
                    COMPILE_EXCEPTION(prop->values.first(), tr( "Property has already been assigned a value"));
                }
            }

            if (!prop->core.isWritable() && !prop->isReadOnlyDeclaration) {
                COMPILE_EXCEPTION(prop, tr( "Invalid property assignment: \"%1\" is a read-only property").arg(prop->name().toString()));
            }


            if (prop->isAlias) {
                for (Property *vtProp = prop->value->properties.first(); vtProp; vtProp = prop->value->properties.next(vtProp)) {
                    vtProp->isAlias = true;
                }
            }

            COMPILE_CHECK(buildValueTypeProperty(enginePrivate->valueTypes[prop->type],
                                                 prop->value, obj, ctxt.incr()));
            obj->addValueTypeProperty(prop);
        } else {
            COMPILE_EXCEPTION(prop, tr("Invalid grouped property access"));
        }

    } else {
        // Load the nested property's meta type
        prop->value->metatype = enginePrivate->metaObjectForType(prop->type);
        if (!prop->value->metatype)
            COMPILE_EXCEPTION(prop, tr("Invalid grouped property access"));

        if (!prop->values.isEmpty()) 
            COMPILE_EXCEPTION(prop->values.first(), tr( "Cannot assign a value directly to a grouped property"));

        obj->addGroupedProperty(prop);

        compileState->objectDepth.push();

        COMPILE_CHECK(buildSubObject(prop->value, ctxt.incr()));

        compileState->objectDepth.pop();
    }

    return true;
}

bool QQmlCompiler::buildValueTypeProperty(QObject *type,
                                                  QQmlScript::Object *obj,
                                                  QQmlScript::Object *baseObj,
                                                  const BindingContext &ctxt)
{
    compileState->objectDepth.push();

    if (obj->defaultProperty)
        COMPILE_EXCEPTION(obj, tr("Invalid property use"));
    obj->metatype = type->metaObject();

    for (Property *prop = obj->properties.first(); prop; prop = obj->properties.next(prop)) {

        QQmlPropertyData *d = property(obj, prop->name());
        if (d == 0) 
            COMPILE_EXCEPTION(prop, tr("Cannot assign to non-existent property \"%1\"").arg(prop->name().toString()));

        prop->index = d->coreIndex;
        prop->type = d->propType;
        prop->core = *d;
        prop->isValueTypeSubProperty = true;

        if (prop->value)
            COMPILE_EXCEPTION(prop, tr("Property assignment expected"));

        if (prop->values.isMany()) {
            COMPILE_EXCEPTION(prop, tr("Single property assignment expected"));
        } else if (!prop->values.isEmpty()) {
            QQmlScript::Value *value = prop->values.first();

            if (value->object) {
                COMPILE_EXCEPTION(prop, tr("Unexpected object assignment"));
            } else if (value->value.isScript()) {
                // ### Check for writability

                //optimization for <Type>.<EnumValue> enum assignments
                bool isEnumAssignment = false;

                if (prop->core.isEnum() || prop->core.propType == QMetaType::Int)
                    COMPILE_CHECK(testQualifiedEnumAssignment(prop, obj, value, &isEnumAssignment));

                if (isEnumAssignment) {
                    value->type = Value::Literal;
                } else {
                    JSBindingReference *reference = pool->New<JSBindingReference>();
                    reference->expression = value->value;
                    reference->property = prop;
                    reference->value = value;
                    reference->bindingContext = ctxt;
                    reference->bindingContext.owner++;
                    addBindingReference(reference);
                    value->type = Value::PropertyBinding;
                }
            } else  {
                COMPILE_CHECK(testLiteralAssignment(prop, value));
                value->type = Value::Literal;
            }
        }

        for (Value *v = prop->onValues.first(); v; v = Property::ValueList::next(v)) {
            Q_ASSERT(v->object);

            COMPILE_CHECK(buildPropertyOnAssignment(prop, obj, baseObj, v, ctxt)); 
        }

        obj->addValueProperty(prop);
    }

    compileState->objectDepth.pop();

    return true;
}

// Build assignments to QML lists.  QML lists are properties of type
// QQmlListProperty<T>.  List properties can accept a list of 
// objects, or a single binding.
bool QQmlCompiler::buildListProperty(QQmlScript::Property *prop,
                                             QQmlScript::Object *obj,
                                             const BindingContext &ctxt)
{
    Q_ASSERT(prop->core.isQList());

    compileState->listDepth.push();

    int t = prop->type;

    obj->addValueProperty(prop);

    int listType = enginePrivate->listType(t);
    bool listTypeIsInterface = QQmlMetaType::isInterface(listType);

    bool assignedBinding = false;
    for (Value *v = prop->values.first(); v; v = Property::ValueList::next(v)) {
        if (v->object) {
            v->type = Value::CreatedObject;
            COMPILE_CHECK(buildObject(v->object, ctxt));

            // We check object coercian here.  We check interface assignment
            // at runtime.
            if (!listTypeIsInterface) {
                if (!canCoerce(listType, v->object)) {
                    COMPILE_EXCEPTION(v, tr("Cannot assign object to list"));
                }
            }

        } else if (v->value.isScript()) {
            if (assignedBinding)
                COMPILE_EXCEPTION(v, tr("Can only assign one binding to lists"));

            assignedBinding = true;
            COMPILE_CHECK(buildBinding(v, prop, ctxt));
            v->type = Value::PropertyBinding;
        } else {
            COMPILE_EXCEPTION(v, tr("Cannot assign primitives to lists"));
        }
    }

    compileState->listDepth.pop();

    return true;
}

// Compiles an assignment to a QQmlScriptString property
bool QQmlCompiler::buildScriptStringProperty(QQmlScript::Property *prop,
                                            QQmlScript::Object *obj,
                                            const BindingContext &ctxt)
{
    if (prop->values.isMany())
        COMPILE_EXCEPTION(prop->values.first()->nextValue, tr( "Cannot assign multiple values to a script property"));

    if (prop->values.first()->object)
        COMPILE_EXCEPTION(prop->values.first(), tr( "Invalid property assignment: script expected"));

    prop->scriptStringScope = ctxt.stack;
    obj->addScriptStringProperty(prop);

    return true;
}

// Compile regular property assignments of the form "property: <value>"
bool QQmlCompiler::buildPropertyAssignment(QQmlScript::Property *prop,
                                          QQmlScript::Object *obj,
                                          const BindingContext &ctxt)
{
    obj->addValueProperty(prop);

    if (prop->values.isMany())
        COMPILE_EXCEPTION(prop->values.first(), tr( "Cannot assign multiple values to a singular property") );

    for (Value *v = prop->values.first(); v; v = Property::ValueList::next(v)) {
        if (v->object) {

            COMPILE_CHECK(buildPropertyObjectAssignment(prop, obj, v, ctxt));

        } else {

            COMPILE_CHECK(buildPropertyLiteralAssignment(prop, obj, v, ctxt));

        }
    }

    for (Value *v = prop->onValues.first(); v; v = Property::ValueList::next(v)) {
        Q_ASSERT(v->object);
        COMPILE_CHECK(buildPropertyOnAssignment(prop, obj, obj, v, ctxt));
    }

    return true;
}

// Compile assigning a single object instance to a regular property
bool QQmlCompiler::buildPropertyObjectAssignment(QQmlScript::Property *prop,
                                                         QQmlScript::Object *obj,
                                                         QQmlScript::Value *v,
                                                         const BindingContext &ctxt)
{
    Q_ASSERT(prop->index != -1);
    Q_ASSERT(v->object->type != -1);

    if (!prop->core.isWritable() && !prop->isReadOnlyDeclaration)
        COMPILE_EXCEPTION(v, tr("Invalid property assignment: \"%1\" is a read-only property").arg(prop->name().toString()));

    if (QQmlMetaType::isInterface(prop->type)) {

        // Assigning an object to an interface ptr property
        COMPILE_CHECK(buildObject(v->object, ctxt));

        v->type = Value::CreatedObject;

    } else if (prop->type == QMetaType::QVariant) {

        // Assigning an object to a QVariant
        COMPILE_CHECK(buildObject(v->object, ctxt));

        v->type = Value::CreatedObject;
    } else {
        // Normally buildObject() will set this up, but we need the static
        // meta object earlier to test for assignability.  It doesn't matter
        // that there may still be outstanding synthesized meta object changes
        // on this type, as they are not relevant for assignability testing
        v->object->metatype = output->types.at(v->object->type).metaObject();
        Q_ASSERT(v->object->metaObject());

        // We want to raw metaObject here as the raw metaobject is the
        // actual property type before we applied any extensions that might
        // effect the properties on the type, but don't effect assignability
        const QMetaObject *propertyMetaObject = enginePrivate->rawMetaObjectForType(prop->type);

        // Will be true if the assgned type inherits propertyMetaObject
        bool isAssignable = false;
        // Determine isAssignable value
        if (propertyMetaObject) {
            const QMetaObject *c = v->object->metatype;
            while(c) {
                isAssignable |= (QQmlPropertyPrivate::equal(c, propertyMetaObject));
                c = c->superClass();
            }
        }

        if (isAssignable) {
            // Simple assignment
            COMPILE_CHECK(buildObject(v->object, ctxt));

            v->type = Value::CreatedObject;
        } else if (propertyMetaObject == &QQmlComponent::staticMetaObject) {
            // Automatic "Component" insertion
            QQmlScript::Object *root = v->object;
            QQmlScript::Object *component = pool->New<Object>();
            component->type = componentTypeRef();
            component->metatype = &QQmlComponent::staticMetaObject;
            component->location = root->location;
            QQmlScript::Value *componentValue = pool->New<Value>();
            componentValue->object = root;
            component->getDefaultProperty()->addValue(componentValue);
            v->object = component;
            COMPILE_CHECK(buildPropertyObjectAssignment(prop, obj, v, ctxt));
        } else {
            COMPILE_EXCEPTION(v->object, tr("Cannot assign object to property"));
        }
    }

    return true;
}

// Compile assigning a single object instance to a regular property using the "on" syntax.
//
// For example:
//     Item {
//         NumberAnimation on x { }
//     }
bool QQmlCompiler::buildPropertyOnAssignment(QQmlScript::Property *prop,
                                                     QQmlScript::Object *obj,
                                                     QQmlScript::Object *baseObj,
                                                     QQmlScript::Value *v,
                                                     const BindingContext &ctxt)
{
    Q_ASSERT(prop->index != -1);
    Q_ASSERT(v->object->type != -1);

    Q_UNUSED(obj);

    if (!prop->core.isWritable())
        COMPILE_EXCEPTION(v, tr("Invalid property assignment: \"%1\" is a read-only property").arg(prop->name().toString()));


    // Normally buildObject() will set this up, but we need the static
    // meta object earlier to test for assignability.  It doesn't matter
    // that there may still be outstanding synthesized meta object changes
    // on this type, as they are not relevant for assignability testing
    v->object->metatype = output->types.at(v->object->type).metaObject();
    Q_ASSERT(v->object->metaObject());

    // Will be true if the assigned type inherits QQmlPropertyValueSource
    bool isPropertyValue = false;
    // Will be true if the assigned type inherits QQmlPropertyValueInterceptor
    bool isPropertyInterceptor = false;
    if (QQmlType *valueType = toQmlType(v->object)) {
        isPropertyValue = valueType->propertyValueSourceCast() != -1;
        isPropertyInterceptor = valueType->propertyValueInterceptorCast() != -1;
    }

    if (isPropertyValue || isPropertyInterceptor) {
        // Assign as a property value source
        COMPILE_CHECK(buildObject(v->object, ctxt));

        if (isPropertyInterceptor && prop->parent->synthdata.isEmpty())
            buildDynamicMeta(baseObj, ForceCreation);
        v->type = isPropertyValue ? Value::ValueSource : Value::ValueInterceptor;
    } else {
        COMPILE_EXCEPTION(v, tr("\"%1\" cannot operate on \"%2\"").arg(elementName(v->object)).arg(prop->name().toString()));
    }

    return true;
}

// Compile assigning a literal or binding to a regular property
bool QQmlCompiler::buildPropertyLiteralAssignment(QQmlScript::Property *prop,
                                                          QQmlScript::Object *obj,
                                                          QQmlScript::Value *v,
                                                          const BindingContext &ctxt)
{
    Q_ASSERT(prop->index != -1);

    if (v->value.isScript()) {

        //optimization for <Type>.<EnumValue> enum assignments
        if (prop->core.isEnum() || prop->core.propType == QMetaType::Int) {
            bool isEnumAssignment = false;
            COMPILE_CHECK(testQualifiedEnumAssignment(prop, obj, v, &isEnumAssignment));
            if (isEnumAssignment) {
                v->type = Value::Literal;
                return true;
            }
        }

        // Test for other binding optimizations
        if (!buildLiteralBinding(v, prop, ctxt))
            COMPILE_CHECK(buildBinding(v, prop, ctxt));

        v->type = Value::PropertyBinding;

    } else {

        COMPILE_CHECK(testLiteralAssignment(prop, v));

        v->type = Value::Literal;
    }

    return true;
}

bool QQmlCompiler::testQualifiedEnumAssignment(QQmlScript::Property *prop,
                                                       QQmlScript::Object *obj,
                                                       QQmlScript::Value *v,
                                                       bool *isAssignment)
{
    bool isIntProp = (prop->core.propType == QMetaType::Int) && !prop->core.isEnum();
    *isAssignment = false;
    if (!prop->core.isEnum() && !isIntProp)
        return true;

    QMetaProperty mprop = obj->metaObject()->property(prop->index);

    if (!prop->core.isWritable() && !prop->isReadOnlyDeclaration)
        COMPILE_EXCEPTION(v, tr("Invalid property assignment: \"%1\" is a read-only property").arg(prop->name().toString()));

    QString string = v->value.asString();
    if (!string.at(0).isUpper())
        return true;

    if (isIntProp) {
        // Allow enum assignment to ints.
        int enumval = evaluateEnum(string.toUtf8());
        if (enumval != -1) {
            v->type = Value::Literal;
            v->value = QQmlScript::Variant((double)enumval);
            *isAssignment = true;
        }
        return true;
    }

    QStringList parts = string.split(QLatin1Char('.'));
    if (parts.count() != 2)
        return true;

    QString typeName = parts.at(0);
    QQmlType *type = 0;
    unit->imports().resolveType(typeName, &type, 0, 0, 0, 0);

    if (!type)
        return true;

    QString enumValue = parts.at(1);
    int value;
    bool ok;

    if (toQmlType(obj) == type) {
        // When these two match, we can short cut the search
        if (mprop.isFlagType()) {
            value = mprop.enumerator().keysToValue(enumValue.toUtf8().constData(), &ok);
        } else {
            value = mprop.enumerator().keyToValue(enumValue.toUtf8().constData(), &ok);
        }
    } else {
        // Otherwise we have to search the whole type
        // This matches the logic in QV8TypeWrapper
        QByteArray enumName = enumValue.toUtf8();
        const QMetaObject *metaObject = type->baseMetaObject();
        ok = false;
        for (int ii = metaObject->enumeratorCount() - 1; !ok && ii >= 0; --ii) {
            QMetaEnum e = metaObject->enumerator(ii);
            value = e.keyToValue(enumName.constData(), &ok);
        }
    }

    if (!ok)
        return true;

    v->type = Value::Literal;
    v->value = QQmlScript::Variant((double)value);
    *isAssignment = true;

    return true;
}

struct StaticQtMetaObject : public QObject
{
    static const QMetaObject *get()
        { return &static_cast<StaticQtMetaObject*> (0)->staticQtMetaObject; }
};

// Similar logic to above, but not knowing target property.
int QQmlCompiler::evaluateEnum(const QByteArray& script) const
{
    int dot = script.indexOf('.');
    if (dot > 0) {
        const QByteArray &scope = script.left(dot);
        QQmlType *type = 0;
        unit->imports().resolveType(QString::fromUtf8(script.left(dot)), &type, 0, 0, 0, 0);
        if (!type && scope != "Qt")
            return -1;
        const QMetaObject *mo = type ? type->metaObject() : StaticQtMetaObject::get();
        const char *key = script.constData() + dot+1;
        int i = mo->enumeratorCount();
        while (i--) {
            bool ok;
            int v = mo->enumerator(i).keyToValue(key, &ok);
            if (ok)
                return v;
        }
    }
    return -1;
}

const QMetaObject *QQmlCompiler::resolveType(const QString& name) const
{
    QQmlType *qmltype = 0;
    if (!unit->imports().resolveType(name, &qmltype, 0, 0, 0, 0))
        return 0;
    if (!qmltype)
        return 0;
    return qmltype->metaObject();
}

// similar to logic of completeComponentBuild, but also sticks data
// into primitives at the end
int QQmlCompiler::rewriteBinding(const QQmlScript::Variant& value, const QString& name)
{
    QQmlRewrite::RewriteBinding rewriteBinding;
    rewriteBinding.setName(QLatin1Char('$') + name.mid(name.lastIndexOf(QLatin1Char('.')) + 1));

    QString rewrite = rewriteBinding(value.asAST(), value.asScript(), 0);

    return output->indexForString(rewrite);
}

QString QQmlCompiler::rewriteSignalHandler(const QQmlScript::Variant& value, const QString &name)
{
    QQmlRewrite::RewriteSignalHandler rewriteSignalHandler;
    return rewriteSignalHandler(value.asAST(), value.asScript(), name);
}

// Ensures that the dynamic meta specification on obj is valid
bool QQmlCompiler::checkDynamicMeta(QQmlScript::Object *obj)
{
    bool seenDefaultProperty = false;

    // We use a coarse grain, 31 bit hash to check if there are duplicates.
    // Calculating the hash for the names is not a waste as we have to test
    // them against the illegalNames set anyway.
    QHashField propNames;
    QHashField methodNames;

    // Check properties
    for (Object::DynamicProperty *p = obj->dynamicProperties.first(); p; p = obj->dynamicProperties.next(p)) {
        const QQmlScript::Object::DynamicProperty &prop = *p;

        if (prop.isDefaultProperty) {
            if (seenDefaultProperty)
                COMPILE_EXCEPTION(&prop, tr("Duplicate default property"));
            seenDefaultProperty = true;
        }

        if (propNames.testAndSet(prop.name.hash())) {
            for (Object::DynamicProperty *p2 = obj->dynamicProperties.first(); p2 != p; 
                 p2 = obj->dynamicProperties.next(p2)) {
                if (p2->name == prop.name) {
                    COMPILE_EXCEPTION_LOCATION(prop.nameLocation.line,
                                               prop.nameLocation.column,
                                               tr("Duplicate property name"));
                }
            }
        }

        if (prop.name.at(0).isUpper()) {
            COMPILE_EXCEPTION_LOCATION(prop.nameLocation.line,
                                       prop.nameLocation.column,
                                       tr("Property names cannot begin with an upper case letter"));
        }

        if (enginePrivate->v8engine()->illegalNames().contains(prop.name)) {
            COMPILE_EXCEPTION_LOCATION(prop.nameLocation.line,
                                       prop.nameLocation.column,
                                       tr("Illegal property name"));
        }
    }

    for (Object::DynamicSignal *s = obj->dynamicSignals.first(); s; s = obj->dynamicSignals.next(s)) {
        const QQmlScript::Object::DynamicSignal &currSig = *s;

        if (methodNames.testAndSet(currSig.name.hash())) {
            for (Object::DynamicSignal *s2 = obj->dynamicSignals.first(); s2 != s;
                 s2 = obj->dynamicSignals.next(s2)) {
                if (s2->name == currSig.name)
                    COMPILE_EXCEPTION(&currSig, tr("Duplicate signal name"));
            }
        }

        if (currSig.name.at(0).isUpper())
            COMPILE_EXCEPTION(&currSig, tr("Signal names cannot begin with an upper case letter"));
        if (enginePrivate->v8engine()->illegalNames().contains(currSig.name))
            COMPILE_EXCEPTION(&currSig, tr("Illegal signal name"));
    }

    for (Object::DynamicSlot *s = obj->dynamicSlots.first(); s; s = obj->dynamicSlots.next(s)) {
        const QQmlScript::Object::DynamicSlot &currSlot = *s;

        if (methodNames.testAndSet(currSlot.name.hash())) {
            for (Object::DynamicSignal *s2 = obj->dynamicSignals.first(); s2;
                 s2 = obj->dynamicSignals.next(s2)) {
                if (s2->name == currSlot.name)
                    COMPILE_EXCEPTION(&currSlot, tr("Duplicate method name"));
            }
            for (Object::DynamicSlot *s2 = obj->dynamicSlots.first(); s2 != s;
                 s2 = obj->dynamicSlots.next(s2)) {
                if (s2->name == currSlot.name)
                    COMPILE_EXCEPTION(&currSlot, tr("Duplicate method name"));
            }
        }

        if (currSlot.name.at(0).isUpper())
            COMPILE_EXCEPTION(&currSlot, tr("Method names cannot begin with an upper case letter"));
        if (enginePrivate->v8engine()->illegalNames().contains(currSlot.name))
            COMPILE_EXCEPTION(&currSlot, tr("Illegal method name"));
    }

    return true;
}

bool QQmlCompiler::mergeDynamicMetaProperties(QQmlScript::Object *obj)
{
    for (Object::DynamicProperty *p = obj->dynamicProperties.first(); p;
         p = obj->dynamicProperties.next(p)) {

        if (!p->defaultValue || p->type == Object::DynamicProperty::Alias)
            continue;

        Property *property = 0;
        if (p->isDefaultProperty) {
            property = obj->getDefaultProperty();
        } else {
            property = obj->getProperty(p->name);
            if (!property->values.isEmpty()) 
                COMPILE_EXCEPTION(property, tr("Property value set multiple times"));
        }

        if (p->isReadOnly)
            property->isReadOnlyDeclaration = true;

        if (property->value)
            COMPILE_EXCEPTION(property, tr("Invalid property nesting"));

        property->values.append(p->defaultValue->values);
    }
    return true;
}

Q_GLOBAL_STATIC(QAtomicInt, classIndexCounter)

bool QQmlCompiler::buildDynamicMeta(QQmlScript::Object *obj, DynamicMetaMode mode)
{
    Q_ASSERT(obj);
    Q_ASSERT(obj->metatype);

    if (mode != ForceCreation &&
        obj->dynamicProperties.isEmpty() &&
        obj->dynamicSignals.isEmpty() &&
        obj->dynamicSlots.isEmpty())
        return true;

    bool resolveAlias = (mode == ResolveAliases);

    const Object::DynamicProperty *defaultProperty = 0;
    int aliasCount = 0;
    int varPropCount = 0;
    int totalPropCount = 0;
    int firstPropertyVarIndex = 0;

    for (Object::DynamicProperty *p = obj->dynamicProperties.first(); p; p = obj->dynamicProperties.next(p)) {

        if (p->type == Object::DynamicProperty::Alias)
            aliasCount++;
        if (p->type == Object::DynamicProperty::Var)
            varPropCount++;

        if (p->isDefaultProperty && 
            (resolveAlias || p->type != Object::DynamicProperty::Alias))
            defaultProperty = p;

        if (!resolveAlias) {
            // No point doing this for both the alias and non alias cases
            QQmlPropertyData *d = property(obj, p->name);
            if (d && d->isFinal())
                COMPILE_EXCEPTION(p, tr("Cannot override FINAL property"));
        }
    }

    bool buildData = resolveAlias || aliasCount == 0;

    QByteArray dynamicData;
    if (buildData) {
        typedef QQmlVMEMetaData VMD;

        dynamicData = QByteArray(sizeof(QQmlVMEMetaData) +
                                 (obj->dynamicProperties.count() - aliasCount) * sizeof(VMD::PropertyData) +
                                 obj->dynamicSlots.count() * sizeof(VMD::MethodData) +
                                 aliasCount * sizeof(VMD::AliasData), 0);
    }

    int uniqueClassId = classIndexCounter()->fetchAndAddRelaxed(1);

    QByteArray newClassName = obj->metatype->className();
    newClassName.append("_QML_");
    newClassName.append(QByteArray::number(uniqueClassId));

    if (compileState->root == obj && !compileState->nested) {
        QString path = output->url.path();
        int lastSlash = path.lastIndexOf(QLatin1Char('/'));
        if (lastSlash > -1) {
            QString nameBase = path.mid(lastSlash + 1, path.length()-lastSlash-5);
            if (!nameBase.isEmpty() && nameBase.at(0).isUpper())
                newClassName = nameBase.toUtf8() + "_QMLTYPE_" + QByteArray::number(uniqueClassId);
        }
    }

    // Size of the array that describes parameter types & names
    int paramDataSize = (obj->aggregateDynamicSignalParameterCount() + obj->aggregateDynamicSlotParameterCount()) * 2
            + obj->dynamicProperties.count() // for Changed() signals return types
            // Return "parameters" don't have names
            - (obj->dynamicSignals.count() + obj->dynamicSlots.count());

    QFastMetaBuilder builder;
    int paramIndex;
    QFastMetaBuilder::StringRef classNameRef = builder.init(newClassName.length(), 
            obj->dynamicProperties.count() - (resolveAlias?0:aliasCount),
            obj->dynamicSlots.count(),
            obj->dynamicSignals.count() + obj->dynamicProperties.count(),
            defaultProperty?1:0, paramDataSize, &paramIndex);

    struct TypeData {
        Object::DynamicProperty::Type dtype;
        int metaType;
    } builtinTypes[] = {
        { Object::DynamicProperty::Var, QMetaType::QVariant },
        { Object::DynamicProperty::Variant, QMetaType::QVariant },
        { Object::DynamicProperty::Int, QMetaType::Int },
        { Object::DynamicProperty::Bool, QMetaType::Bool },
        { Object::DynamicProperty::Real, QMetaType::Double },
        { Object::DynamicProperty::String, QMetaType::QString },
        { Object::DynamicProperty::Url, QMetaType::QUrl },
        { Object::DynamicProperty::Color, QMetaType::QColor },
        { Object::DynamicProperty::Time, QMetaType::QTime },
        { Object::DynamicProperty::Date, QMetaType::QDate },
        { Object::DynamicProperty::DateTime, QMetaType::QDateTime },
        { Object::DynamicProperty::Rect, QMetaType::QRectF },
    };
    static const int builtinTypeCount = sizeof(builtinTypes) / sizeof(TypeData);

    // Reserve dynamic properties
    if (obj->dynamicProperties.count()) {
        typedef QQmlVMEMetaData VMD;

        int effectivePropertyIndex = 0;
        for (Object::DynamicProperty *p = obj->dynamicProperties.first(); p; p = obj->dynamicProperties.next(p)) {

            // Reserve space for name
            if (p->type != Object::DynamicProperty::Alias || resolveAlias)
                p->nameRef = builder.newString(p->name.utf8length());

            int metaType = 0;
            int propertyType = 0; // for VMD
            bool readonly = false;

            if (p->type == Object::DynamicProperty::Alias) {
                continue;
            } else if (p->type < builtinTypeCount) {
                Q_ASSERT(builtinTypes[p->type].dtype == p->type);
                metaType = builtinTypes[p->type].metaType;
                propertyType = metaType;

            } else {
                Q_ASSERT(p->type == Object::DynamicProperty::CustomList ||
                         p->type == Object::DynamicProperty::Custom);

                // XXX don't double resolve this in the case of an alias run

                QByteArray customTypeName;
                QQmlType *qmltype = 0;
                QString url;
                if (!unit->imports().resolveType(p->customType.toString(), &qmltype, &url, 0, 0, 0))
                    COMPILE_EXCEPTION(p, tr("Invalid property type"));

                if (!qmltype) {
                    QQmlTypeData *tdata = enginePrivate->typeLoader.getType(QUrl(url));
                    Q_ASSERT(tdata);
                    Q_ASSERT(tdata->isComplete());
                    customTypeName = tdata->compiledData()->root->className();
                    tdata->release();
                } else {
                    customTypeName = qmltype->typeName();
                }

                if (p->type == Object::DynamicProperty::Custom) {
                    customTypeName += '*';
                    propertyType = QMetaType::QObjectStar;
                } else {
                    readonly = true;
                    customTypeName = QByteArrayLiteral("QQmlListProperty<") + customTypeName + '>';
                    propertyType = qMetaTypeId<QQmlListProperty<QObject> >();
                }

                metaType = QMetaType::type(customTypeName);
                Q_ASSERT(metaType != QMetaType::UnknownType);
                Q_ASSERT(metaType != QMetaType::Void);
            }

            if (p->type == Object::DynamicProperty::Var)
                continue;

            if (p->isReadOnly)
                readonly = true;

            if (buildData) {
                VMD *vmd = (QQmlVMEMetaData *)dynamicData.data();
                vmd->propertyCount++;
                (vmd->propertyData() + effectivePropertyIndex)->propertyType = propertyType;
            }

            builder.setProperty(effectivePropertyIndex, p->nameRef, metaType,
                                readonly?QFastMetaBuilder::None:QFastMetaBuilder::Writable,
                                effectivePropertyIndex);

            p->changedNameRef = builder.newString(p->name.utf8length() + Changed_string.size());
            builder.setSignal(effectivePropertyIndex, p->changedNameRef, paramIndex);
            paramIndex++;

            effectivePropertyIndex++;
        }

        if (varPropCount) {
            VMD *vmd = (QQmlVMEMetaData *)dynamicData.data();
            if (buildData)
                vmd->varPropertyCount = varPropCount;
            firstPropertyVarIndex = effectivePropertyIndex;
            totalPropCount = varPropCount + effectivePropertyIndex;
            for (Object::DynamicProperty *p = obj->dynamicProperties.first(); p; p = obj->dynamicProperties.next(p)) {
                if (p->type == Object::DynamicProperty::Var) {
                    if (buildData) {
                        vmd->propertyCount++;
                        (vmd->propertyData() + effectivePropertyIndex)->propertyType = QMetaType::QVariant;
                    }

                    builder.setProperty(effectivePropertyIndex, p->nameRef,
                                        QMetaType::QVariant,
                                        p->isReadOnly?QFastMetaBuilder::None:QFastMetaBuilder::Writable,
                                        effectivePropertyIndex);

                    p->changedNameRef = builder.newString(p->name.utf8length() + Changed_string.size());
                    builder.setSignal(effectivePropertyIndex, p->changedNameRef, paramIndex);
                    paramIndex++;

                    effectivePropertyIndex++;
                }
            }
        }
        
        if (aliasCount) {
            int aliasIndex = 0;
            for (Object::DynamicProperty *p = obj->dynamicProperties.first(); p; p = obj->dynamicProperties.next(p)) {
                if (p->type == Object::DynamicProperty::Alias) {
                    if (resolveAlias) {
                        Q_ASSERT(buildData);
                        ((QQmlVMEMetaData *)dynamicData.data())->aliasCount++;
                        COMPILE_CHECK(compileAlias(builder, dynamicData, obj, effectivePropertyIndex, 
                                                   aliasIndex, *p));
                    }
                    // Even if we aren't resolving the alias, we need a fake signal so that the 
                    // metaobject remains consistent across the resolve and non-resolve alias runs
                    p->changedNameRef = builder.newString(p->name.utf8length() + Changed_string.size());
                    builder.setSignal(effectivePropertyIndex, p->changedNameRef, paramIndex);
                    paramIndex++;
                    effectivePropertyIndex++;
                    aliasIndex++;
                }
            }
        }
    }

    // Reserve default property
    QFastMetaBuilder::StringRef defPropRef;
    if (defaultProperty) {
        defPropRef = builder.newString(int(sizeof("DefaultProperty")) - 1);
        builder.setClassInfo(0, defPropRef, defaultProperty->nameRef);
    }

    // Reserve dynamic signals
    int signalIndex = 0;
    for (Object::DynamicSignal *s = obj->dynamicSignals.first(); s; s = obj->dynamicSignals.next(s)) {

        s->nameRef = builder.newString(s->name.utf8length());

        int paramCount = s->parameterNames.count();
        QVarLengthArray<int, 10> paramTypes(paramCount);
        if (paramCount) {
            s->parameterNamesRef = pool->NewRawList<QFastMetaBuilder::StringRef>(paramCount);
            for (int i = 0; i < paramCount; ++i) {
                s->parameterNamesRef[i] = builder.newString(s->parameterNames.at(i).utf8length());
                Q_ASSERT(s->parameterTypes.at(i) < builtinTypeCount);
                paramTypes[i] = builtinTypes[s->parameterTypes.at(i)].metaType;
            }
        }

        if (buildData)
            ((QQmlVMEMetaData *)dynamicData.data())->signalCount++;
        
        builder.setSignal(signalIndex + obj->dynamicProperties.count(), s->nameRef,
                          paramIndex, paramCount, paramTypes.constData(), s->parameterNamesRef.data());
        paramIndex += paramCount*2 + 1;
        ++signalIndex;
    }

    // Reserve dynamic slots
    if (obj->dynamicSlots.count()) {

        typedef QQmlVMEMetaData VMD;

        int methodIndex = 0;
        for (Object::DynamicSlot *s = obj->dynamicSlots.first(); s; s = obj->dynamicSlots.next(s)) {
            s->nameRef = builder.newString(s->name.utf8length());
            int paramCount = s->parameterNames.count();

            QVarLengthArray<int, 10> paramTypes(paramCount);
            if (paramCount) {
                s->parameterNamesRef = pool->NewRawList<QFastMetaBuilder::StringRef>(paramCount);
                for (int i = 0; i < paramCount; ++i) {
                    s->parameterNamesRef[i] = builder.newString(s->parameterNames.at(i).size());
                    paramTypes[i] = QMetaType::QVariant;
                }
            }

            builder.setMethod(methodIndex, s->nameRef, paramIndex, paramCount,
                              paramTypes.constData(), s->parameterNamesRef.data(), QMetaType::QVariant);
            paramIndex += paramCount*2 + 1;

            if (buildData) {
                QString funcScript;
                int namesSize = 0;
                if (paramCount) namesSize += s->parameterNamesLength() + (paramCount - 1 /* commas */);
                funcScript.reserve(int(sizeof("(function ")) - 1  + s->name.length() + 1 /* lparen */ +
                        namesSize + 1 /* rparen */ + s->body.length() + 1 /* rparen */);
                funcScript = QLatin1String("(function ") + s->name.toString() + QLatin1Char('(');
                for (int jj = 0; jj < paramCount; ++jj) {
                    if (jj) funcScript.append(QLatin1Char(','));
                    funcScript.append(QLatin1String(s->parameterNames.at(jj)));
                }
                funcScript += QLatin1Char(')') + s->body + QLatin1Char(')');

                QByteArray utf8 = funcScript.toUtf8();
                VMD::MethodData methodData = { s->parameterNames.count(), 0, 
                                               utf8.length(),
                                               s->location.start.line };

                VMD *vmd = (QQmlVMEMetaData *)dynamicData.data();
                vmd->methodCount++;

                VMD::MethodData &md = *(vmd->methodData() + methodIndex);
                md = methodData;
                md.bodyOffset = dynamicData.size();

                dynamicData.append((const char *)utf8.constData(), utf8.length());
            }


            methodIndex++;
        }
    }

    // Now allocate properties
    for (Object::DynamicProperty *p = obj->dynamicProperties.first(); p; p = obj->dynamicProperties.next(p)) {

        char *d = p->changedNameRef.data();
        p->name.writeUtf8(d);
        strcpy(d + p->name.utf8length(), "Changed");
        p->changedNameRef.loadByteArrayData();

        if (p->type == Object::DynamicProperty::Alias && !resolveAlias)
            continue;

        p->nameRef.load(p->name);
    }

    // Allocate default property if necessary
    if (defaultProperty) 
        defPropRef.load("DefaultProperty");

    // Now allocate signals
    for (Object::DynamicSignal *s = obj->dynamicSignals.first(); s; s = obj->dynamicSignals.next(s)) {

        s->nameRef.load(s->name);

        for (int jj = 0; jj < s->parameterNames.count(); ++jj)
            s->parameterNamesRef[jj].load(s->parameterNames.at(jj));
    }

    // Now allocate methods
    for (Object::DynamicSlot *s = obj->dynamicSlots.first(); s; s = obj->dynamicSlots.next(s)) {
        s->nameRef.load(s->name);

        for (int jj = 0; jj < s->parameterNames.count(); ++jj)
            s->parameterNamesRef[jj].load(s->parameterNames.at(jj).constData());
    }

    // Now allocate class name
    classNameRef.load(newClassName);

    obj->metadata = builder.toData();
    builder.fromData(&obj->extObject, obj->metatype, obj->metadata);

    if (mode == IgnoreAliases && aliasCount) 
        compileState->aliasingObjects.append(obj);

    obj->synthdata = dynamicData;

    if (obj->synthCache) {
        obj->synthCache->release();
        obj->synthCache = 0;
    }

    if (obj->type != -1) {
        QQmlPropertyCache *superCache = output->types[obj->type].createPropertyCache(engine);
        QQmlPropertyCache *cache =
            superCache->copyAndAppend(engine, &obj->extObject,
                                      QQmlPropertyData::NoFlags,
                                      QQmlPropertyData::IsVMEFunction,
                                      QQmlPropertyData::IsVMESignal);

        // now we modify the flags appropriately for var properties.
        int propertyOffset = obj->extObject.propertyOffset();
        QQmlPropertyData *currPropData = 0;
        for (int pvi = firstPropertyVarIndex; pvi < totalPropCount; ++pvi) {
            currPropData = cache->property(pvi + propertyOffset);
            currPropData->setFlags(currPropData->getFlags() | QQmlPropertyData::IsVMEProperty);
        }

        obj->synthCache = cache;
    }

    return true;
}

bool QQmlCompiler::checkValidId(QQmlScript::Value *v, const QString &val)
{
    if (val.isEmpty()) 
        COMPILE_EXCEPTION(v, tr( "Invalid empty ID"));

    QChar ch = val.at(0);
    if (ch.isLetter() && !ch.isLower())
        COMPILE_EXCEPTION(v, tr( "IDs cannot start with an uppercase letter"));

    QChar u(QLatin1Char('_'));
    if (!ch.isLetter() && ch != u)
        COMPILE_EXCEPTION(v, tr( "IDs must start with a letter or underscore"));

    for (int ii = 1; ii < val.count(); ++ii) {
        ch = val.at(ii);
        if (!ch.isLetterOrNumber() && ch != u)
            COMPILE_EXCEPTION(v, tr( "IDs must contain only letters, numbers, and underscores"));
    }

    if (enginePrivate->v8engine()->illegalNames().contains(val))
        COMPILE_EXCEPTION(v, tr( "ID illegally masks global JavaScript property"));

    return true;
}

#include <private/qqmljsparser_p.h>

static QStringList astNodeToStringList(QQmlJS::AST::Node *node)
{
    if (node->kind == QQmlJS::AST::Node::Kind_IdentifierExpression) {
        QString name =
            static_cast<QQmlJS::AST::IdentifierExpression *>(node)->name.toString();
        return QStringList() << name;
    } else if (node->kind == QQmlJS::AST::Node::Kind_FieldMemberExpression) {
        QQmlJS::AST::FieldMemberExpression *expr = static_cast<QQmlJS::AST::FieldMemberExpression *>(node);

        QStringList rv = astNodeToStringList(expr->base);
        if (rv.isEmpty())
            return rv;
        rv.append(expr->name.toString());
        return rv;
    }
    return QStringList();
}

bool QQmlCompiler::compileAlias(QFastMetaBuilder &builder,
                                        QByteArray &data,
                                        QQmlScript::Object *obj,
                                        int propIndex, int aliasIndex,
                                        Object::DynamicProperty &prop)
{
    Q_ASSERT(!prop.nameRef.isEmpty());
    if (!prop.defaultValue)
        COMPILE_EXCEPTION(obj, tr("No property alias location"));

    if (!prop.defaultValue->values.isOne() ||
        prop.defaultValue->values.first()->object ||
        !prop.defaultValue->values.first()->value.isScript())
        COMPILE_EXCEPTION(prop.defaultValue, tr("Invalid alias location"));

    QQmlJS::AST::Node *node = prop.defaultValue->values.first()->value.asAST();
    if (!node)
        COMPILE_EXCEPTION(obj, tr("No property alias location")); // ### Can this happen?

    QStringList alias = astNodeToStringList(node);

    if (alias.count() < 1 || alias.count() > 3)
        COMPILE_EXCEPTION(prop.defaultValue, tr("Invalid alias reference. An alias reference must be specified as <id>, <id>.<property> or <id>.<value property>.<property>"));

    QQmlScript::Object *idObject = compileState->ids.value(alias.at(0));
    if (!idObject)
        COMPILE_EXCEPTION(prop.defaultValue, tr("Invalid alias reference. Unable to find id \"%1\"").arg(alias.at(0)));

    QByteArray typeName;

    int propIdx = -1;
    int flags = 0;
    int type = 0;
    bool writable = false;
    bool resettable = false;
    if (alias.count() == 2 || alias.count() == 3) {
        propIdx = indexOfProperty(idObject, alias.at(1));

        if (-1 == propIdx) {
            COMPILE_EXCEPTION(prop.defaultValue, tr("Invalid alias location"));
        } else if (propIdx > 0xFFFF) {
            COMPILE_EXCEPTION(prop.defaultValue, tr("Alias property exceeds alias bounds"));
        }

        QMetaProperty aliasProperty = idObject->metaObject()->property(propIdx);
        if (!aliasProperty.isScriptable())
            COMPILE_EXCEPTION(prop.defaultValue, tr("Invalid alias location"));

        writable = aliasProperty.isWritable() && !prop.isReadOnly;
        resettable = aliasProperty.isResettable() && !prop.isReadOnly;

        type = aliasProperty.userType();

        if (alias.count() == 3) {
            QQmlValueType *valueType = enginePrivate->valueTypes[aliasProperty.type()];
            if (!valueType)
                COMPILE_EXCEPTION(prop.defaultValue, tr("Invalid alias location"));

            propIdx |= ((unsigned int)aliasProperty.type()) << 24;

            int valueTypeIndex = valueType->metaObject()->indexOfProperty(alias.at(2).toUtf8().constData());
            if (valueTypeIndex == -1)
                COMPILE_EXCEPTION(prop.defaultValue, tr("Invalid alias location"));
            Q_ASSERT(valueTypeIndex <= 0xFF);
            
            aliasProperty = valueType->metaObject()->property(valueTypeIndex);
            propIdx |= (valueTypeIndex << 16);

            // update the property type
            type = aliasProperty.userType();
        }

        if (aliasProperty.isEnumType()) 
            typeName = "int";  // Avoid introducing a dependency on the aliased metaobject
        else
            typeName = aliasProperty.typeName();
    } else {
        Q_ASSERT(idObject->type != -1); // How else did it get an id?

        const QQmlCompiledData::TypeReference &ref = output->types.at(idObject->type);
        if (ref.type)
            typeName = ref.type->typeName();
        else
            typeName = ref.component->root->className();

        typeName += '*';
    }

    if (typeName.endsWith('*'))
        flags |= QML_ALIAS_FLAG_PTR;

    if (type == QMetaType::UnknownType) {
        Q_ASSERT(!typeName.isEmpty());
        type = QMetaType::type(typeName);
        Q_ASSERT(type != QMetaType::UnknownType);
        Q_ASSERT(type != QMetaType::Void);
    }

    QQmlVMEMetaData::AliasData aliasData = { idObject->idIndex, propIdx, flags };

    typedef QQmlVMEMetaData VMD;
    VMD *vmd = (QQmlVMEMetaData *)data.data();
    *(vmd->aliasData() + aliasIndex) = aliasData;

    int propertyFlags = 0;
    if (writable)
        propertyFlags |= QFastMetaBuilder::Writable;
    if (resettable)
        propertyFlags |= QFastMetaBuilder::Resettable;

    builder.setProperty(propIndex, prop.nameRef, type,
                        (QFastMetaBuilder::PropertyFlag)propertyFlags,
                        propIndex);

    return true;
}

bool QQmlCompiler::buildBinding(QQmlScript::Value *value,
                                        QQmlScript::Property *prop,
                                        const BindingContext &ctxt)
{
    Q_ASSERT(prop->index != -1);
    Q_ASSERT(prop->parent);
    Q_ASSERT(prop->parent->metaObject());

    if (!prop->core.isWritable() && !prop->core.isQList() && !prop->isReadOnlyDeclaration)
        COMPILE_EXCEPTION(prop, tr("Invalid property assignment: \"%1\" is a read-only property").arg(prop->name().toString()));

    JSBindingReference *reference = pool->New<JSBindingReference>();
    reference->expression = value->value;
    reference->property = prop;
    reference->value = value;
    reference->bindingContext = ctxt;
    addBindingReference(reference);

    return true;
}

bool QQmlCompiler::buildLiteralBinding(QQmlScript::Value *v,
                                               QQmlScript::Property *prop,
                                               const QQmlCompilerTypes::BindingContext &)
{
    Q_ASSERT(v->value.isScript());

    if (!prop->core.isWritable())
        return false;

    AST::Node *binding = v->value.asAST();

    if (prop->type == QVariant::String) {
        if (AST::CallExpression *e = AST::cast<AST::CallExpression *>(binding)) {
            if (AST::IdentifierExpression *i = AST::cast<AST::IdentifierExpression *>(e->base)) {
                if (i->name == qsTrId_string) {
                    AST::ArgumentList *arg1 = e->arguments?e->arguments:0;
                    AST::ArgumentList *arg2 = arg1?arg1->next:0;

                    if (arg1 && arg1->expression->kind == AST::Node::Kind_StringLiteral &&
                        (!arg2 || arg2->expression->kind == AST::Node::Kind_NumericLiteral) &&
                        (!arg2 || !arg2->next)) {

                        QStringRef text;
                        int n = -1;

                        text = AST::cast<AST::StringLiteral *>(arg1->expression)->value;
                        if (arg2) n = (int)AST::cast<AST::NumericLiteral *>(arg2->expression)->value;

                        TrBindingReference *reference = pool->New<TrBindingReference>();
                        reference->dataType = BindingReference::TrId;
                        reference->text = text;
                        reference->n = n;
                        v->bindingReference = reference;
                        return true;
                    }

                } else if (i->name == qsTr_string) {

                    AST::ArgumentList *arg1 = e->arguments?e->arguments:0;
                    AST::ArgumentList *arg2 = arg1?arg1->next:0;
                    AST::ArgumentList *arg3 = arg2?arg2->next:0;

                    if (arg1 && arg1->expression->kind == AST::Node::Kind_StringLiteral &&
                        (!arg2 || arg2->expression->kind == AST::Node::Kind_StringLiteral) &&
                        (!arg3 || arg3->expression->kind == AST::Node::Kind_NumericLiteral) &&
                        (!arg3 || !arg3->next)) {

                        QStringRef text;
                        QStringRef comment;
                        int n = -1;

                        text = AST::cast<AST::StringLiteral *>(arg1->expression)->value;
                        if (arg2) comment = AST::cast<AST::StringLiteral *>(arg2->expression)->value;
                        if (arg3) n = (int)AST::cast<AST::NumericLiteral *>(arg3->expression)->value;

                        TrBindingReference *reference = pool->New<TrBindingReference>();
                        reference->dataType = BindingReference::Tr;
                        reference->text = text;
                        reference->comment = comment;
                        reference->n = n;
                        v->bindingReference = reference;
                        return true;
                    }

                }
            }
        }

    }

    return false;
}

void QQmlCompiler::genBindingAssignment(QQmlScript::Value *binding,
                                                QQmlScript::Property *prop,
                                                QQmlScript::Object *obj,
                                                QQmlScript::Property *valueTypeProperty)
{
    Q_UNUSED(obj);
    Q_ASSERT(binding->bindingReference);

    const BindingReference &ref = *binding->bindingReference;
    if (ref.dataType == BindingReference::TrId) {
        const TrBindingReference &tr = static_cast<const TrBindingReference &>(ref);

        Instruction::StoreTrIdString store;
        store.propertyIndex = prop->core.coreIndex;
        store.text = output->indexForByteArray(tr.text.toUtf8());
        store.n = tr.n;
        output->addInstruction(store);
    } else if (ref.dataType == BindingReference::Tr) {
        const TrBindingReference &tr = static_cast<const TrBindingReference &>(ref);

        Instruction::StoreTrString store;
        store.propertyIndex = prop->core.coreIndex;
        store.context = translationContextIndex();
        store.text = output->indexForByteArray(tr.text.toUtf8());
        store.comment = output->indexForByteArray(tr.comment.toUtf8());
        store.n = tr.n;
        output->addInstruction(store);
    } else if (ref.dataType == BindingReference::V4) {
        const JSBindingReference &js = static_cast<const JSBindingReference &>(ref);

        Instruction::StoreV4Binding store;
        store.value = js.compiledIndex;
        store.context = js.bindingContext.stack;
        store.owner = js.bindingContext.owner;
        store.isAlias = prop->isAlias;
        if (valueTypeProperty) {
            store.property = (valueTypeProperty->index & 0xFFFF) |
                             ((valueTypeProperty->type & 0xFF)) << 16 |
                             ((prop->index & 0xFF) << 24);
            store.isRoot = (compileState->root == valueTypeProperty->parent);
        } else {
            store.property = prop->index;
            store.isRoot = (compileState->root == obj);
        }
        store.line = binding->location.start.line;
        store.column = binding->location.start.column;
        output->addInstruction(store);
    } else if (ref.dataType == BindingReference::V8) {
        const JSBindingReference &js = static_cast<const JSBindingReference &>(ref);

        Instruction::StoreV8Binding store;
        store.value = js.compiledIndex;
        store.context = js.bindingContext.stack;
        store.owner = js.bindingContext.owner;
        store.isAlias = prop->isAlias;
        if (valueTypeProperty) {
            store.isRoot = (compileState->root == valueTypeProperty->parent);
        } else {
            store.isRoot = (compileState->root == obj);
        }
        store.line = binding->location.start.line;
        store.column = binding->location.start.column;

        Q_ASSERT(js.bindingContext.owner == 0 ||
                 (js.bindingContext.owner != 0 && valueTypeProperty));
        if (js.bindingContext.owner) {
            store.property = genValueTypeData(prop, valueTypeProperty);
        } else {
            store.property = prop->core;
        }

        output->addInstruction(store);
    } else if (ref.dataType == BindingReference::QtScript) {
        const JSBindingReference &js = static_cast<const JSBindingReference &>(ref);

        Instruction::StoreBinding store;
        store.value = output->indexForString(js.rewrittenExpression);
        store.context = js.bindingContext.stack;
        store.owner = js.bindingContext.owner;
        store.line = binding->location.start.line;
        store.column = binding->location.start.column;
        store.isAlias = prop->isAlias;

        if (valueTypeProperty) {
            store.isRoot = (compileState->root == valueTypeProperty->parent);
        } else {
            store.isRoot = (compileState->root == obj);
        }

        Q_ASSERT(js.bindingContext.owner == 0 ||
                 (js.bindingContext.owner != 0 && valueTypeProperty));
        if (js.bindingContext.owner) {
            store.property = genValueTypeData(prop, valueTypeProperty);
        } else {
            store.property = prop->core;
        }

        output->addInstruction(store);
    } else {
        Q_ASSERT(!"Unhandled BindingReference::DataType type");
    }
}

int QQmlCompiler::genContextCache()
{
    if (compileState->ids.count() == 0)
        return -1;

    QQmlIntegerCache *cache = new QQmlIntegerCache();
    cache->reserve(compileState->ids.count());
    for (Object *o = compileState->ids.first(); o; o = compileState->ids.next(o)) 
        cache->add(o->id, o->idIndex);

    output->contextCaches.append(cache);
    return output->contextCaches.count() - 1;
}

QQmlPropertyData
QQmlCompiler::genValueTypeData(QQmlScript::Property *valueTypeProp, 
                                       QQmlScript::Property *prop)
{
    typedef QQmlPropertyPrivate QDPP;
    return QDPP::saveValueType(prop->parent->metaObject(), prop->index, 
                               enginePrivate->valueTypes[prop->type]->metaObject(), 
                               valueTypeProp->index, engine);
}

bool QQmlCompiler::completeComponentBuild()
{
    if (componentStats)
        componentStats->componentStat.ids = compileState->ids.count();

    for (Object *aliasObject = compileState->aliasingObjects.first(); aliasObject; 
         aliasObject = compileState->aliasingObjects.next(aliasObject)) 
        COMPILE_CHECK(buildDynamicMeta(aliasObject, ResolveAliases));

    QV4Compiler::Expression expr(unit->imports());
    expr.component = compileState->root;
    expr.ids = &compileState->ids;
    expr.importCache = output->importCache;

    QV4Compiler bindingCompiler;

    QList<JSBindingReference*> sharedBindings;

    for (JSBindingReference *b = compileState->bindings.first(); b; b = b->nextReference) {

        JSBindingReference &binding = *b;

        // First try v4
        expr.context = binding.bindingContext.object;
        expr.property = binding.property;
        expr.expression = binding.expression;

        int index = bindingCompiler.compile(expr, enginePrivate);
        if (index != -1) {
            binding.dataType = BindingReference::V4;
            binding.compiledIndex = index;
            if (componentStats)
                componentStats->componentStat.optimizedBindings.append(b->value->location);
            continue;
        }

        // Pre-rewrite the expression
        QString expression = binding.expression.asScript();

        QQmlRewrite::RewriteBinding rewriteBinding;
        rewriteBinding.setName(QLatin1Char('$')+binding.property->name().toString());
        bool isSharable = false;
        binding.rewrittenExpression = rewriteBinding(binding.expression.asAST(), expression, &isSharable);

        if (isSharable && binding.property->type != qMetaTypeId<QQmlBinding*>()) {
            binding.dataType = BindingReference::V8;
            sharedBindings.append(b);

            if (componentStats)
                componentStats->componentStat.sharedBindings.append(b->value->location);
        } else {
            binding.dataType = BindingReference::QtScript;

            if (componentStats)
                componentStats->componentStat.scriptBindings.append(b->value->location);
        }
    }

    if (!sharedBindings.isEmpty()) {
        struct Sort {
            static bool lt(const JSBindingReference *lhs, const JSBindingReference *rhs)
            {
                return lhs->value->location.start.line < rhs->value->location.start.line;
            }
        };

        qSort(sharedBindings.begin(), sharedBindings.end(), Sort::lt);

        int startLineNumber = sharedBindings.at(0)->value->location.start.line;
        int lineNumber = startLineNumber;

        QByteArray functionArray("[", 1);
        for (int ii = 0; ii < sharedBindings.count(); ++ii) {

            JSBindingReference *reference = sharedBindings.at(ii);
            QQmlScript::Value *value = reference->value;
            const QString &expression = reference->rewrittenExpression;

            if (ii != 0) functionArray.append(",", 1);

            while (lineNumber < value->location.start.line) {
                lineNumber++;
                functionArray.append("\n", 1);
            }

            functionArray += expression.toUtf8();
            lineNumber += expression.count(QLatin1Char('\n'));
            reference->compiledIndex = ii;
        }
        functionArray.append("]", 1);

        compileState->v8BindingProgram = functionArray;
        compileState->v8BindingProgramLine = startLineNumber;
    }

    if (bindingCompiler.isValid()) 
        compileState->compiledBindingData = bindingCompiler.program();

    // Check pop()'s matched push()'s
    Q_ASSERT(compileState->objectDepth.depth() == 0);
    Q_ASSERT(compileState->listDepth.depth() == 0);

    saveComponentState();

    return true;
}

void QQmlCompiler::dumpStats()
{
    Q_ASSERT(componentStats);
    qWarning().nospace() << "QML Document: " << output->url.toString();
    for (int ii = 0; ii < componentStats->savedComponentStats.count(); ++ii) {
        const ComponentStat &stat = componentStats->savedComponentStats.at(ii);
        qWarning().nospace() << "    Component Line " << stat.lineNumber;
        qWarning().nospace() << "        Total Objects:      " << stat.objects;
        qWarning().nospace() << "        IDs Used:           " << stat.ids;
        qWarning().nospace() << "        Optimized Bindings: " << stat.optimizedBindings.count();

        {
        QByteArray output;
        for (int ii = 0; ii < stat.optimizedBindings.count(); ++ii) {
            if (0 == (ii % 10)) {
                if (ii) output.append("\n");
                output.append("            ");
            }

            output.append('(');
            output.append(QByteArray::number(stat.optimizedBindings.at(ii).start.line));
            output.append(':');
            output.append(QByteArray::number(stat.optimizedBindings.at(ii).start.column));
            output.append(") ");
        }
        if (!output.isEmpty())
            qWarning().nospace() << output.constData();
        }

        qWarning().nospace() << "        Shared Bindings:    " << stat.sharedBindings.count();
        {
        QByteArray output;
        for (int ii = 0; ii < stat.sharedBindings.count(); ++ii) {
            if (0 == (ii % 10)) {
                if (ii) output.append('\n');
                output.append("            ");
            }

            output.append('(');
            output.append(QByteArray::number(stat.sharedBindings.at(ii).start.line));
            output.append(':');
            output.append(QByteArray::number(stat.sharedBindings.at(ii).start.column));
            output.append(") ");
        }
        if (!output.isEmpty())
            qWarning().nospace() << output.constData();
        }

        qWarning().nospace() << "        QScript Bindings:   " << stat.scriptBindings.count();
        {
        QByteArray output;
        for (int ii = 0; ii < stat.scriptBindings.count(); ++ii) {
            if (0 == (ii % 10)) {
                if (ii) output.append('\n');
                output.append("            ");
            }

            output.append('(');
            output.append(QByteArray::number(stat.scriptBindings.at(ii).start.line));
            output.append(':');
            output.append(QByteArray::number(stat.scriptBindings.at(ii).start.column));
            output.append(") ");
        }
        if (!output.isEmpty())
            qWarning().nospace() << output.constData();
        }
    }
}

/*!
    Returns true if from can be assigned to a (QObject) property of type
    to.
*/
bool QQmlCompiler::canCoerce(int to, QQmlScript::Object *from)
{
    const QMetaObject *toMo = enginePrivate->rawMetaObjectForType(to);
    const QMetaObject *fromMo = from->metaObject();

    while (fromMo) {
        if (QQmlPropertyPrivate::equal(fromMo, toMo))
            return true;
        fromMo = fromMo->superClass();
    }
    return false;
}

/*!
    Returns the element name, as written in the QML file, for o.
*/
QString QQmlCompiler::elementName(QQmlScript::Object *o)
{
    Q_ASSERT(o);
    if (o->type != -1) {
        return unit->parser().referencedTypes().at(o->type)->name;
    } else {
        return QString();
    }
}

QQmlType *QQmlCompiler::toQmlType(QQmlScript::Object *from)
{
    if (from->type != -1 && output->types.at(from->type).type)
        return output->types.at(from->type).type;

    // ### Optimize
    const QMetaObject *mo = from->metatype;
    QQmlType *type = 0;
    while (!type && mo) {
        type = QQmlMetaType::qmlType(mo);
        mo = mo->superClass();
    }
   return type;
}

QStringList QQmlCompiler::deferredProperties(QQmlScript::Object *obj)
{
    const QMetaObject *mo = obj->metatype;

    int idx = mo->indexOfClassInfo("DeferredPropertyNames");
    if (idx == -1)
        return QStringList();

    QMetaClassInfo classInfo = mo->classInfo(idx);
    QStringList rv = QString::fromUtf8(classInfo.value()).split(QLatin1Char(','));
    return rv;
}

QQmlPropertyData *
QQmlCompiler::property(QQmlScript::Object *object, int index)
{
    QQmlPropertyCache *cache = 0;

    if (object->synthCache)
        cache = object->synthCache;
    else if (object->type != -1)
        cache = output->types[object->type].createPropertyCache(engine);
    else
        cache = QQmlEnginePrivate::get(engine)->cache(object->metaObject());

    return cache->property(index);
}

QQmlPropertyData *
QQmlCompiler::property(QQmlScript::Object *object, const QHashedStringRef &name, bool *notInRevision)
{
    if (notInRevision) *notInRevision = false;

    QQmlPropertyCache *cache = 0;

    if (object->synthCache)
        cache = object->synthCache;
    else if (object->type != -1)
        cache = output->types[object->type].createPropertyCache(engine);
    else
        cache = QQmlEnginePrivate::get(engine)->cache(object->metaObject());

    QQmlPropertyData *d = cache->property(name);

    // Find the first property
    while (d && d->isFunction())
        d = cache->overrideData(d);

    if (d && !cache->isAllowedInRevision(d)) {
        if (notInRevision) *notInRevision = true;
        return 0;
    } else {
        return d;
    }
}

// This code must match the semantics of QQmlPropertyPrivate::findSignalByName
QQmlPropertyData *
QQmlCompiler::signal(QQmlScript::Object *object, const QHashedStringRef &name, bool *notInRevision)
{
    if (notInRevision) *notInRevision = false;

    QQmlPropertyCache *cache = 0;

    if (object->synthCache)
        cache = object->synthCache;
    else if (object->type != -1)
        cache = output->types[object->type].createPropertyCache(engine);
    else
        cache = QQmlEnginePrivate::get(engine)->cache(object->metaObject());


    QQmlPropertyData *d = cache->property(name);
    if (notInRevision) *notInRevision = false;

    while (d && !(d->isFunction()))
        d = cache->overrideData(d);

    if (d && !cache->isAllowedInRevision(d)) {
        if (notInRevision) *notInRevision = true;
        return 0;
    } else if (d) {
        return d;
    }

    if (name.endsWith(Changed_string)) {
        QHashedStringRef propName = name.mid(0, name.length() - Changed_string.length());

        d = property(object, propName, notInRevision);
        if (d) 
            return cache->method(d->notifyIndex);
    }

    return 0;
}

// This code must match the semantics of QQmlPropertyPrivate::findSignalByName
int QQmlCompiler::indexOfSignal(QQmlScript::Object *object, const QString &name, 
                                        bool *notInRevision)
{
    QQmlPropertyData *d = signal(object, QStringRef(&name), notInRevision);
    return d?d->coreIndex:-1;
}

int QQmlCompiler::indexOfProperty(QQmlScript::Object *object, const QString &name, 
                                          bool *notInRevision)
{
    return indexOfProperty(object, QStringRef(&name), notInRevision);
}

int QQmlCompiler::indexOfProperty(QQmlScript::Object *object, const QHashedStringRef &name, 
                                          bool *notInRevision)
{
    QQmlPropertyData *d = property(object, name, notInRevision);
    return d?d->coreIndex:-1;
}

QT_END_NAMESPACE
