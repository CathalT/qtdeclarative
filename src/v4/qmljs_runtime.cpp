/****************************************************************************
**
** Copyright (C) 2012 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the V4VM module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qv4global.h"
#include "debugging.h"
#include "qmljs_runtime.h"
#include "qv4object.h"
#include "qv4ir_p.h"
#include "qv4objectproto.h"
#include "qv4globalobject.h"
#include "qv4stringobject.h"
#include "private/qlocale_tools_p.h"

#include <QtCore/qmath.h>
#include <QtCore/qnumeric.h>
#include <QtCore/QDebug>
#include <cstdio>
#include <cassert>
#include <typeinfo>
#include <stdlib.h>

#include "../3rdparty/double-conversion/double-conversion.h"

namespace QQmlJS {
namespace VM {

QString numberToString(double num, int radix = 10)
{
    if (isnan(num)) {
        return QStringLiteral("NaN");
    } else if (qIsInf(num)) {
        return QLatin1String(num < 0 ? "-Infinity" : "Infinity");
    }

    if (radix == 10) {
        char str[100];
        double_conversion::StringBuilder builder(str, sizeof(str));
        double_conversion::DoubleToStringConverter::EcmaScriptConverter().ToShortest(num, &builder);
        return QString::fromLatin1(builder.Finalize());
    }

    QString str;
    bool negative = false;

    if (num < 0) {
        negative = true;
        num = -num;
    }

    double frac = num - ::floor(num);
    num = Value::toInteger(num);

    do {
        char c = (char)::fmod(num, radix);
        c = (c < 10) ? (c + '0') : (c - 10 + 'a');
        str.prepend(QLatin1Char(c));
        num = ::floor(num / radix);
    } while (num != 0);

    if (frac != 0) {
        str.append(QLatin1Char('.'));
        do {
            frac = frac * radix;
            char c = (char)::floor(frac);
            c = (c < 10) ? (c + '0') : (c - 10 + 'a');
            str.append(QLatin1Char(c));
            frac = frac - ::floor(frac);
        } while (frac != 0);
    }

    if (negative)
        str.prepend(QLatin1Char('-'));

    return str;
}

extern "C" {

Value __qmljs_init_closure(VM::Function *clos, ExecutionContext *ctx)
{
    assert(clos);
    return Value::fromObject(ctx->engine->newScriptFunction(ctx, clos));
}

Function *__qmljs_register_function(ExecutionContext *ctx, String *name,
                                    bool hasDirectEval,
                                    bool usesArgumentsObject, bool isStrict,
                                    bool hasNestedFunctions,
                                    String **formals, unsigned formalCount,
                                    String **locals, unsigned localCount)
{
    Function *f = ctx->engine->newFunction(name ? name->toQString() : QString());

    f->hasDirectEval = hasDirectEval;
    f->usesArgumentsObject = usesArgumentsObject;
    f->isStrict = isStrict;
    f->hasNestedFunctions = hasNestedFunctions;

    for (unsigned i = 0; i < formalCount; ++i)
        if (formals[i])
            f->formals.append(formals[i]);
    for (unsigned i = 0; i < localCount; ++i)
        if (locals[i])
            f->locals.append(locals[i]);

    return f;
}

Value __qmljs_string_literal_undefined(ExecutionContext *ctx)
{
    return Value::fromString(ctx->engine->newString(QStringLiteral("undefined")));
}

Value __qmljs_string_literal_null(ExecutionContext *ctx)
{
    return Value::fromString(ctx->engine->newString(QStringLiteral("null")));
}

Value __qmljs_string_literal_true(ExecutionContext *ctx)
{
    return Value::fromString(ctx->engine->newString(QStringLiteral("true")));
}

Value __qmljs_string_literal_false(ExecutionContext *ctx)
{
    return Value::fromString(ctx->engine->newString(QStringLiteral("false")));
}

Value __qmljs_string_literal_object(ExecutionContext *ctx)
{
    return Value::fromString(ctx->engine->newString(QStringLiteral("object")));
}

Value __qmljs_string_literal_boolean(ExecutionContext *ctx)
{
    return Value::fromString(ctx->engine->newString(QStringLiteral("boolean")));
}

Value __qmljs_string_literal_number(ExecutionContext *ctx)
{
    return Value::fromString(ctx->engine->newString(QStringLiteral("number")));
}

Value __qmljs_string_literal_string(ExecutionContext *ctx)
{
    return Value::fromString(ctx->engine->newString(QStringLiteral("string")));
}

Value __qmljs_string_literal_function(ExecutionContext *ctx)
{
    return Value::fromString(ctx->engine->newString(QStringLiteral("function")));
}

Value __qmljs_delete_subscript(ExecutionContext *ctx, const Value &base, Value index)
{
    if (Object *o = base.asObject()) {
        uint n = UINT_MAX;
        if (index.isInteger())
            n = index.integerValue();
        else if (index.isDouble())
            n = index.doubleValue();
        if (n < UINT_MAX)
            return Value::fromBoolean(o->__delete__(ctx, n));
    }

    String *name = index.toString(ctx);
    return __qmljs_delete_member(ctx, base, name);
}

Value __qmljs_delete_member(ExecutionContext *ctx, const Value &base, String *name)
{
    Value obj = base.toObject(ctx);
    return Value::fromBoolean(obj.objectValue()->__delete__(ctx, name));
}

Value __qmljs_delete_name(ExecutionContext *ctx, String *name)
{
    return Value::fromBoolean(ctx->deleteProperty(name));
}

void __qmljs_add_helper(ExecutionContext *ctx, Value *result, const Value &left, const Value &right)
{
    Value pleft = __qmljs_to_primitive(left, ctx, PREFERREDTYPE_HINT);
    Value pright = __qmljs_to_primitive(right, ctx, PREFERREDTYPE_HINT);
    if (pleft.isString() || pright.isString()) {
        if (!pleft.isString())
            pleft = __qmljs_to_string(pleft, ctx);
        if (!pright.isString())
            pright = __qmljs_to_string(pright, ctx);
        String *string = __qmljs_string_concat(ctx, pleft.stringValue(), pright.stringValue());
        *result = Value::fromString(string);
        return;
    }
    double x = __qmljs_to_number(pleft, ctx);
    double y = __qmljs_to_number(pright, ctx);
    *result = Value::fromDouble(x + y);
}

void __qmljs_instanceof(ExecutionContext *ctx, Value *result, const Value &left, const Value &right)
{
    Object *o = right.asObject();
    if (!o)
        __qmljs_throw_type_error(ctx);

    bool r = o->hasInstance(ctx, left);
    *result = Value::fromBoolean(r);
}

void __qmljs_in(ExecutionContext *ctx, Value *result, const Value &left, const Value &right)
{
    if (!right.isObject())
        __qmljs_throw_type_error(ctx);
    String *s = left.toString(ctx);
    bool r = right.objectValue()->__hasProperty__(ctx, s);
    *result = Value::fromBoolean(r);
}

void __qmljs_inplace_bit_and_name(ExecutionContext *ctx, String *name, const Value &value)
{
    ctx->inplaceBitOp(name, value, __qmljs_bit_and);
}

void __qmljs_inplace_bit_or_name(ExecutionContext *ctx, String *name, const Value &value)
{
    ctx->inplaceBitOp(name, value, __qmljs_bit_or);
}

void __qmljs_inplace_bit_xor_name(ExecutionContext *ctx, String *name, const Value &value)
{
    ctx->inplaceBitOp(name, value, __qmljs_bit_xor);
}

void __qmljs_inplace_add_name(ExecutionContext *ctx, String *name, const Value &value)
{
    ctx->inplaceBitOp(name, value, __qmljs_add);
}

void __qmljs_inplace_sub_name(ExecutionContext *ctx, String *name, const Value &value)
{
    ctx->inplaceBitOp(name, value, __qmljs_sub);
}

void __qmljs_inplace_mul_name(ExecutionContext *ctx, String *name, const Value &value)
{
    ctx->inplaceBitOp(name, value, __qmljs_mul);
}

void __qmljs_inplace_div_name(ExecutionContext *ctx, String *name, const Value &value)
{
    ctx->inplaceBitOp(name, value, __qmljs_div);
}

void __qmljs_inplace_mod_name(ExecutionContext *ctx, String *name, const Value &value)
{
    ctx->inplaceBitOp(name, value, __qmljs_mod);
}

void __qmljs_inplace_shl_name(ExecutionContext *ctx, String *name, const Value &value)
{
    ctx->inplaceBitOp(name, value, __qmljs_shl);
}

void __qmljs_inplace_shr_name(ExecutionContext *ctx, String *name, const Value &value)
{
    ctx->inplaceBitOp(name, value, __qmljs_shr);
}

void __qmljs_inplace_ushr_name(ExecutionContext *ctx, String *name, const Value &value)
{
    ctx->inplaceBitOp(name, value, __qmljs_ushr);
}

void __qmljs_inplace_bit_and_element(ExecutionContext *ctx, const Value &base, const Value &index, const Value &rhs)
{
    Object *obj = base.toObject(ctx).objectValue();
    obj->inplaceBinOp(ctx, __qmljs_bit_and, index, rhs);
}

void __qmljs_inplace_bit_or_element(ExecutionContext *ctx, const Value &base, const Value &index, const Value &rhs)
{
    Object *obj = base.toObject(ctx).objectValue();
    obj->inplaceBinOp(ctx, __qmljs_bit_or, index, rhs);
}

void __qmljs_inplace_bit_xor_element(ExecutionContext *ctx, const Value &base, const Value &index, const Value &rhs)
{
    Object *obj = base.toObject(ctx).objectValue();
    obj->inplaceBinOp(ctx, __qmljs_bit_xor, index, rhs);
}

void __qmljs_inplace_add_element(ExecutionContext *ctx, const Value &base, const Value &index, const Value &rhs)
{
    Object *obj = base.toObject(ctx).objectValue();
    obj->inplaceBinOp(ctx, __qmljs_add, index, rhs);
}

void __qmljs_inplace_sub_element(ExecutionContext *ctx, const Value &base, const Value &index, const Value &rhs)
{
    Object *obj = base.toObject(ctx).objectValue();
    obj->inplaceBinOp(ctx, __qmljs_sub, index, rhs);
}

void __qmljs_inplace_mul_element(ExecutionContext *ctx, const Value &base, const Value &index, const Value &rhs)
{
    Object *obj = base.toObject(ctx).objectValue();
    obj->inplaceBinOp(ctx, __qmljs_mul, index, rhs);
}

void __qmljs_inplace_div_element(ExecutionContext *ctx, const Value &base, const Value &index, const Value &rhs)
{
    Object *obj = base.toObject(ctx).objectValue();
    obj->inplaceBinOp(ctx, __qmljs_div, index, rhs);
}

void __qmljs_inplace_mod_element(ExecutionContext *ctx, const Value &base, const Value &index, const Value &rhs)
{
    Object *obj = base.toObject(ctx).objectValue();
    obj->inplaceBinOp(ctx, __qmljs_mod, index, rhs);
}

void __qmljs_inplace_shl_element(ExecutionContext *ctx, const Value &base, const Value &index, const Value &rhs)
{
    Object *obj = base.toObject(ctx).objectValue();
    obj->inplaceBinOp(ctx, __qmljs_shl, index, rhs);
}

void __qmljs_inplace_shr_element(ExecutionContext *ctx, const Value &base, const Value &index, const Value &rhs)
{
    Object *obj = base.toObject(ctx).objectValue();
    obj->inplaceBinOp(ctx, __qmljs_shr, index, rhs);
}

void __qmljs_inplace_ushr_element(ExecutionContext *ctx, const Value &base, const Value &index, const Value &rhs)
{
    Object *obj = base.toObject(ctx).objectValue();
    obj->inplaceBinOp(ctx, __qmljs_ushr, index, rhs);
}

void __qmljs_inplace_bit_and_member(ExecutionContext *ctx, const Value &base, String *name, const Value &rhs)
{
    Object *o = base.toObject(ctx).objectValue();
    o->inplaceBinOp(ctx, __qmljs_bit_and, name, rhs);
}

void __qmljs_inplace_bit_or_member(ExecutionContext *ctx, const Value &base, String *name, const Value &rhs)
{
    Object *o = base.toObject(ctx).objectValue();
    o->inplaceBinOp(ctx, __qmljs_bit_or, name, rhs);
}

void __qmljs_inplace_bit_xor_member(ExecutionContext *ctx, const Value &base, String *name, const Value &rhs)
{
    Object *o = base.toObject(ctx).objectValue();
    o->inplaceBinOp(ctx, __qmljs_bit_xor, name, rhs);
}

void __qmljs_inplace_add_member(ExecutionContext *ctx, const Value &base, String *name, const Value &rhs)
{
    Object *o = base.toObject(ctx).objectValue();
    o->inplaceBinOp(ctx, __qmljs_add, name, rhs);
}

void __qmljs_inplace_sub_member(ExecutionContext *ctx, const Value &base, String *name, const Value &rhs)
{
    Object *o = base.toObject(ctx).objectValue();
    o->inplaceBinOp(ctx, __qmljs_sub, name, rhs);
}

void __qmljs_inplace_mul_member(ExecutionContext *ctx, const Value &base, String *name, const Value &rhs)
{
    Object *o = base.toObject(ctx).objectValue();
    o->inplaceBinOp(ctx, __qmljs_mul, name, rhs);
}

void __qmljs_inplace_div_member(ExecutionContext *ctx, const Value &base, String *name, const Value &rhs)
{
    Object *o = base.toObject(ctx).objectValue();
    o->inplaceBinOp(ctx, __qmljs_div, name, rhs);
}

void __qmljs_inplace_mod_member(ExecutionContext *ctx, const Value &base, String *name, const Value &rhs)
{
    Object *o = base.toObject(ctx).objectValue();
    o->inplaceBinOp(ctx, __qmljs_mod, name, rhs);
}

void __qmljs_inplace_shl_member(ExecutionContext *ctx, const Value &base, String *name, const Value &rhs)
{
    Object *o = base.toObject(ctx).objectValue();
    o->inplaceBinOp(ctx, __qmljs_shl, name, rhs);
}

void __qmljs_inplace_shr_member(ExecutionContext *ctx, const Value &base, String *name, const Value &rhs)
{
    Object *o = base.toObject(ctx).objectValue();
    o->inplaceBinOp(ctx, __qmljs_shr, name, rhs);
}

void __qmljs_inplace_ushr_member(ExecutionContext *ctx, const Value &base, String *name, const Value &rhs)
{
    Object *o = base.toObject(ctx).objectValue();
    o->inplaceBinOp(ctx, __qmljs_ushr, name, rhs);
}

String *__qmljs_string_from_utf8(ExecutionContext *ctx, const char *s)
{
    return ctx->engine->newString(QString::fromUtf8(s));
}

String *__qmljs_identifier_from_utf8(ExecutionContext *ctx, const char *s)
{
    return ctx->engine->newString(QString::fromUtf8(s));
}

int __qmljs_string_length(ExecutionContext *, String *string)
{
    return string->toQString().length();
}

double __qmljs_string_to_number(const String *string)
{
    QString s = string->toQString();
    s = s.trimmed();
    if (s.startsWith(QLatin1String("0x")) || s.startsWith(QLatin1String("0X")))
        return s.toLong(0, 16);
    bool ok;
    QByteArray ba = s.toLatin1();
    const char *begin = ba.constData();
    const char *end = 0;
    double d = qstrtod(begin, &end, &ok);
    if (end - begin != ba.size()) {
        if (ba == "Infinity" || ba == "+Infinity")
            d = Q_INFINITY;
        else if (ba == "-Infinity")
            d = -Q_INFINITY;
        else
            d = std::numeric_limits<double>::quiet_NaN();
    }
    return d;
}

Value __qmljs_string_from_number(ExecutionContext *ctx, double number)
{
    String *string = ctx->engine->newString(numberToString(number, 10));
    return Value::fromString(string);
}

Bool __qmljs_string_compare(ExecutionContext *, String *left, String *right)
{
    return left->toQString() < right->toQString();
}

Bool __qmljs_string_equal(String *left, String *right)
{
    return left->isEqualTo(right);
}

String *__qmljs_string_concat(ExecutionContext *ctx, String *first, String *second)
{
    return ctx->engine->newString(first->toQString() + second->toQString());
}

Bool __qmljs_is_function(Value value)
{
    return value.objectValue()->asFunctionObject() != 0;
}

Value __qmljs_object_default_value(ExecutionContext *ctx, Value object, int typeHint)
{
    if (typeHint == PREFERREDTYPE_HINT) {
        if (object.asDateObject())
            typeHint = STRING_HINT;
        else
            typeHint = NUMBER_HINT;
    }

    String *meth1 = ctx->engine->newString("toString");
    String *meth2 = ctx->engine->newString("valueOf");

    if (typeHint == NUMBER_HINT)
        qSwap(meth1, meth2);

    assert(object.isObject());
    Object *oo = object.objectValue();

    Value conv = oo->__get__(ctx, meth1);
    if (FunctionObject *o = conv.asFunctionObject()) {
        Value r = o->call(ctx, object, 0, 0);
        if (r.isPrimitive())
            return r;
    }

    conv = oo->__get__(ctx, meth2);
    if (FunctionObject *o = conv.asFunctionObject()) {
        Value r = o->call(ctx, object, 0, 0);
        if (r.isPrimitive())
            return r;
    }

    ctx->throwTypeError();
    return Value::undefinedValue();
}

void __qmljs_throw_type_error(ExecutionContext *ctx)
{
    ctx->throwTypeError();
}

Value __qmljs_new_object(ExecutionContext *ctx)
{
    return Value::fromObject(ctx->engine->newObject());
}

Value __qmljs_new_boolean_object(ExecutionContext *ctx, bool boolean)
{
    Value value = Value::fromBoolean(boolean);
    return Value::fromObject(ctx->engine->newBooleanObject(value));
}

Value __qmljs_new_number_object(ExecutionContext *ctx, double number)
{
    Value value = Value::fromDouble(number);
    return Value::fromObject(ctx->engine->newNumberObject(value));
}

Value __qmljs_new_string_object(ExecutionContext *ctx, String *string)
{
    Value value = Value::fromString(string);
    return Value::fromObject(ctx->engine->newStringObject(ctx, value));
}

void __qmljs_set_property(ExecutionContext *ctx, const Value &object, String *name, const Value &value)
{
    Object *o = object.isObject() ? object.objectValue() : __qmljs_to_object(object, ctx).objectValue();
    o->__put__(ctx, name, value);
}

Value __qmljs_get_element(ExecutionContext *ctx, Value object, Value index)
{
    uint type = object.type();
    uint idx = index.asArrayIndex();

    if (type != Value::Object_Type) {
        if (type == Value::String_Type && idx < UINT_MAX) {
            String *str = object.stringValue();
            if (idx >= (uint)str->toQString().length())
                return Value::undefinedValue();
            const QString s = str->toQString().mid(idx, 1);
            return Value::fromString(ctx, s);
        }

        object = __qmljs_to_object(object, ctx);
    }

    Object *o = object.objectValue();

    if (idx < UINT_MAX) {
        const PropertyDescriptor *p = o->nonSparseArrayAt(idx);
        if (p && p->type == PropertyDescriptor::Data)
            return p->value;

        return o->__get__(ctx, idx);
    }

    String *name = index.toString(ctx);
    return o->__get__(ctx, name);
}

void __qmljs_set_element(ExecutionContext *ctx, Value object, Value index, Value value)
{
    if (!object.isObject())
        object = __qmljs_to_object(object, ctx);

    Object *o = object.objectValue();

    uint idx = index.asArrayIndex();
    if (idx < UINT_MAX) {
        PropertyDescriptor *p = o->nonSparseArrayAt(idx);
        if (p && p->type == PropertyDescriptor::Data && p->isWritable()) {
            p->value = value;
            return;
        }
        o->__put__(ctx, idx, value);
        return;
    }

    String *name = index.toString(ctx);
    o->__put__(ctx, name, value);
}

Value __qmljs_foreach_iterator_object(Value in, ExecutionContext *ctx)
{
    if (!in.isNull() && !in.isUndefined())
        in = __qmljs_to_object(in, ctx);
    Object *it = ctx->engine->newForEachIteratorObject(ctx, in.asObject());
    return Value::fromObject(it);
}

Value __qmljs_foreach_next_property_name(Value foreach_iterator)
{
    assert(foreach_iterator.isObject());

    ForEachIteratorObject *it = static_cast<ForEachIteratorObject *>(foreach_iterator.objectValue());
    assert(it->asForeachIteratorObject());

    return it->nextPropertyName();
}


void __qmljs_set_activation_property(ExecutionContext *ctx, String *name, const Value &value)
{
    ctx->setProperty(name, value);
}

void __qmljs_get_property(ExecutionContext *ctx, Value *result, const Value &object, String *name)
{
    Value res;
    Object *o = object.asObject();
    if (o) {
        res = o->__get__(ctx, name);
    } else if (object.isString() && name->isEqualTo(ctx->engine->id_length)) {
        res = Value::fromInt32(object.stringValue()->toQString().length());
    } else {
        o = __qmljs_to_object(object, ctx).objectValue();
        res = o->__get__(ctx, name);
    }
    if (result)
        *result = res;
}

void __qmljs_get_activation_property(ExecutionContext *ctx, Value *result, String *name)
{
    *result = ctx->getProperty(name);
}

void __qmljs_get_property_lookup(ExecutionContext *ctx, Value *result, const Value &object, int lookupIndex)
{
    Value res;
    Lookup *l = ctx->lookups + lookupIndex;
    if (Object *o = object.asObject()) {
        PropertyDescriptor *p = 0;
        if (o->internalClass == l->mainClass) {
            if (!l->protoClass) {
                p = o->memberData + l->index;
            } else if (o->prototype && o->prototype->internalClass == l->protoClass) {
                p = o->prototype->memberData + l->index;
            }
        }

        if (!p) {
            uint idx = o->internalClass->find(l->name);
            if (idx < UINT_MAX) {
                l->mainClass = o->internalClass;
                l->protoClass = 0;
                l->index = idx;
                p = o->memberData + idx;
            } else if (o->prototype) {
                idx = o->prototype->internalClass->find(l->name);
                if (idx < UINT_MAX) {
                    l->mainClass = o->internalClass;
                    l->protoClass = o->prototype->internalClass;
                    l->index = idx;
                    p = o->prototype->memberData + idx;
                }
            }
        }

        if (p)
            res = p->type == PropertyDescriptor::Data ? p->value : o->getValue(ctx, p);
        else
            res = o->__get__(ctx, l->name);
    } else {
        if (object.isString() && l->name->isEqualTo(ctx->engine->id_length)) {
            res = Value::fromInt32(object.stringValue()->toQString().length());
        } else {
            o = __qmljs_to_object(object, ctx).objectValue();
            res = o->__get__(ctx, l->name);
        }
    }
    if (result)
        *result = res;
}

void __qmljs_set_property_lookup(ExecutionContext *ctx, const Value &object, int lookupIndex, const Value &value)
{
    Object *o = object.isObject() ? object.objectValue() : __qmljs_to_object(object, ctx).objectValue();
    Lookup *l = ctx->lookups + lookupIndex;

    if (l->index != ArrayObject::LengthPropertyIndex || !o->isArrayObject()) {
        if (o->internalClass == l->mainClass) {
            o->putValue(ctx, o->memberData + l->index, value);
            return;
        }

        uint idx = o->internalClass->find(l->name);
        if (idx < UINT_MAX) {
            l->mainClass = o->internalClass;
            l->index = idx;
            return o->putValue(ctx, o->memberData + idx, value);
        }
    }

    o->__put__(ctx, l->name, value);
}


Value __qmljs_get_thisObject(ExecutionContext *ctx)
{
    return ctx->thisObject;
}

uint __qmljs_equal(const Value &x, const Value &y, ExecutionContext *ctx)
{
    if (x.type() == y.type()) {
        switch (x.type()) {
        case Value::Undefined_Type:
            return true;
        case Value::Null_Type:
            return true;
        case Value::Boolean_Type:
            return x.booleanValue() == y.booleanValue();
            break;
        case Value::Integer_Type:
            return x.integerValue() == y.integerValue();
        case Value::String_Type:
            return x.stringValue()->isEqualTo(y.stringValue());
        case Value::Object_Type:
            return x.objectValue() == y.objectValue();
        default: // double
            return x.doubleValue() == y.doubleValue();
        }
        // unreachable
    } else {
        if (x.isNumber() && y.isNumber())
            return x.asDouble() == y.asDouble();
        if (x.isNull() && y.isUndefined()) {
            return true;
        } else if (x.isUndefined() && y.isNull()) {
            return true;
        } else if (x.isNumber() && y.isString()) {
            Value ny = Value::fromDouble(__qmljs_to_number(y, ctx));
            return __qmljs_equal(x, ny, ctx);
        } else if (x.isString() && y.isNumber()) {
            Value nx = Value::fromDouble(__qmljs_to_number(x, ctx));
            return __qmljs_equal(nx, y, ctx);
        } else if (x.isBoolean()) {
            Value nx = Value::fromDouble((double) x.booleanValue());
            return __qmljs_equal(nx, y, ctx);
        } else if (y.isBoolean()) {
            Value ny = Value::fromDouble((double) y.booleanValue());
            return __qmljs_equal(x, ny, ctx);
        } else if ((x.isNumber() || x.isString()) && y.isObject()) {
            Value py = __qmljs_to_primitive(y, ctx, PREFERREDTYPE_HINT);
            return __qmljs_equal(x, py, ctx);
        } else if (x.isObject() && (y.isNumber() || y.isString())) {
            Value px = __qmljs_to_primitive(x, ctx, PREFERREDTYPE_HINT);
            return __qmljs_equal(px, y, ctx);
        }
    }

    return false;
}

void __qmljs_call_activation_property(ExecutionContext *context, Value *result, String *name, Value *args, int argc)
{
    Object *base;
    Value func = context->getPropertyAndBase(name, &base);
    FunctionObject *o = func.asFunctionObject();
    if (!o)
        context->throwTypeError();

    Value thisObject = base ? Value::fromObject(base) : Value::undefinedValue();

    if (o == context->engine->evalFunction && name->isEqualTo(context->engine->id_eval)) {
        Value res = static_cast<EvalFunction *>(o)->evalCall(context, thisObject, args, argc, true);
        if (result)
            *result = res;
        return;
    }

    Value res = o->call(context, thisObject, args, argc);
    if (result)
        *result = res;
}

void __qmljs_call_property(ExecutionContext *context, Value *result, const Value &thatObject, String *name, Value *args, int argc)
{
    Value thisObject = thatObject;
    Object *baseObject;
    if (thisObject.isString()) {
        baseObject = context->engine->stringPrototype;
    } else {
        if (!thisObject.isObject())
            thisObject = __qmljs_to_object(thisObject, context);

        assert(thisObject.isObject());
       baseObject = thisObject.objectValue();
    }

    Value func = baseObject->__get__(context, name);
    FunctionObject *o = func.asFunctionObject();
    if (!o)
        context->throwTypeError();

    Value res = o->call(context, thisObject, args, argc);
    if (result)
        *result = res;
}

void __qmljs_call_property_lookup(ExecutionContext *context, Value *result, const Value &thatObject, uint index, Value *args, int argc)
{
    Value thisObject = thatObject;
    Lookup *l = context->lookups + index;

    Object *baseObject;
    if (thisObject.isString()) {
        baseObject = context->engine->stringPrototype;
    } else {
        if (!thisObject.isObject())
            thisObject = __qmljs_to_object(thisObject, context);

       assert(thisObject.isObject());
       baseObject = thisObject.objectValue();
    }

    PropertyDescriptor *p = 0;
    if (baseObject->internalClass == l->mainClass) {
        if (!l->protoClass) {
            p = baseObject->memberData + l->index;
        } else if (baseObject->prototype && baseObject->prototype->internalClass == l->protoClass) {
            p = baseObject->prototype->memberData + l->index;
        }
    }

    if (!p) {
        uint idx = baseObject->internalClass->find(l->name);
        if (idx < UINT_MAX) {
            l->mainClass = baseObject->internalClass;
            l->protoClass = 0;
            l->index = idx;
            p = baseObject->memberData + idx;
        } else if (baseObject->prototype) {
            idx = baseObject->prototype->internalClass->find(l->name);
            if (idx < UINT_MAX) {
                l->mainClass = baseObject->internalClass;
                l->protoClass = baseObject->prototype->internalClass;
                l->index = idx;
                p = baseObject->prototype->memberData + idx;
            }
        }
    }

    Value func;
    if (p)
        func =  p->type == PropertyDescriptor::Data ? p->value : baseObject->getValue(context, p);
    else
        func = baseObject->__get__(context, l->name);

    FunctionObject *o = func.asFunctionObject();
    if (!o)
        context->throwTypeError();

    Value res = o->call(context, thisObject, args, argc);
    if (result)
        *result = res;
}

void __qmljs_call_element(ExecutionContext *context, Value *result, const Value &that, const Value &index, Value *args, int argc)
{
    Value thisObject = that;
    if (!thisObject.isObject())
        thisObject = __qmljs_to_object(thisObject, context);

    assert(thisObject.isObject());
    Object *baseObject = thisObject.objectValue();

    Value func = baseObject->__get__(context, index.toString(context));
    FunctionObject *o = func.asFunctionObject();
    if (!o)
        context->throwTypeError();

    Value res = o->call(context, thisObject, args, argc);
    if (result)
        *result = res;
}

void __qmljs_call_value(ExecutionContext *context, Value *result, const Value *thisObject, const Value &func, Value *args, int argc)
{
    FunctionObject *o = func.asFunctionObject();
    if (!o)
        context->throwTypeError();
    Value res = o->call(context, thisObject ? *thisObject : Value::undefinedValue(), args, argc);
    if (result)
        *result = res;
}

void __qmljs_construct_activation_property(ExecutionContext *context, Value *result, String *name, Value *args, int argc)
{
    Value func = context->getProperty(name);
    __qmljs_construct_value(context, result, func, args, argc);
}

void __qmljs_construct_value(ExecutionContext *context, Value *result, const Value &func, Value *args, int argc)
{
    if (FunctionObject *f = func.asFunctionObject()) {
        Value res = f->construct(context, args, argc);
        if (result)
            *result = res;
        return;
    }

    context->throwTypeError();
}

void __qmljs_construct_property(ExecutionContext *context, Value *result, const Value &base, String *name, Value *args, int argc)
{
    Value thisObject = base;
    if (!thisObject.isObject())
        thisObject = __qmljs_to_object(base, context);

    Value func = thisObject.objectValue()->__get__(context, name);
    if (FunctionObject *f = func.asFunctionObject()) {
        Value res = f->construct(context, args, argc);
        if (result)
            *result = res;
        return;
    }

    context->throwTypeError();
}

void __qmljs_throw(Value value, ExecutionContext *context)
{
    assert(!context->engine->unwindStack.isEmpty());

    if (context->engine->debugger)
        context->engine->debugger->aboutToThrow(&value);

    ExecutionEngine::ExceptionHandler &handler = context->engine->unwindStack.last();

    // clean up call contexts
    while (context != handler.context) {
        ExecutionContext *parent = context->parent;
        if (!context->withObject)
            context->leaveCallContext();
        context = parent;
    }

    context->engine->exception = value;

    longjmp(handler.stackFrame, 1);
}

Q_V4_EXPORT void * __qmljs_create_exception_handler(ExecutionContext *context)
{
    context->engine->exception = Value::undefinedValue();
    context->engine->unwindStack.append(ExecutionEngine::ExceptionHandler());
    ExecutionEngine::ExceptionHandler &handler = context->engine->unwindStack.last();
    handler.context = context;
    return handler.stackFrame;
}

void __qmljs_delete_exception_handler(ExecutionContext *context)
{
    assert(!context->engine->unwindStack.isEmpty());

    context->engine->unwindStack.pop_back();
}

Value __qmljs_get_exception(ExecutionContext *context)
{
    return context->engine->exception;
}

void __qmljs_builtin_typeof(ExecutionContext *ctx, Value *result, const Value &value)
{
    if (!result)
        return;
    switch (value.type()) {
    case Value::Undefined_Type:
        *result =__qmljs_string_literal_undefined(ctx);
        break;
    case Value::Null_Type:
        *result = __qmljs_string_literal_object(ctx);
        break;
    case Value::Boolean_Type:
        *result =__qmljs_string_literal_boolean(ctx);
        break;
    case Value::String_Type:
        *result = __qmljs_string_literal_string(ctx);
        break;
    case Value::Object_Type:
        if (__qmljs_is_callable(value, ctx))
            *result =__qmljs_string_literal_function(ctx);
        else
            *result = __qmljs_string_literal_object(ctx); // ### implementation-defined
        break;
    default:
        *result =__qmljs_string_literal_number(ctx);
        break;
    }
}

void __qmljs_builtin_typeof_name(ExecutionContext *context, Value *result, String *name)
{
    if (result)
        __qmljs_builtin_typeof(context, result, context->getPropertyNoThrow(name));
}

void __qmljs_builtin_typeof_member(ExecutionContext *context, Value *result, const Value &base, String *name)
{
    Value obj = base.toObject(context);
    if (result)
        __qmljs_builtin_typeof(context, result, obj.objectValue()->__get__(context, name));
}

void __qmljs_builtin_typeof_element(ExecutionContext *context, Value *result, const Value &base, const Value &index)
{
    String *name = index.toString(context);
    Value obj = base.toObject(context);
    if (result)
        __qmljs_builtin_typeof(context, result, obj.objectValue()->__get__(context, name));
}

void __qmljs_builtin_post_increment(ExecutionContext *ctx, Value *result, Value *val)
{
    if (val->isInteger() && val->integerValue() < INT_MAX) {
        if (result)
            *result = *val;
        val->int_32 += 1;
        return;
    }

    double d = __qmljs_to_number(*val, ctx);
    *val = Value::fromDouble(d + 1);
    if (result)
        *result = Value::fromDouble(d);
}

void __qmljs_builtin_post_increment_name(ExecutionContext *context, Value *result, String *name)
{
    Value v = context->getProperty(name);

    if (v.isInteger() && v.integerValue() < INT_MAX) {
        if (result)
            *result = v;
        v.int_32 += 1;
    } else {
        double d = __qmljs_to_number(v, context);
        if (result)
            *result = Value::fromDouble(d);
        v = Value::fromDouble(d + 1);
    }

    context->setProperty(name, v);
}

void __qmljs_builtin_post_increment_member(ExecutionContext *context, Value *result, const Value &base, String *name)
{
    Object *o = __qmljs_to_object(base, context).objectValue();

    Value v = o->__get__(context, name);

    if (v.isInteger() && v.integerValue() < INT_MAX) {
        if (result)
            *result = v;
        v.int_32 += 1;
    } else {
        double d = __qmljs_to_number(v, context);
        if (result)
            *result = Value::fromDouble(d);
        v = Value::fromDouble(d + 1);
    }

    o->__put__(context, name, v);
}

void __qmljs_builtin_post_increment_element(ExecutionContext *context, Value *result, const Value &base, const Value *index)
{
    Object *o = __qmljs_to_object(base, context).objectValue();

    uint idx = index->asArrayIndex();

    if (idx == UINT_MAX) {
        String *s = index->toString(context);
        return __qmljs_builtin_post_increment_member(context, result, base, s);
    }

    Value v = o->__get__(context, idx);

    if (v.isInteger() && v.integerValue() < INT_MAX) {
        if (result)
            *result = v;
        v.int_32 += 1;
    } else {
        double d = __qmljs_to_number(v, context);
        if (result)
            *result = Value::fromDouble(d);
        v = Value::fromDouble(d + 1);
    }

    o->__put__(context, idx, v);
}

void __qmljs_builtin_post_decrement(ExecutionContext *ctx, Value *result, Value *val)
{
    if (val->isInteger() && val->integerValue() > INT_MIN) {
        if (result)
            *result = *val;
        val->int_32 -= 1;
        return;
    }

    double d = __qmljs_to_number(*val, ctx);
    *val = Value::fromDouble(d - 1);
    if (result)
        *result = Value::fromDouble(d);
}

void __qmljs_builtin_post_decrement_name(ExecutionContext *context, Value *result, String *name)
{
    Value v = context->getProperty(name);

    if (v.isInteger() && v.integerValue() > INT_MIN) {
        if (result)
            *result = v;
        v.int_32 -= 1;
    } else {
        double d = __qmljs_to_number(v, context);
        if (result)
            *result = Value::fromDouble(d);
        v = Value::fromDouble(d - 1);
    }

    context->setProperty(name, v);
}

void __qmljs_builtin_post_decrement_member(ExecutionContext *context, Value *result, const Value &base, String *name)
{
    Object *o = __qmljs_to_object(base, context).objectValue();

    Value v = o->__get__(context, name);

    if (v.isInteger() && v.integerValue() > INT_MIN) {
        if (result)
            *result = v;
        v.int_32 -= 1;
    } else {
        double d = __qmljs_to_number(v, context);
        if (result)
            *result = Value::fromDouble(d);
        v = Value::fromDouble(d - 1);
    }

    o->__put__(context, name, v);
}

void __qmljs_builtin_post_decrement_element(ExecutionContext *context, Value *result, const Value &base, const Value &index)
{
    Object *o = __qmljs_to_object(base, context).objectValue();

    uint idx = index.asArrayIndex();

    if (idx == UINT_MAX) {
        String *s = index.toString(context);
        return __qmljs_builtin_post_decrement_member(context, result, base, s);
    }

    Value v = o->__get__(context, idx);

    if (v.isInteger() && v.integerValue() > INT_MIN) {
        if (result)
            *result = v;
        v.int_32 -= 1;
    } else {
        double d = __qmljs_to_number(v, context);
        if (result)
            *result = Value::fromDouble(d);
        v = Value::fromDouble(d - 1);
    }

    o->__put__(context, idx, v);
}

void __qmljs_builtin_throw(Value val, ExecutionContext *context)
{
    __qmljs_throw(val, context);
}

ExecutionContext *__qmljs_builtin_push_with_scope(Value o, ExecutionContext *ctx)
{
    Object *obj = __qmljs_to_object(o, ctx).asObject();
    return ctx->createWithScope(obj);
}

ExecutionContext *__qmljs_builtin_push_catch_scope(String *exceptionVarName, ExecutionContext *ctx)
{
    return ctx->createCatchScope(exceptionVarName);
}

ExecutionContext *__qmljs_builtin_pop_scope(ExecutionContext *ctx)
{
    return ctx->popScope();
}

void __qmljs_builtin_declare_var(ExecutionContext *ctx, bool deletable, String *name)
{
    ctx->createMutableBinding(name, deletable);
}

void __qmljs_builtin_define_property(Value object, String *name, Value val, ExecutionContext *ctx)
{
    Object *o = object.asObject();
    assert(o);

    PropertyDescriptor pd;
    pd.value = val;
    pd.type = PropertyDescriptor::Data;
    pd.writable = PropertyDescriptor::Enabled;
    pd.enumberable = PropertyDescriptor::Enabled;
    pd.configurable = PropertyDescriptor::Enabled;
    o->__defineOwnProperty__(ctx, name, &pd);
}

void __qmljs_builtin_define_array_property(Value object, int index, Value val, ExecutionContext *ctx)
{
    Object *o = object.asObject();
    assert(o);

    PropertyDescriptor pd;
    pd.value = val;
    pd.type = PropertyDescriptor::Data;
    pd.writable = PropertyDescriptor::Enabled;
    pd.enumberable = PropertyDescriptor::Enabled;
    pd.configurable = PropertyDescriptor::Enabled;
    o->__defineOwnProperty__(ctx, index, &pd);
}

void __qmljs_builtin_define_getter_setter(Value object, String *name, Value getter, Value setter, ExecutionContext *ctx)
{
    Object *o = object.asObject();
    assert(o);

    PropertyDescriptor pd;
    pd.get = getter.asFunctionObject();
    pd.set = setter.asFunctionObject();
    pd.type = PropertyDescriptor::Accessor;
    pd.writable = PropertyDescriptor::Undefined;
    pd.enumberable = PropertyDescriptor::Enabled;
    pd.configurable = PropertyDescriptor::Enabled;
    o->__defineOwnProperty__(ctx, name, &pd);
}

Value __qmljs_increment(const Value &value, ExecutionContext *ctx)
{
    TRACE1(value);

    if (value.isInteger())
        return Value::fromInt32(value.integerValue() + 1);

    double d = __qmljs_to_number(value, ctx);
    return Value::fromDouble(d + 1);
}

Value __qmljs_decrement(const Value &value, ExecutionContext *ctx)
{
    TRACE1(value);

    if (value.isInteger())
        return Value::fromInt32(value.integerValue() - 1);

    double d = __qmljs_to_number(value, ctx);
    return Value::fromDouble(d - 1);
}

} // extern "C"


} // namespace VM
} // namespace QQmlJS
