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
#ifndef QV4ISEL_MASM_P_H
#define QV4ISEL_MASM_P_H

#include "qv4ir_p.h"
#include "qmljs_objects.h"
#include "qmljs_runtime.h"

#include <QtCore/QHash>
#include <config.h>
#include <wtf/Vector.h>
#include <assembler/MacroAssembler.h>

namespace QQmlJS {
namespace MASM {

class InstructionSelection: protected IR::StmtVisitor, public JSC::MacroAssembler
{
public:
    InstructionSelection(VM::ExecutionEngine *engine, IR::Module *module, uchar *code);
    ~InstructionSelection();

    void operator()(IR::Function *function);

protected:
#if CPU(X86)

#undef VALUE_FITS_IN_REGISTER

    static const RegisterID StackFrameRegister = JSC::X86Registers::ebp;
    static const RegisterID StackPointerRegister = JSC::X86Registers::esp;
    static const RegisterID ContextRegister = JSC::X86Registers::esi;
    static const RegisterID ReturnValueRegister = JSC::X86Registers::eax;
    static const RegisterID ScratchRegister = JSC::X86Registers::ecx;
    static const RegisterID CalleeSavedFirstRegister = ScratchRegister;
    static const RegisterID CalleeSavedLastRegister = ScratchRegister;
    static const FPRegisterID FPGpr0 = JSC::X86Registers::xmm0;

    static const int RegisterSize = 4;

    static const int RegisterArgumentCount = 0;
    static RegisterID registerForArgument(int)
    {
        assert(false);
        // Not reached.
        return JSC::X86Registers::eax;
    }
#elif CPU(X86_64)

#define VALUE_FITS_IN_REGISTER

    static const RegisterID StackFrameRegister = JSC::X86Registers::ebp;
    static const RegisterID StackPointerRegister = JSC::X86Registers::esp;
    static const RegisterID ContextRegister = JSC::X86Registers::r14;
    static const RegisterID ReturnValueRegister = JSC::X86Registers::eax;
    static const RegisterID ScratchRegister = JSC::X86Registers::r10;
    static const FPRegisterID FPGpr0 = JSC::X86Registers::xmm0;

    static const int RegisterSize = 8;

    static const int RegisterArgumentCount = 6;
    static RegisterID registerForArgument(int index)
    {
        static RegisterID regs[RegisterArgumentCount] = {
            JSC::X86Registers::edi,
            JSC::X86Registers::esi,
            JSC::X86Registers::edx,
            JSC::X86Registers::ecx,
            JSC::X86Registers::r8,
            JSC::X86Registers::r9
        };
        assert(index >= 0 && index < RegisterArgumentCount);
        return regs[index];
    };
#elif CPU(ARM)

#undef VALUE_FITS_IN_REGISTER

    static const RegisterID StackFrameRegister = JSC::ARMRegisters::r4;
    static const RegisterID StackPointerRegister = JSC::ARMRegisters::sp;
    static const RegisterID ContextRegister = JSC::ARMRegisters::r5;
    static const RegisterID ReturnValueRegister = JSC::ARMRegisters::r0;
    static const RegisterID ScratchRegister = JSC::ARMRegisters::r6;
    static const RegisterID CalleeSavedFirstRegister = JSC::ARMRegisters::r4;
    static const RegisterID CalleeSavedLastRegister = JSC::ARMRegisters::r11;
    static const FPRegisterID FPGpr0 = JSC::ARMRegisters::d0;

    static const int RegisterSize = 4;

    static const RegisterID RegisterArgument1 = JSC::ARMRegisters::r0;
    static const RegisterID RegisterArgument2 = JSC::ARMRegisters::r1;
    static const RegisterID RegisterArgument3 = JSC::ARMRegisters::r2;
    static const RegisterID RegisterArgument4 = JSC::ARMRegisters::r3;

    static const int RegisterArgumentCount = 4;
    static RegisterID registerForArgument(int index)
    {
        assert(index >= 0 && index < RegisterArgumentCount);
        return static_cast<RegisterID>(JSC::ARMRegisters::r0 + index);
    };
#else
#error Argh.
#endif

    struct VoidType {};
    static const VoidType Void;

    // Explicit type to allow distinguishing between
    // pushing an address itself or the value it points
    // to onto the stack when calling functions.
    struct Pointer : public Address
    {
        explicit Pointer(const Address& addr)
            : Address(addr)
        {}
        explicit Pointer(RegisterID reg, int32_t offset)
            : Address(reg, offset)
        {}
    };

    void enterStandardStackFrame(int locals)
    {
#if CPU(ARM)
        push(JSC::ARMRegisters::lr);
#endif
        push(StackFrameRegister);
        move(StackPointerRegister, StackFrameRegister);
        subPtr(TrustedImm32(locals*sizeof(QQmlJS::VM::Value)), StackPointerRegister);
#if CPU(X86) || CPU(ARM)
        for (int saveReg = CalleeSavedFirstRegister; saveReg <= CalleeSavedLastRegister; ++saveReg)
            push(static_cast<RegisterID>(saveReg));
#endif
    }
    void leaveStandardStackFrame(int locals)
    {
#if CPU(X86) || CPU(ARM)
        for (int saveReg = CalleeSavedLastRegister; saveReg >= CalleeSavedFirstRegister; --saveReg)
            pop(static_cast<RegisterID>(saveReg));
#endif
        addPtr(TrustedImm32(locals*sizeof(QQmlJS::VM::Value)), StackPointerRegister);
        pop(StackFrameRegister);
#if CPU(ARM)
        pop(JSC::ARMRegisters::lr);
#endif
    }

    Address addressForArgument(int index) const
    {
        if (index < RegisterArgumentCount)
            return Address(registerForArgument(index), 0);

        // StackFrameRegister points to its old value on the stack, and above
        // it we have the return address, hence the need to step over two
        // values before reaching the first argument.
        return Address(StackFrameRegister, (index - RegisterArgumentCount + 2) * sizeof(void*));
    }

    // Some run-time functions take (Value* args, int argc). This function is for populating
    // the args.
    Pointer argumentAddressForCall(int argument)
    {
        const int index = _function->maxNumberOfArguments - argument;
        return Pointer(StackFrameRegister, sizeof(VM::Value) * (-index)
                                          - sizeof(void*) // size of ebp
                       );
    }
    Pointer baseAddressForCallArguments()
    {
        return argumentAddressForCall(0);
    }

    VM::String *identifier(const QString &s);
    Pointer loadTempAddress(RegisterID reg, IR::Temp *t);
    void callActivationProperty(IR::Call *call, IR::Temp *result);
    void callProperty(IR::Call *call, IR::Temp *result);
    void constructActivationProperty(IR::New *call, IR::Temp *result);
    void constructProperty(IR::New *ctor, IR::Temp *result);
    void callValue(IR::Call *call, IR::Temp *result);
    void constructValue(IR::New *call, IR::Temp *result);
    void checkExceptions();

    virtual void visitExp(IR::Exp *);
    virtual void visitEnter(IR::Enter *);
    virtual void visitLeave(IR::Leave *);
    virtual void visitMove(IR::Move *);
    virtual void visitJump(IR::Jump *);
    virtual void visitCJump(IR::CJump *);
    virtual void visitRet(IR::Ret *);

private:
    void jumpToBlock(IR::BasicBlock *target);

    typedef JSC::FunctionPtr FunctionPtr;

    void callAbsolute(const char* functionName, FunctionPtr function) {
        CallToLink ctl;
        ctl.call = call();
        ctl.externalFunction = function;
        ctl.functionName = functionName;
        _callsToLink.append(ctl);
    }

    void loadArgument(RegisterID source, RegisterID dest)
    {
        move(source, dest);
    }

    void loadArgument(TrustedImmPtr ptr, RegisterID dest)
    {
        move(TrustedImmPtr(ptr), dest);
    }

    void loadArgument(const Pointer& ptr, RegisterID dest)
    {
        addPtr(TrustedImm32(ptr.offset), ptr.base, dest);
    }

#ifdef VALUE_FITS_IN_REGISTER
    void loadArgument(IR::Temp* temp, RegisterID dest)
    {
        if (!temp) {
            VM::Value undefined = VM::Value::undefinedValue();
            move(TrustedImm64(undefined.val), dest);
        } else {
            Pointer addr = loadTempAddress(dest, temp);
            load64(addr, dest);
        }
    }
#endif

    void loadArgument(VM::String* string, RegisterID dest)
    {
        loadArgument(TrustedImmPtr(string), dest);
    }

    void loadArgument(TrustedImm32 imm32, RegisterID dest)
    {
        xorPtr(dest, dest);
        if (imm32.m_value)
            move(imm32, dest);
    }

    void storeArgument(RegisterID src, IR::Temp *temp)
    {
        if (temp) {
            Pointer addr = loadTempAddress(ScratchRegister, temp);
#ifdef VALUE_FITS_IN_REGISTER
            store64(src, addr);
#else
            // If the value doesn't fit into a register, then the
            // register contains the address to where the argument
            // (return value) is stored. Copy it from there.
            copyValue(addr, Pointer(src, 0));
#endif
        }
    }

#ifdef VALUE_FITS_IN_REGISTER
    void storeArgument(RegisterID src, const Pointer &dest)
    {
        store64(src, dest);
    }
#endif

    void storeArgument(RegisterID src, RegisterID dest)
    {
        move(src, dest);
    }

    void storeArgument(RegisterID, VoidType)
    {
    }

    using JSC::MacroAssembler::push;

    void push(const Pointer& ptr)
    {
        addPtr(TrustedImm32(ptr.offset), ptr.base, ScratchRegister);
        push(ScratchRegister);
    }

    void push(VM::Value value)
    {
#ifdef VALUE_FITS_IN_REGISTER
        move(TrustedImm64(value.val), ScratchRegister);
        push(ScratchRegister);
#else
        move(TrustedImm32(value.int_32), ScratchRegister);
        push(ScratchRegister);
        move(TrustedImm32(value.tag), ScratchRegister);
        push(ScratchRegister);
#endif
    }

    void push(IR::Temp* temp)
    {
        if (temp) {
            Address addr = loadTempAddress(ScratchRegister, temp);
            addr.offset += 4;
            push(addr);
            addr.offset -= 4;
            push(addr);
        } else {
            VM::Value undefined = VM::Value::undefinedValue();
            push(undefined);
        }
    }

    void push(TrustedImmPtr ptr)
    {
        move(TrustedImmPtr(ptr), ScratchRegister);
        push(ScratchRegister);
    }

    void push(VM::String* name)
    {
        push(TrustedImmPtr(name));
    }

    void callFunctionPrologue()
    {
#if CPU(X86)
        // Callee might clobber it :(
        push(ContextRegister);
#endif
    }
    void callFunctionEpilogue()
    {
#if CPU(X86)
        pop(ContextRegister);
#endif
    }

    #define isel_stringIfyx(s) #s
    #define isel_stringIfy(s) isel_stringIfyx(s)

    #define generateFunctionCall(t, function, ...) \
        generateFunctionCallImp(t, isel_stringIfy(function), function, __VA_ARGS__)

    static inline int sizeOfArgument(VoidType)
    { return 0; }
    static inline int sizeOfArgument(RegisterID)
    { return RegisterSize; }
    static inline int sizeOfArgument(IR::Temp*)
    { return 8; } // Size of value
    static inline int sizeOfArgument(const Pointer&)
    { return sizeof(void*); }
    static inline int sizeOfArgument(VM::String* string)
    { return sizeof(string); }
    static inline int sizeOfArgument(TrustedImmPtr)
    { return sizeof(void*); }
    static inline int sizeOfArgument(TrustedImm32)
    { return 4; }

    struct ArgumentLoader
    {
        ArgumentLoader(InstructionSelection* instructionSelection, int totalNumberOfArguments)
            : isel(instructionSelection)
            , stackSpaceForArguments(0)
            , currentRegisterIndex(qMin(totalNumberOfArguments - 1, RegisterArgumentCount - 1))
        {
        }

        template <typename T>
        void load(T argument)
        {
            if (currentRegisterIndex >= 0) {
                isel->loadArgument(argument, registerForArgument(currentRegisterIndex));
                --currentRegisterIndex;
            } else {
                isel->push(argument);
                stackSpaceForArguments += sizeOfArgument(argument);
            }
        }

        void load(VoidType)
        {
            if (currentRegisterIndex >= 0)
                --currentRegisterIndex;
        }

        InstructionSelection *isel;
        int stackSpaceForArguments;
        int currentRegisterIndex;
    };

    template <typename ArgRet, typename Arg1, typename Arg2, typename Arg3, typename Arg4, typename Arg5>
    void generateFunctionCallImp(ArgRet r, const char* functionName, FunctionPtr function, Arg1 arg1, Arg2 arg2, Arg3 arg3, Arg4 arg4, Arg5 arg5)
    {
        callFunctionPrologue();

        int totalNumberOfArgs = 5;

        // If necessary reserve space for the return value on the stack and
        // pass the pointer to it as the first hidden parameter.
        bool returnValueOnStack = false;
        int sizeOfReturnValueOnStack = sizeOfArgument(r);
        if (sizeOfReturnValueOnStack > RegisterSize) {
            sub32(TrustedImm32(sizeOfReturnValueOnStack), StackPointerRegister);
            ++totalNumberOfArgs;
            returnValueOnStack = true;
        }

        ArgumentLoader l(this, totalNumberOfArgs);
        l.load(arg5);
        l.load(arg4);
        l.load(arg3);
        l.load(arg2);
        l.load(arg1);

        if (returnValueOnStack) {
            // Load address of return value
            l.load(Pointer(StackPointerRegister, l.stackSpaceForArguments));
        }

        callAbsolute(functionName, function);

        int stackSizeToCorrect = l.stackSpaceForArguments;
        if (returnValueOnStack) {
            stackSizeToCorrect -= sizeof(void*); // Callee removed the hidden argument (address of return value)
            stackSizeToCorrect += sizeOfReturnValueOnStack;
        }

        storeArgument(ReturnValueRegister, r);

        if (stackSizeToCorrect)
            add32(TrustedImm32(stackSizeToCorrect), StackPointerRegister);

        callFunctionEpilogue();
    }

    template <typename ArgRet, typename Arg1, typename Arg2, typename Arg3, typename Arg4>
    void generateFunctionCallImp(ArgRet r, const char* functionName, FunctionPtr function, Arg1 arg1, Arg2 arg2, Arg3 arg3, Arg4 arg4)
    {
        generateFunctionCallImp(r, functionName, function, arg1, arg2, arg3, arg4, VoidType());
    }

    template <typename ArgRet, typename Arg1, typename Arg2, typename Arg3>
    void generateFunctionCallImp(ArgRet r, const char* functionName, FunctionPtr function, Arg1 arg1, Arg2 arg2, Arg3 arg3)
    {
        generateFunctionCallImp(r, functionName, function, arg1, arg2, arg3, VoidType(), VoidType());
    }

    template <typename ArgRet, typename Arg1, typename Arg2>
    void generateFunctionCallImp(ArgRet r, const char* functionName, FunctionPtr function, Arg1 arg1, Arg2 arg2)
    {
        generateFunctionCallImp(r, functionName, function, arg1, arg2, VoidType(), VoidType(), VoidType());
    }

    template <typename ArgRet, typename Arg1>
    void generateFunctionCallImp(ArgRet r, const char* functionName, FunctionPtr function, Arg1 arg1)
    {
        generateFunctionCallImp(r, functionName, function, arg1, VoidType(), VoidType(), VoidType(), VoidType());
    }

    int prepareVariableArguments(IR::ExprList* args);

    typedef VM::Value (*ActivationMethod)(VM::Context *, VM::String *name, VM::Value *args, int argc);
    typedef VM::Value (*BuiltinMethod)(VM::Context *, VM::Value *args, int argc);
    void callRuntimeMethodImp(IR::Temp *result, const char* name, ActivationMethod method, IR::Expr *base, IR::ExprList *args);
    void callRuntimeMethodImp(IR::Temp *result, const char* name, BuiltinMethod method, IR::ExprList *args);
#define callRuntimeMethod(result, function, ...) \
    callRuntimeMethodImp(result, isel_stringIfy(function), function, __VA_ARGS__)

    using JSC::MacroAssembler::loadDouble;
    void loadDouble(IR::Temp* temp, FPRegisterID dest)
    {
        Pointer ptr = loadTempAddress(ScratchRegister, temp);
        loadDouble(ptr, dest);
    }

    using JSC::MacroAssembler::storeDouble;
    void storeDouble(FPRegisterID source, IR::Temp* temp)
    {
        Pointer ptr = loadTempAddress(ScratchRegister, temp);
        storeDouble(source, ptr);
    }

    template <typename Result, typename Source>
    void copyValue(Result result, Source source);

    struct CallToLink {
        Call call;
        FunctionPtr externalFunction;
        const char* functionName;
    };

    void storeValue(VM::Value value, Address destination)
    {
#ifdef VALUE_FITS_IN_REGISTER
        store64(TrustedImm64(value.val), destination);
#else
        store32(TrustedImm32(value.int_32), destination);
        destination.offset += 4;
        store32(TrustedImm32(value.tag), destination);
#endif
    }

    VM::ExecutionEngine *_engine;
    IR::Module *_module;
    IR::Function *_function;
    IR::BasicBlock *_block;
    uchar *_buffer;
    uchar *_code;
    uchar *_codePtr;
    QHash<IR::BasicBlock *, QVector<Jump> > _patches;
    QHash<IR::BasicBlock *, Label> _addrs;
    QList<CallToLink> _callsToLink;
};

} // end of namespace MASM
} // end of namespace QQmlJS

#endif // QV4ISEL_MASM_P_H
