/*
* The MIT License (MIT)
*
* Copyright (c) Microsoft Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*
*
* Portions lifted from CPython under the PSF license.
*/
#include "intrins.h"
#include "pyjit.h"

#ifdef _MSC_VER

#include <safeint.h>
using namespace msl::utilities;

#endif

PyObject* g_emptyTuple;

#include <dictobject.h>
#include <vector>

#define NAME_ERROR_MSG \
    "name '%.200s' is not defined"

#define UNBOUNDLOCAL_ERROR_MSG \
    "local variable '%.200s' referenced before assignment"
#define UNBOUNDFREE_ERROR_MSG \
    "free variable '%.200s' referenced before assignment" \
    " in enclosing scope"

FILE * g_traceLog = stdout;

template<typename T>
void decref(T v) {
    Py_DECREF(v);
}

template<typename T, typename... Args>
void decref(T v, Args... args) {
    Py_DECREF(v) ; decref(args...);
}

static void
format_exc_check_arg(PyObject *exc, const char *format_str, PyObject *obj) {
    const char *obj_str;

    if (!obj)
        return;

    obj_str = _PyUnicode_AsString(obj);
    if (!obj_str)
        return;

    PyErr_Format(exc, format_str, obj_str);
}

static void
format_exc_unbound(PyCodeObject *co, int oparg) {
    PyObject *name;
    /* Don't stomp existing exception */
    if (PyErr_Occurred())
        return;
    if (oparg < PyTuple_GET_SIZE(co->co_cellvars)) {
        name = PyTuple_GET_ITEM(co->co_cellvars,
            oparg);
        format_exc_check_arg(
            PyExc_UnboundLocalError,
            UNBOUNDLOCAL_ERROR_MSG,
            name);
    }
    else {
        name = PyTuple_GET_ITEM(co->co_freevars, oparg -
            PyTuple_GET_SIZE(co->co_cellvars));
        format_exc_check_arg(PyExc_NameError,
            UNBOUNDFREE_ERROR_MSG, name);
    }
}

PyObject* PyJit_Add(PyObject *left, PyObject *right) {
    PyObject *sum;
    if (PyUnicode_CheckExact(left) && PyUnicode_CheckExact(right)) {
        PyUnicode_Append(&left, right);
        sum = left;
    }
    else {
        sum = PyNumber_Add(left, right);
        Py_DECREF(left);
    }
    Py_DECREF(right);
    return sum;
}

PyObject* PyJit_Subscr(PyObject *left, PyObject *right) {
    auto res = PyObject_GetItem(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_SubscrIndex(PyObject *o, PyObject *key, Py_ssize_t index)
{
    PyMappingMethods *m;
    PySequenceMethods *ms;
    PyObject* res;

    if (o == nullptr || key == nullptr) {
        PyErr_SetString(PyExc_ValueError,
                        "Internal call, PyJit_SubscrIndex with key or container null");
        return nullptr;
    }

    ms = Py_TYPE(o)->tp_as_sequence;
    if (ms && ms->sq_item) {
        res = PySequence_GetItem(o, index);
    } else {
        res = PyObject_GetItem(o, key);
    }
    Py_DECREF(o);
    Py_DECREF(key);

    return res;
}

PyObject* PyJit_SubscrIndexHash(PyObject *o, PyObject *key, Py_ssize_t index, Py_hash_t hash)
{
    if (PyDict_CheckExact(o))
        return PyJit_SubscrDictHash(o, key, hash);
    else
        return PyJit_SubscrIndex(o, key, index);
}

PyObject* PyJit_SubscrDict(PyObject *o, PyObject *key){
    if (!PyDict_CheckExact(o))
        return PyJit_Subscr(o, key);

    PyObject* res = PyDict_GetItem(o, key);
    Py_XINCREF(res);
    Py_DECREF(o);
    Py_DECREF(key);
    return res;
}

PyObject* PyJit_SubscrDictHash(PyObject *o, PyObject *key, Py_hash_t hash){
    if (!PyDict_CheckExact(o))
        return PyJit_Subscr(o, key);
    PyObject* value = _PyDict_GetItem_KnownHash(o, key, hash);
    Py_XINCREF(value);
    Py_DECREF(o);
    Py_DECREF(key);
    return value;
}

PyObject* PyJit_SubscrList(PyObject *o, PyObject *key){
    if (!PyList_CheckExact(o))
        return PyJit_Subscr(o, key);
    PyObject* res;

    if (PyIndex_Check(key)) {
        Py_ssize_t key_value;
        key_value = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (key_value == -1 && PyErr_Occurred()) {
            res = nullptr;
        } else if (key_value < 0){
            // Supports negative indexes without converting back to PyLong..
            res = PySequence_GetItem(o, key_value);
        } else {
            res = PyList_GetItem(o, key_value);
            Py_XINCREF(res);
        }
    }
    else {
        return PyJit_Subscr(o, key);
    }
    Py_DECREF(o);
    Py_DECREF(key);
    return res;
}

PyObject* PyJit_SubscrListIndex(PyObject *o, PyObject *key, Py_ssize_t index){
    if (!PyList_CheckExact(o))
        return PyJit_Subscr(o, key);
    PyObject* res = PyList_GetItem(o, index);
    Py_XINCREF(res);
    Py_DECREF(o);
    Py_DECREF(key);
    return res;
}

PyObject* PyJit_SubscrTuple(PyObject *o, PyObject *key){
    if (!PyTuple_CheckExact(o))
        return PyJit_Subscr(o, key);
    PyObject* res;

    if (PyIndex_Check(key)) {
        Py_ssize_t key_value;
        key_value = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (key_value == -1 && PyErr_Occurred()) {
            res = nullptr;
        } else if (key_value < 0){
            // Supports negative indexes without converting back to PyLong..
            res = PySequence_GetItem(o, key_value);
        } else {
            res = PyTuple_GetItem(o, key_value);
            Py_XINCREF(res);
        }
    }
    else {
        return PyJit_Subscr(o, key);
    }
    error:
    Py_DECREF(key);
    Py_DECREF(o);
    return res;
}

PyObject* PyJit_SubscrTupleIndex(PyObject *o, PyObject *key, Py_ssize_t index){
    if (!PyTuple_CheckExact(o))
        return PyJit_Subscr(o, key);
    PyObject* res = PyTuple_GetItem(o, index);
    Py_XINCREF(res);
    Py_DECREF(o);
    Py_DECREF(key);
    return res;
}

PyObject* PyJit_RichCompare(PyObject *left, PyObject *right, size_t op) {
    auto res = PyObject_RichCompare(left, right, op);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_Contains(PyObject *left, PyObject *right) {
    auto res = PySequence_Contains(right, left);
    Py_DECREF(left);
    Py_DECREF(right);
    if (res < 0) {
        return nullptr;
    }
    auto ret = res ? Py_True : Py_False;
    Py_INCREF(ret);
    return ret;
}

PyObject* PyJit_NotContains(PyObject *left, PyObject *right) {
    auto res = PySequence_Contains(right, left);
    Py_DECREF(left);
    Py_DECREF(right);
    if (res < 0) {
        return nullptr;
    }
    auto ret = res ? Py_False : Py_True;
    Py_INCREF(ret);
    return ret;
}

PyObject* PyJit_NewFunction(PyObject* code, PyObject* qualname, PyFrameObject* frame) {
    auto res = PyFunction_NewWithQualName(code, frame->f_globals, qualname);
    Py_DECREF(code);
    Py_DECREF(qualname);
    return res;
}

PyObject* PyJit_LoadClosure(PyFrameObject* frame, size_t index) {
    PyObject** cells = frame->f_localsplus + frame->f_code->co_nlocals;
    PyObject *value = cells[index];

    if (value == nullptr) {
        format_exc_unbound(frame->f_code, (int)index);
    }
    else {
        Py_INCREF(value);
    }
    return value;
}

PyObject* PyJit_SetClosure(PyObject* closure, PyObject* func) {
    PyFunction_SetClosure(func, closure);
    Py_DECREF(closure);
    return func;
}

PyObject* PyJit_BuildSlice(PyObject* start, PyObject* stop, PyObject* step) {
    auto slice = PySlice_New(start, stop, step);
    Py_DECREF(start);
    Py_DECREF(stop);
    Py_XDECREF(step);
    return slice;
}

PyObject* PyJit_UnaryPositive(PyObject* value) {
    assert(value != nullptr);
    auto res = PyNumber_Positive(value);
    Py_DECREF(value);
    return res;
}

PyObject* PyJit_UnaryNegative(PyObject* value) {
    assert(value != nullptr);
    auto res = PyNumber_Negative(value);
    Py_DECREF(value);
    return res;
}

PyObject* PyJit_UnaryNot(PyObject* value) {
    assert(value != nullptr);
    int err = PyObject_IsTrue(value);
    Py_DECREF(value);
    if (err == 0) {
        Py_INCREF(Py_True);
        return Py_True;
    }
    else if (err > 0) {
        Py_INCREF(Py_False);
        return Py_False;
    }
    return nullptr;
}

int PyJit_UnaryNot_Int(PyObject* value) {
    assert(value != nullptr);
    int err = PyObject_IsTrue(value);
    Py_DECREF(value);
    if (err < 0) {
        return -1;
    }
    return err ? 0 : 1;
}

PyObject* PyJit_UnaryInvert(PyObject* value) {
    assert(value != nullptr);
    auto res = PyNumber_Invert(value);
    Py_DECREF(value);
    return res;
}

PyObject* PyJit_NewList(size_t size){
    auto list = PyList_New(size);
    return list;
}

PyObject* PyJit_ListAppend(PyObject* list, PyObject* value) {
    assert(list != nullptr);
    assert(PyList_CheckExact(list));
    int err = PyList_Append(list, value);
    Py_DECREF(value);
    if (err) {
        return nullptr;
    }
    return list;
}

PyObject* PyJit_SetAdd(PyObject* set, PyObject* value) {
    assert(set != nullptr);
    int err ;
    err = PySet_Add(set, value);
    Py_DECREF(value);
    if (err != 0) {
        goto error;
    }
    return set;
error:
    return nullptr;
}

PyObject* PyJit_UpdateSet(PyObject* iterable, PyObject* set) {
    assert(set != nullptr);
    int res = _PySet_Update(set, iterable);
    Py_DECREF(iterable);
    if (res < 0)
        goto error;
    return set;
error:
    return nullptr;
}

PyObject* PyJit_MapAdd(PyObject*map, PyObject*key, PyObject* value) {
    assert(map != nullptr);
    if (!PyDict_Check(map)) {
        PyErr_SetString(PyExc_TypeError,
                        "invalid argument type to MapAdd");
        Py_DECREF(map);
        return nullptr;
    }
    int err = PyDict_SetItem(map, key, value);  /* v[w] = u */
    Py_DECREF(value);
    Py_DECREF(key);
    if (err) {
        return nullptr;
    }
    return map;
}

PyObject* PyJit_Multiply(PyObject *left, PyObject *right) {
    auto res = PyNumber_Multiply(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_TrueDivide(PyObject *left, PyObject *right) {
    auto res = PyNumber_TrueDivide(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_FloorDivide(PyObject *left, PyObject *right) {
    auto res = PyNumber_FloorDivide(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_Power(PyObject *left, PyObject *right) {
    auto res = PyNumber_Power(left, right, Py_None);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_Modulo(PyObject *left, PyObject *right) {
    auto res = (PyUnicode_CheckExact(left) && (
		!PyUnicode_Check(right) || PyUnicode_CheckExact(right))) ?
        PyUnicode_Format(left, right) :
        PyNumber_Remainder(left, right);

    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_Subtract(PyObject *left, PyObject *right) {
    auto res = PyNumber_Subtract(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_MatrixMultiply(PyObject *left, PyObject *right) {
    auto res = PyNumber_MatrixMultiply(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_BinaryLShift(PyObject *left, PyObject *right) {
    auto res = PyNumber_Lshift(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_BinaryRShift(PyObject *left, PyObject *right) {
    auto res = PyNumber_Rshift(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_BinaryAnd(PyObject *left, PyObject *right) {
    auto res = PyNumber_And(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_BinaryXor(PyObject *left, PyObject *right) {
    auto res = PyNumber_Xor(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_BinaryOr(PyObject *left, PyObject *right) {
    auto res = PyNumber_Or(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_InplacePower(PyObject *left, PyObject *right) {
    auto res = PyNumber_InPlacePower(left, right, Py_None);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_InplaceMultiply(PyObject *left, PyObject *right) {
    auto res = PyNumber_InPlaceMultiply(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_InplaceMatrixMultiply(PyObject *left, PyObject *right) {
    auto res = PyNumber_InPlaceMatrixMultiply(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_InplaceTrueDivide(PyObject *left, PyObject *right) {
    auto res = PyNumber_InPlaceTrueDivide(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_InplaceFloorDivide(PyObject *left, PyObject *right) {
    auto res = PyNumber_InPlaceFloorDivide(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_InplaceModulo(PyObject *left, PyObject *right) {
    auto res = PyNumber_InPlaceRemainder(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_InplaceAdd(PyObject *left, PyObject *right) {
    PyObject* res;
    if (PyUnicode_CheckExact(left) && PyUnicode_CheckExact(right)) {
        PyUnicode_Append(&left, right);
        res = left;
    }
    else {
        res = PyNumber_InPlaceAdd(left, right);
        Py_DECREF(left);
    }
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_InplaceSubtract(PyObject *left, PyObject *right) {
    auto res = PyNumber_InPlaceSubtract(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_InplaceLShift(PyObject *left, PyObject *right) {
    auto res = PyNumber_InPlaceLshift(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_InplaceRShift(PyObject *left, PyObject *right) {
    auto res = PyNumber_InPlaceRshift(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_InplaceAnd(PyObject *left, PyObject *right) {
    auto res = PyNumber_InPlaceAnd(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_InplaceXor(PyObject *left, PyObject *right) {
    auto res = PyNumber_InPlaceXor(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

PyObject* PyJit_InplaceOr(PyObject *left, PyObject *right) {
    auto res = PyNumber_InPlaceOr(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    return res;
}

int PyJit_PrintExpr(PyObject *value) {
    _Py_IDENTIFIER(displayhook);
    PyObject *hook = _PySys_GetObjectId(&PyId_displayhook);
    PyObject *res;
    if (hook == nullptr) {
        PyErr_SetString(PyExc_RuntimeError,
            "lost sys.displayhook");
        Py_DECREF(value);
        return 1;
    }
    res = PyObject_CallOneArg(hook, value);
    Py_DECREF(value);
    if (res == nullptr) {
        return 1;
    }
    Py_DECREF(res);
    return 0;
}

void PyJit_PrepareException(PyObject** exc, PyObject**val, PyObject** tb, PyObject** oldexc, PyObject**oldVal, PyObject** oldTb) {
    auto tstate = PyThreadState_GET();

    // we take ownership of these into locals...
    if (tstate->curexc_type != nullptr) {
        *oldexc = tstate->curexc_type;
    }
    else {
        *oldexc = Py_None;
        Py_INCREF(Py_None);
    }
    *oldVal = tstate->curexc_value;
    *oldTb = tstate->curexc_traceback;

    PyErr_Fetch(exc, val, tb);
    /* Make the raw exception data
    available to the handler,
    so a program can emulate the
    Python main loop. */
    PyErr_NormalizeException(
        exc, val, tb);
    if (tb != nullptr)
        PyException_SetTraceback(*val, *tb);
    else
        PyException_SetTraceback(*val, Py_None);
    Py_INCREF(*exc);
    tstate->curexc_type = *exc;
    Py_INCREF(*val);
    tstate->curexc_value = *val;
    assert(PyExceptionInstance_Check(*val));
    tstate->curexc_traceback = *tb;
    if (*tb == nullptr)
        *tb = Py_None;
    Py_INCREF(*tb);
}

void PyJit_UnwindEh(PyObject*exc, PyObject*val, PyObject*tb) {
    auto tstate = PyThreadState_GET();
    assert(val == nullptr || PyExceptionInstance_Check(val));
    auto oldtb = tstate->curexc_traceback;
    auto oldtype = tstate->curexc_type;
    auto oldvalue = tstate->curexc_value;
    tstate->curexc_traceback = tb;
    tstate->curexc_type = exc;
    tstate->curexc_value = val;
    Py_XDECREF(oldtb);
    Py_XDECREF(oldtype);
    Py_XDECREF(oldvalue);
}

#define CANNOT_CATCH_MSG "catching classes that do not inherit from "\
                         "BaseException is not allowed"

PyObject* PyJit_CompareExceptions(PyObject*v, PyObject* w) {
    if (PyTuple_Check(w)) {
        Py_ssize_t i, length;
        length = PyTuple_Size(w);
        for (i = 0; i < length; i += 1) {
            PyObject *exc = PyTuple_GET_ITEM(w, i);
            if (!PyExceptionClass_Check(exc)) {
                PyErr_SetString(PyExc_TypeError,
                    CANNOT_CATCH_MSG);
                Py_DECREF(v);
                Py_DECREF(w);
                return nullptr;
            }
        }
    }
    else {
        if (!PyExceptionClass_Check(w)) {
            PyErr_SetString(PyExc_TypeError,
                CANNOT_CATCH_MSG);
            Py_DECREF(v);
            Py_DECREF(w);
            return nullptr;
        }
    }
    int res = PyErr_GivenExceptionMatches(v, w);
    Py_DECREF(v);
    Py_DECREF(w);
    v = res ? Py_True : Py_False;
    Py_INCREF(v);
    return v;
}

void PyJit_UnboundLocal(PyObject* name) {
    format_exc_check_arg(
        PyExc_UnboundLocalError,
        UNBOUNDLOCAL_ERROR_MSG,
        name
        );
}

void PyJit_DebugTrace(char* msg) {
    puts(msg);
}

void PyJit_PyErrRestore(PyObject*tb, PyObject*value, PyObject*exception) {
    if (exception == Py_None) {
        exception = nullptr;
    }
    PyErr_Restore(exception, value, tb);
}

PyObject* PyJit_ImportName(PyObject*level, PyObject*from, PyObject* name, PyFrameObject* f) {
    _Py_IDENTIFIER(__import__);
    PyThreadState *tstate = PyThreadState_GET();
    PyObject *imp_func = _PyDict_GetItemId(f->f_builtins, &PyId___import__);
    PyObject *args, *res;
    PyObject* stack[5];

    if (imp_func == nullptr) {
        PyErr_SetString(PyExc_ImportError,
            "__import__ not found");
        return nullptr;
    }

    Py_INCREF(imp_func);

    stack[0] = name;
    stack[1] = f->f_globals;
    stack[2] = f->f_locals == nullptr ? Py_None : f->f_locals;
    stack[3] = from;
    stack[4] = level;
#ifdef GIL
    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();
#endif
    res = _PyObject_FastCall(imp_func, stack, 5);
#ifdef GIL
    PyGILState_Release(gstate);
#endif
    Py_DECREF(imp_func);
    return res;
}

PyObject* PyJit_ImportFrom(PyObject*v, PyObject* name) {
    PyThreadState *tstate = PyThreadState_GET();
    _Py_IDENTIFIER(__name__);
    PyObject *x;
    PyObject *fullmodname, *pkgname, *pkgpath, *pkgname_or_unknown, *errmsg;

    if (_PyObject_LookupAttr(v, name, &x) != 0) {
        return x;
    }
    /* Issue #17636: in case this failed because of a circular relative
       import, try to fallback on reading the module directly from
       sys.modules. */
    pkgname = _PyObject_GetAttrId(v, &PyId___name__);
    if (pkgname == nullptr) {
        goto error;
    }
    if (!PyUnicode_Check(pkgname)) {
        Py_CLEAR(pkgname);
        goto error;
    }
    fullmodname = PyUnicode_FromFormat("%U.%U", pkgname, name);
    if (fullmodname == nullptr) {
        Py_DECREF(pkgname);
        return nullptr;
    }
    x = PyImport_GetModule(fullmodname);
    Py_DECREF(fullmodname);
    if (x == nullptr && !PyErr_Occurred()) {
        goto error;
    }
    Py_DECREF(pkgname);
    return x;
error:
    pkgpath = PyModule_GetFilenameObject(v);
    if (pkgname == nullptr) {
        pkgname_or_unknown = PyUnicode_FromString("<unknown module name>");
        if (pkgname_or_unknown == nullptr) {
            Py_XDECREF(pkgpath);
            return nullptr;
        }
    } else {
        pkgname_or_unknown = pkgname;
    }

    if (pkgpath == nullptr || !PyUnicode_Check(pkgpath)) {
        PyErr_Clear();
        errmsg = PyUnicode_FromFormat(
                "cannot import name %R from %R (unknown location)",
                name, pkgname_or_unknown
        );
        /* NULL checks for errmsg and pkgname done by PyErr_SetImportError. */
        PyErr_SetImportError(errmsg, pkgname, nullptr);
    }
    else {
        _Py_IDENTIFIER(__spec__);
        PyObject *spec = _PyObject_GetAttrId(v, &PyId___spec__);
        const char *fmt =
                _PyModuleSpec_IsInitializing(spec) ?
                "cannot import name %R from partially initialized module %R "
                "(most likely due to a circular import) (%S)" :
                "cannot import name %R from %R (%S)";
        Py_XDECREF(spec);

        errmsg = PyUnicode_FromFormat(fmt, name, pkgname_or_unknown, pkgpath);
        /* NULL checks for errmsg and pkgname done by PyErr_SetImportError. */
        PyErr_SetImportError(errmsg, pkgname, pkgpath);
    }

    Py_XDECREF(errmsg);
    Py_XDECREF(pkgname_or_unknown);
    Py_XDECREF(pkgpath);
    return nullptr;
}

static int
import_all_from(PyObject *locals, PyObject *v) {
    _Py_IDENTIFIER(__all__);
    _Py_IDENTIFIER(__dict__);
    PyObject *all = _PyObject_GetAttrId(v, &PyId___all__);
    PyObject *dict, *name, *value;
    int skip_leading_underscores = 0;
    int pos, err;

    if (all == nullptr) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError))
            return -1; /* Unexpected error */
        PyErr_Clear();
        dict = _PyObject_GetAttrId(v, &PyId___dict__);
        if (dict == nullptr) {
            if (!PyErr_ExceptionMatches(PyExc_AttributeError))
                return -1;
            PyErr_SetString(PyExc_ImportError,
                "from-import-* object has no __dict__ and no __all__");
            return -1;
        }
        all = PyMapping_Keys(dict);
        Py_DECREF(dict);
        if (all == nullptr)
            return -1;
        skip_leading_underscores = 1;
    }

    for (pos = 0, err = 0;; pos++) {
        name = PySequence_GetItem(all, pos);
        if (name == nullptr) {
            if (!PyErr_ExceptionMatches(PyExc_IndexError))
                err = -1;
            else
                PyErr_Clear();
            break;
        }

        if (skip_leading_underscores &&
            PyUnicode_Check(name) &&
            PyUnicode_READY(name) != -1 &&
            PyUnicode_READ_CHAR(name, 0) == '_') {
            Py_DECREF(name);
            continue;
        }
        value = PyObject_GetAttr(v, name);
        if (value == nullptr)
            err = -1;
        else if (PyDict_CheckExact(locals))
            err = PyDict_SetItem(locals, name, value);
        else
            err = PyObject_SetItem(locals, name, value);
        Py_DECREF(name);
        Py_XDECREF(value);
        if (err != 0)
            break;
    }
    Py_DECREF(all);
    return err;
}

int PyJit_ImportStar(PyObject*from, PyFrameObject* f) {
    PyObject *locals;
    int err;
    if (PyFrame_FastToLocalsWithError(f) < 0)
        return 1;

    locals = f->f_locals;
    if (locals == nullptr) {
        PyErr_SetString(PyExc_SystemError,
            "no locals found during 'import *'");
        return 1;
    }
    err = import_all_from(locals, from);
    PyFrame_LocalsToFast(f, 0);
    Py_DECREF(from);
    return err;
}

PyObject* PyJit_CallKwArgs(PyObject* func, PyObject*callargs, PyObject*kwargs) {
	PyObject* result = nullptr;
	if (!PyDict_CheckExact(kwargs)) {
		PyObject *d = PyDict_New();
        if (d == nullptr) {
            goto error;
        }
		if (PyDict_Update(d, kwargs) != 0) {
			Py_DECREF(d);
			/* PyDict_Update raises attribute
			* error (percolated from an attempt
			* to get 'keys' attribute) instead of
			* a type error if its second argument
			* is not a mapping.
			*/
			if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
				PyErr_Format(PyExc_TypeError,
					"%.200s%.200s argument after ** "
					"must be a mapping, not %.200s",
					PyEval_GetFuncName(func),
					PyEval_GetFuncDesc(func),
					kwargs->ob_type->tp_name);
			}
			goto error;
		}
		Py_DECREF(kwargs);
		kwargs = d;
	}

	if (!PyTuple_CheckExact(callargs)) {
		if (Py_TYPE(callargs)->tp_iter == nullptr &&
			!PySequence_Check(callargs)) {
			PyErr_Format(PyExc_TypeError,
				"%.200s%.200s argument after * "
				"must be an iterable, not %.200s",
				PyEval_GetFuncName(func),
				PyEval_GetFuncDesc(func),
				callargs->ob_type->tp_name);
			goto error;
		}

		auto tmp = PySequence_Tuple(callargs);
		if (tmp == nullptr) {
			goto error;
		}
		Py_DECREF(callargs);
		callargs = tmp;
	}
#ifdef GIL
    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();
#endif
	result = PyObject_Call(func, callargs, kwargs);
#ifdef GIL
	PyGILState_Release(gstate);
#endif
error:
	Py_DECREF(func);
	Py_DECREF(callargs);
	Py_DECREF(kwargs);
	return result;
}

PyObject* PyJit_CallArgs(PyObject* func, PyObject*callargs) {
	PyObject* result = nullptr;
	if (!PyTuple_CheckExact(callargs)) {
		if (Py_TYPE(callargs)->tp_iter == nullptr &&
			!PySequence_Check(callargs)) {
			PyErr_Format(PyExc_TypeError,
				"%.200s%.200s argument after * "
				"must be an iterable, not %.200s",
				PyEval_GetFuncName(func),
				PyEval_GetFuncDesc(func),
				callargs->ob_type->tp_name);
			goto error;
		}
		auto tmp = PySequence_Tuple(callargs);
		if (tmp == nullptr) {
			goto error;
			return nullptr;
		}
		Py_DECREF(callargs);
		callargs = tmp;
	}
#ifdef GIL
    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();
#endif
	result = PyObject_Call(func, callargs, nullptr);
#ifdef GIL
	PyGILState_Release(gstate);
#endif
error:
	Py_DECREF(func);
	Py_DECREF(callargs);
	return result;
}

void PyJit_PushFrame(PyFrameObject* frame) {
    PyThreadState_GET()->frame = frame;
}

void PyJit_PopFrame(PyFrameObject* frame) {
    PyThreadState_GET()->frame = frame->f_back;
}

void PyJit_EhTrace(PyFrameObject *f) {
    PyTraceBack_Here(f);
}

int PyJit_Raise(PyObject *exc, PyObject *cause) {
    PyObject *type = nullptr, *value = nullptr;

    if (exc == nullptr) {
        /* Reraise */
        PyThreadState *tstate = PyThreadState_GET();
        PyObject *tb;
        type = tstate->curexc_type;
        value = tstate->curexc_value;
        tb = tstate->curexc_traceback;
        if (type == Py_None || type == nullptr) {
            PyErr_SetString(PyExc_RuntimeError,
                "No active exception to reraise");
            return 0;
        }
        Py_XINCREF(type);
        Py_XINCREF(value);
        Py_XINCREF(tb);
        PyErr_Restore(type, value, tb);
        return 1;
    }

    /* We support the following forms of raise:
    raise
    raise <instance>
    raise <type> */

    if (PyExceptionClass_Check(exc)) {
        type = exc;
        value = PyObject_CallObject(exc, nullptr);
        if (value == nullptr)
            goto raise_error;
        if (!PyExceptionInstance_Check(value)) {
            PyErr_Format(PyExc_TypeError,
                "calling %R should have returned an instance of "
                "BaseException, not %R",
                type, Py_TYPE(value));
            goto raise_error;
        }
    }
    else if (PyExceptionInstance_Check(exc)) {
        value = exc;
        type = PyExceptionInstance_Class(exc);
        Py_INCREF(type);
    }
    else {
        /* Not something you can raise.  You get an exception
        anyway, just not what you specified :-) */
        Py_DECREF(exc);
        PyErr_SetString(PyExc_TypeError,
            "exceptions must derive from BaseException");
        goto raise_error;
    }

    if (cause) {
        PyObject *fixed_cause;
        if (PyExceptionClass_Check(cause)) {
            fixed_cause = PyObject_CallObject(cause, nullptr);
            if (fixed_cause == nullptr)
                goto raise_error;
            Py_DECREF(cause);
        }
        else if (PyExceptionInstance_Check(cause)) {
            fixed_cause = cause;
        }
        else if (cause == Py_None) {
            Py_DECREF(cause);
            fixed_cause = nullptr;
        }
        else {
            PyErr_SetString(PyExc_TypeError,
                "exception causes must derive from "
                "BaseException");
            goto raise_error;
        }
        PyException_SetCause(value, fixed_cause);
    }

    PyErr_SetObject(type, value);
    /* PyErr_SetObject incref's its arguments */
    Py_XDECREF(value);
    Py_XDECREF(type);
    return 0;

raise_error:
    Py_XDECREF(value);
    Py_XDECREF(type);
    Py_XDECREF(cause);
    return 0;
}

PyObject* PyJit_LoadClassDeref(PyFrameObject* frame, size_t oparg) {
    PyObject* value;
    PyCodeObject* co = frame->f_code;
    size_t idx = oparg - PyTuple_GET_SIZE(co->co_cellvars);
    assert(idx >= 0 && idx < ((size_t)PyTuple_GET_SIZE(co->co_freevars)));
    auto name = PyTuple_GET_ITEM(co->co_freevars, idx);
    auto locals = frame->f_locals;
    if (PyDict_CheckExact(locals)) {
        value = PyDict_GetItem(locals, name);
        Py_XINCREF(value);
    }
    else {
        value = PyObject_GetItem(locals, name);
        if (value == nullptr && PyErr_Occurred()) {
            if (!PyErr_ExceptionMatches(PyExc_KeyError)) {
                return nullptr;
            }
            PyErr_Clear();
        }
    }
    if (!value) {
        auto freevars = frame->f_localsplus + co->co_nlocals;
        PyObject *cell = freevars[oparg];
        value = PyCell_GET(cell);
        if (value == nullptr) {
            format_exc_unbound(co, (int)oparg);
            return nullptr;
        }
        Py_INCREF(value);
    }

    return value;
}

PyObject* PyJit_ExtendList(PyObject *iterable, PyObject *list) {
    assert(list != nullptr);
    assert(PyList_CheckExact(list));
    PyObject *none_val = _PyList_Extend((PyListObject *)list, iterable);
    if (none_val == nullptr) {
        if (Py_TYPE(iterable)->tp_iter == nullptr && !PySequence_Check(iterable))
        {
            PyErr_Format(PyExc_TypeError,
                         "argument must be an iterable, not %.200s",
                         iterable->ob_type->tp_name);
            goto error;
        }
        Py_DECREF(iterable);
        goto error;
    }
    Py_DECREF(none_val);
    Py_DECREF(iterable);
    return list;
error:
    return nullptr;
}

PyObject* PyJit_ListToTuple(PyObject *list) {
    PyObject* res = PyList_AsTuple(list);
    Py_DECREF(list);
    return res;
}

int PyJit_StoreMap(PyObject *key, PyObject *value, PyObject* map) {
    assert(PyDict_CheckExact(map));
    assert(value != nullptr);
    auto res = PyDict_SetItem(map, key, value);
    Py_DECREF(key);
    Py_DECREF(value);
    return res;
}

int PyJit_StoreMapNoDecRef(PyObject *key, PyObject *value, PyObject* map) {
    assert(map != nullptr);
    assert(value != nullptr);
	assert(PyDict_CheckExact(map));
	auto res = PyDict_SetItem(map, key, value);
	return res;
}

PyObject * PyJit_BuildDictFromTuples(PyObject *keys_and_values) {
    assert(keys_and_values != nullptr);
    auto len = PyTuple_GET_SIZE(keys_and_values) - 1;
    PyObject* keys = PyTuple_GET_ITEM(keys_and_values, len);
    if (keys == nullptr){
        PyErr_Format(PyExc_TypeError, "Cannot build dict, keys are null.");
        return nullptr;
    }
    if (!PyTuple_Check(keys)){
        PyErr_Format(PyExc_TypeError, "Cannot build dict, keys are %s,not tuple type.", keys->ob_type->tp_name);
        return nullptr;
    }
    auto map = _PyDict_NewPresized(len);
    if (map == nullptr) {
        goto error;
    }
    for (auto i = 0; i < len; i++) {
        int err;
        PyObject *key = PyTuple_GET_ITEM(keys, i);
        PyObject *value = PyTuple_GET_ITEM(keys_and_values, i);
        err = PyDict_SetItem(map, key, value);
        if (err != 0) {
            Py_DECREF(map);
            goto error;
        }
    }
error:
    Py_DECREF(keys_and_values); // will decref 'keys' tuple as part of its dealloc routine
    return map;
}

PyObject* PyJit_LoadAssertionError() {
    PyObject *value = PyExc_AssertionError;
    Py_INCREF(value);
    return value;
}

PyObject* PyJit_DictUpdate(PyObject* other, PyObject* dict) {
    assert(dict != nullptr);
    if (PyDict_Update(dict, other) < 0){
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Format(PyExc_TypeError,
                          "'%.200s' object is not a mapping",
                          Py_TYPE(other)->tp_name);
        }
        goto error;
    }

    Py_DECREF(other);
    return dict;
error:
    Py_DECREF(other);
    return nullptr;
}

PyObject* PyJit_DictMerge(PyObject* dict, PyObject* other) {
    assert(dict != nullptr);
    if (_PyDict_MergeEx(dict, other, 2) < 0){
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Format(PyExc_TypeError,
                         "'%.200s' object is not a mapping",
                         Py_TYPE(other)->tp_name);
        }
        goto error;
    }

    Py_DECREF(other);
    return dict;
error:
    Py_DECREF(other);
    return nullptr;
}

int PyJit_StoreSubscr(PyObject* value, PyObject *container, PyObject *index) {
    auto res = PyObject_SetItem(container, index, value);
    Py_DECREF(index);
    Py_DECREF(value);
    Py_DECREF(container);
    return res;
}

int PyJit_StoreSubscrIndex(PyObject* value, PyObject *container, PyObject *objIndex, Py_ssize_t index) {
    PyMappingMethods *m;
    int res;

    if (container == nullptr || objIndex == nullptr || value == nullptr) {
        return -1;
    }
    m = Py_TYPE(container)->tp_as_mapping;
    if (m && m->mp_ass_subscript) {
        res = m->mp_ass_subscript(container, objIndex, value);
    } else if (Py_TYPE(container)->tp_as_sequence) {
        res = PySequence_SetItem(container, index, value);
    } else {
        PyErr_Format(PyExc_TypeError,
                     "'%.200s' object does not support item assignment",
                     Py_TYPE(container)->tp_name);
        res = -1;
    }

    Py_DECREF(objIndex);
    Py_DECREF(value);
    Py_DECREF(container);
    return res;
}

int PyJit_StoreSubscrIndexHash(PyObject* value, PyObject *container, PyObject *objIndex, Py_ssize_t index, Py_hash_t hash)
{
    if (PyDict_CheckExact(container))
        return PyJit_StoreSubscrDictHash(value, container, objIndex, hash);
    else
        return PyJit_StoreSubscrIndex(value, container, objIndex, index);
}

int PyJit_StoreSubscrDict(PyObject* value, PyObject *container, PyObject *index) {
    if(!PyDict_CheckExact(container)) // just incase we got the type wrong.
        return PyJit_StoreSubscr(value, container, index);
    auto res = PyDict_SetItem(container, index, value);
    Py_DECREF(index);
    Py_DECREF(value);
    Py_DECREF(container);
    return res;
}

int PyJit_StoreSubscrDictHash(PyObject* value, PyObject *container, PyObject *index, Py_hash_t hash) {
    if(!PyDict_CheckExact(container)) // just incase we got the type wrong.
        return PyJit_StoreSubscr(value, container, index);
    auto res = _PyDict_SetItem_KnownHash(container, index, value, hash);
    Py_DECREF(index);
    Py_DECREF(value);
    Py_DECREF(container);
    return res;
}

int PyJit_StoreSubscrList(PyObject* value, PyObject *container, PyObject *index) {
    int res ;
    if(!PyList_CheckExact(container)) // just incase we got the type wrong.
        return PyJit_StoreSubscr(value, container, index);
    if (PyIndex_Check(index)) {
        Py_ssize_t key_value;
        key_value = PyNumber_AsSsize_t(index, PyExc_IndexError);
        if (key_value == -1 && PyErr_Occurred()) {
            res = -1;
        } else if (key_value < 0){
            // Supports negative indexes without converting back to PyLong..
            res = PySequence_SetItem(container, key_value, value);
        } else {
            res = PyList_SetItem(container, key_value, value);
            Py_INCREF(value);
        }
    }
    else {
        return PyJit_StoreSubscr(value, container, index);
    }
    Py_DECREF(index);
    Py_DECREF(value);
    Py_DECREF(container);
    return res;
}

int PyJit_StoreSubscrListIndex(PyObject* value, PyObject *container, PyObject * objIndex, Py_ssize_t index) {
    int res ;
    if(!PyList_CheckExact(container)) // just incase we got the type wrong.
        return PyJit_StoreSubscr(value, container, objIndex);
    res = PyList_SetItem(container, index, value);
    Py_INCREF(value);
    Py_DECREF(objIndex);
    Py_DECREF(value);
    Py_DECREF(container);
    return res;
}

int PyJit_DeleteSubscr(PyObject *container, PyObject *index) {
    auto res = PyObject_DelItem(container, index);
    Py_DECREF(index);
    Py_DECREF(container);
    return res;
}

PyObject* PyJit_CallN(PyObject *target, PyObject* args) {
    // we stole references for the tuple...
#ifdef GIL
    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();
#endif
    auto res = PyObject_Call(target, args, nullptr);
#ifdef GIL
    PyGILState_Release(gstate);
#endif
    Py_DECREF(target);
    Py_DECREF(args);
    return res;
}

int PyJit_StoreGlobal(PyObject* v, PyFrameObject* f, PyObject* name) {
    int err = PyDict_SetItem(f->f_globals, name, v);
    Py_DECREF(v);
    return err;
}

int PyJit_DeleteGlobal(PyFrameObject* f, PyObject* name) {
    return PyDict_DelItem(f->f_globals, name);
}

PyObject *
PyJit_PyDict_LoadGlobal(PyDictObject *globals, PyDictObject *builtins, PyObject *key) {
	auto res = PyDict_GetItem((PyObject*)globals, key);
	if (res != nullptr) {
		return res;
	}

	return PyDict_GetItem((PyObject*)builtins, key);
}


PyObject* PyJit_LoadGlobal(PyFrameObject* f, PyObject* name) {
    PyObject* v;
    if (PyDict_CheckExact(f->f_globals)
        && PyDict_CheckExact(f->f_builtins)) {
        v = PyJit_PyDict_LoadGlobal((PyDictObject *)f->f_globals,
            (PyDictObject *)f->f_builtins,
            name);
        if (v == nullptr) {
            if (!_PyErr_OCCURRED())
                format_exc_check_arg(PyExc_NameError, NAME_ERROR_MSG, name);
            return nullptr;
        }
        Py_INCREF(v);
    }
    else {
        /* Slow-path if globals or builtins is not a dict */
        auto asciiName = PyUnicode_AsUTF8(name);
        v = PyObject_GetItem(f->f_globals, name);
        if (v == nullptr) {
            v = PyObject_GetItem(f->f_builtins, name);
            if (v == nullptr) {
                if (PyErr_ExceptionMatches(PyExc_KeyError))
                    format_exc_check_arg(
                        PyExc_NameError,
                        NAME_ERROR_MSG, name);
                return nullptr;
            }
            else {
                PyErr_Clear();
            }
        }
    }
    return v;
}

PyObject* PyJit_GetIter(PyObject* iterable) {
    auto res = PyObject_GetIter(iterable);
    Py_DECREF(iterable);
    return res;
}

PyObject* PyJit_IterNext(PyObject* iter) {
    if (iter == nullptr || !PyIter_Check(iter)){
        PyErr_Format(PyExc_ValueError,
                     "Invalid iterator given to iternext, got %s - %s at %p.", ObjInfo(iter),
                     PyUnicode_AsUTF8(PyObject_Repr(iter)), iter);
        return nullptr;
    }

    auto res = (*iter->ob_type->tp_iternext)(iter);
    if (res == nullptr) {
        if (PyErr_Occurred()) {
            if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                return nullptr;
            }
            PyErr_Clear();
            return (PyObject*)(0xff);
        } else {
            return (PyObject*)(0xff);
        }
    }
    return res;
}

PyObject* PyJit_CellGet(PyFrameObject* frame, size_t index) {
    PyObject** cells = frame->f_localsplus + frame->f_code->co_nlocals;
    PyObject *value = PyCell_GET(cells[index]);

    if (value == nullptr) {
        format_exc_unbound(frame->f_code, (int)index);
    }
    else {
        Py_INCREF(value);
    }
    return value;
}

void PyJit_CellSet(PyObject* value, PyFrameObject* frame, size_t index) {
    PyObject** cells = frame->f_localsplus + frame->f_code->co_nlocals;
    auto cell = cells[index];
    if (cell == nullptr){
        cells[index] = PyCell_New(value);
    } else {
        auto oldobj = PyCell_Get(cell);
        PyCell_Set(cell, value);
        Py_XDECREF(oldobj);
    }
}

PyObject* PyJit_BuildClass(PyFrameObject *f) {
    _Py_IDENTIFIER(__build_class__);

    PyObject *bc;
    if (PyDict_CheckExact(f->f_builtins)) {
        bc = _PyDict_GetItemId(f->f_builtins, &PyId___build_class__);
        if (bc == nullptr) {
            PyErr_SetString(PyExc_NameError,
                "__build_class__ not found");
            return nullptr;
        }
        Py_INCREF(bc);
    }
    else {
        PyObject *build_class_str = _PyUnicode_FromId(&PyId___build_class__);
        if (build_class_str == nullptr) {
            return nullptr;
        }
        bc = PyObject_GetItem(f->f_builtins, build_class_str);
        if (bc == nullptr) {
            if (PyErr_ExceptionMatches(PyExc_KeyError)) {
                PyErr_SetString(PyExc_NameError, "__build_class__ not found");
                return nullptr;
            }
        }
    }
    return bc;
}

// Returns: the address for the 1st set of items, the constructed list, and the
// address where the remainder live.
PyObject** PyJit_UnpackSequenceEx(PyObject* seq, size_t leftSize, size_t rightSize, PyObject** tempStorage, PyObject** listRes, PyObject*** remainder) {
    if (PyTuple_CheckExact(seq) && ((size_t)PyTuple_GET_SIZE(seq)) >= (leftSize + rightSize)) {
        auto listSize = PyTuple_GET_SIZE(seq) - (leftSize + rightSize);
        auto list = (PyListObject*)PyList_New(listSize);
        if (list == nullptr) {
            return nullptr;
        }
        for (int i = 0; i < listSize; i++) {
            list->ob_item[i] = ((PyTupleObject *)seq)->ob_item[i + leftSize];
        }
        *remainder = ((PyTupleObject *)seq)->ob_item + leftSize + listSize;
        *listRes = (PyObject*)list;
        auto res = ((PyTupleObject *)seq)->ob_item;
        auto size = PyTuple_GET_SIZE(seq);
        for (int i = 0; i < size; i++) {
            Py_INCREF(res[i]);
        }
        return res;
    }
    else if (PyList_CheckExact(seq) && ((size_t)PyList_GET_SIZE(seq)) >= (leftSize + rightSize)) {
        auto listSize = PyList_GET_SIZE(seq) - (leftSize + rightSize);
        auto list = (PyListObject*)PyList_New(listSize);
        if (list == nullptr) {
            return nullptr;
        }
        for (int i = 0; i < listSize; i++) {
            list->ob_item[i] = ((PyListObject *)seq)->ob_item[i + leftSize];
        }
        *remainder = ((PyListObject *)seq)->ob_item + leftSize + listSize;
        *listRes = (PyObject*)list;

        auto res = ((PyListObject *)seq)->ob_item;
        auto size = PyList_GET_SIZE(seq);
        for (int i = 0; i < size; i++) {
            Py_INCREF(res[i]);
        }
        return res;
    }
    else {
        // the function allocated space on the stack for us to
        // store these temporarily.
        auto it = PyObject_GetIter(seq);
        int i = 0;
        auto sp = tempStorage + leftSize + rightSize;

        if (it == nullptr) {
            goto Error;
        }


        for (; i < leftSize; i++) {
            auto w = PyIter_Next(it);
            if (w == nullptr) {
                /* Iterator done, via error or exhaustion. */
                if (!PyErr_Occurred()) {
                    PyErr_Format(PyExc_ValueError,
                        "need more than %d value%s to unpack",
                        i, i == 1 ? "" : "s");
                }
                goto Error;
            }
            tempStorage[i] = w;
        }

        if (listRes == nullptr) {
            /* We better have exhausted the iterator now. */
            auto w = PyIter_Next(it);
            if (w == nullptr) {
                if (PyErr_Occurred())
                    goto Error;
                Py_DECREF(it);
                return tempStorage;
            }
            Py_DECREF(w);
            PyErr_Format(PyExc_ValueError, "too many values to unpack "
                "(expected %d)", leftSize);
            goto Error;
        }
        else {

            auto l = PySequence_List(it);
            if (l == nullptr)
                goto Error;
            *listRes = l;
            i++;

            size_t ll = PyList_GET_SIZE(l);
            if (ll < rightSize) {
                PyErr_Format(PyExc_ValueError, "need more than %zd values to unpack",
                    leftSize + ll);
                goto Error;
            }

            /* Pop the "after-variable" args off the list. */
            for (auto j = rightSize; j > 0; j--, i++) {
                *--sp = PyList_GET_ITEM(l, ll - j);
            }
            /* Resize the list. */
            Py_SIZE(l) = ll - rightSize;
        }
        Py_DECREF(it);
        if (remainder != nullptr) {
            *remainder = tempStorage + leftSize;
        }
        return tempStorage;

    Error:
        for (int j = 0; j < leftSize; j++) {
            Py_XDECREF(tempStorage[j]);
        }
        Py_XDECREF(it);
        return nullptr;
    }
}

void PyJit_UnpackError(size_t expected, size_t got) {
    if (got < expected) {
        PyErr_Format(PyExc_ValueError,
            "need more than %d value%s to unpack",
            got, got == 1 ? "" : "s");
    }
    else if (got > expected) {
        PyErr_Format(PyExc_ValueError, "too many values to unpack "
            "(expected %d)", expected);
    }
}

// Unpacks the given sequence and returns a pointer to where the sequence
// is stored.  If this is a type we can just grab the array from it returns
// the array.  Otherwise we unpack the sequence into tempStorage which was
// allocated on the stack when we entered the generated method body.
PyObject** PyJit_UnpackSequence(PyObject* seq, size_t size, PyObject** tempStorage) {
    if (PyTuple_CheckExact(seq)) {
        if (PyTuple_GET_SIZE(seq) == size) {
            PyObject** res = ((PyTupleObject *)seq)->ob_item;
            for (int i = 0; i < size; i++) {
                Py_INCREF(res[i]);
            }
            return res;
        }

        PyJit_UnpackError(size, PyTuple_GET_SIZE(seq));
        return nullptr;
    }
    else if (PyList_CheckExact(seq)) {
        if (PyList_GET_SIZE(seq) == size) {
            PyObject** res = ((PyListObject *)seq)->ob_item;
            for (int i = 0; i < size; i++) {
                Py_INCREF(res[i]);
            }
            return res;
        }
        PyJit_UnpackError(size, PyList_GET_SIZE(seq));
        return nullptr;
    }

    return PyJit_UnpackSequenceEx(seq, size, 0, tempStorage, nullptr, nullptr);
}

PyObject* PyJit_LoadAttr(PyObject* owner, PyObject* name) {
    PyObject *res = PyObject_GetAttr(owner, name);
    Py_DECREF(owner);
    return res;
}

const char * ObjInfo(PyObject *obj) {
    if (obj == nullptr) {
        return "<NULL>";
    }
    if (PyUnicode_Check(obj)) {
        return PyUnicode_AsUTF8(obj);
    }
    else if (obj->ob_type != nullptr) {
        return obj->ob_type->tp_name;
    }
    else {
        return "<null type>";
    }
}

int PyJit_StoreAttr(PyObject* value, PyObject* owner, PyObject* name) {
    int res = PyObject_SetAttr(owner, name, value);
    Py_DECREF(owner);
    Py_DECREF(value);
    return res;
}

int PyJit_DeleteAttr(PyObject* owner, PyObject* name) {
    int res = PyObject_DelAttr(owner, name);
    Py_DECREF(owner);
    return res;
}

int PyJit_SetupAnnotations(PyFrameObject* frame) {
    _Py_IDENTIFIER(__annotations__);
    int err;
    PyObject *ann_dict;
    if (frame->f_locals == nullptr) {
        PyErr_Format(PyExc_SystemError,
                      "no locals found when setting up annotations");
        return -1;
    }
    /* check if __annotations__ in locals()... */
    if (PyDict_CheckExact(frame->f_locals)) {
        ann_dict = _PyDict_GetItemIdWithError(frame->f_locals,
                                              &PyId___annotations__);
        if (ann_dict == nullptr) {
            if (PyErr_Occurred()) {
                return -1;
            }
            /* ...if not, create a new one */
            ann_dict = PyDict_New();
            if (ann_dict == nullptr) {
                return -1;
            }
            err = _PyDict_SetItemId(frame->f_locals,
                                    &PyId___annotations__, ann_dict);
            Py_DECREF(ann_dict);
            if (err != 0) {
                return -1;
            }
        }
    }
    else {
        /* do the same if locals() is not a dict */
        PyObject *ann_str = _PyUnicode_FromId(&PyId___annotations__);
        if (ann_str == nullptr) {
            return -1;
        }
        ann_dict = PyObject_GetItem(frame->f_locals, ann_str);
        if (ann_dict == nullptr) {
            if (!PyErr_ExceptionMatches(PyExc_KeyError)) {
                return -1;
            }
            PyErr_Clear();
            ann_dict = PyDict_New();
            if (ann_dict == nullptr) {
                return -1;
            }
            err = PyObject_SetItem(frame->f_locals, ann_str, ann_dict);
            Py_DECREF(ann_dict);
            if (err != 0) {
                return -1;
            }
        }
        else {
            Py_DECREF(ann_dict);
        }
    }
    return 0;
}

PyObject* PyJit_LoadName(PyFrameObject* f, PyObject* name) {
    PyObject *locals = f->f_locals;
    PyObject *v;
    if (locals == nullptr) {
        PyErr_Format(PyExc_SystemError,
            "no locals when loading %R", name);
        return nullptr;
    }
    if (PyDict_CheckExact(locals)) {
        v = PyDict_GetItem(locals, name);
        Py_XINCREF(v);
    }
    else {
        v = PyObject_GetItem(locals, name);
        if (v == nullptr && _PyErr_OCCURRED()) {
            if (!PyErr_ExceptionMatches(PyExc_KeyError))
                return nullptr;
            PyErr_Clear();
        }
    }
    if (v == nullptr) {
        v = PyDict_GetItem(f->f_globals, name);
        Py_XINCREF(v);
        if (v == nullptr) {
            if (PyDict_CheckExact(f->f_builtins)) {
                v = PyDict_GetItem(f->f_builtins, name);
                if (v == nullptr) {
                    format_exc_check_arg(
                        PyExc_NameError,
                        NAME_ERROR_MSG, name);
                    return nullptr;
                }
                Py_INCREF(v);
            }
            else {
                v = PyObject_GetItem(f->f_builtins, name);
                if (v == nullptr) {
                    if (PyErr_ExceptionMatches(PyExc_KeyError))
                        format_exc_check_arg(
                            PyExc_NameError,
                            NAME_ERROR_MSG, name);
                    return nullptr;
                }
            }
        }
    }
    return v;
}

int PyJit_StoreName(PyObject* v, PyFrameObject* f, PyObject* name) {
    PyObject *ns = f->f_locals;
    int err;
    if (ns == nullptr) {
        PyErr_Format(PyExc_SystemError,
            "no locals found when storing %R", name);
        Py_DECREF(v);
        return 1;
    }
    if (PyDict_CheckExact(ns))
        err = PyDict_SetItem(ns, name, v);
    else
        err = PyObject_SetItem(ns, name, v);
    Py_DECREF(v);
    return err;
}

int PyJit_DeleteName(PyFrameObject* f, PyObject* name) {
    PyObject *ns = f->f_locals;
    int err;
    if (ns == nullptr) {
        PyErr_Format(PyExc_SystemError,
            "no locals when deleting %R", name);
        return 1;
    }
    err = PyObject_DelItem(ns, name);
    if (err != 0) {
        format_exc_check_arg(PyExc_NameError,
            NAME_ERROR_MSG,
            name);
    }
    return err;
}


template<typename T>
inline T tuple_build(PyObject* v, PyObject* arg) {
    int l = PyTuple_Size(v);
}

template<typename T>
inline PyObject* Call(PyObject* target) {
    return Call0(target);
}

template<typename T, typename ... Args>
inline PyObject* Call(PyObject *target, Args...args) {
    auto tstate = PyThreadState_GET();
    PyObject* res = nullptr;
    if (target == nullptr) {
        if (!PyErr_Occurred())
            PyErr_Format(PyExc_TypeError,
                         "missing target in call");
        return nullptr;
    }
    if (PyCFunction_Check(target)) {
        PyObject* _args[sizeof...(args)] = {args...};
#ifdef GIL
        PyGILState_STATE gstate;
        gstate = PyGILState_Ensure();
#endif
        if (tstate->use_tracing && tstate->c_profileobj && g_pyjionSettings.profiling) {
            // Call the function with profiling hooks
            trace(tstate, tstate->frame, PyTrace_C_CALL, target, tstate->c_profilefunc, tstate->c_profileobj);
            res = PyObject_Vectorcall(target, _args, sizeof...(args) | PY_VECTORCALL_ARGUMENTS_OFFSET, nullptr);
            if (res == nullptr)
                trace(tstate, tstate->frame, PyTrace_C_EXCEPTION, target, tstate->c_profilefunc, tstate->c_profileobj);
            else
                trace(tstate, tstate->frame, PyTrace_C_RETURN, target, tstate->c_profilefunc, tstate->c_profileobj);
        } else {
            // Regular function call
            res = PyObject_Vectorcall(target, _args, sizeof...(args) | PY_VECTORCALL_ARGUMENTS_OFFSET, nullptr);
        }

#ifdef GIL
        PyGILState_Release(gstate);
#endif
    } else {
        auto t_args = PyTuple_New(sizeof...(args));
        if (t_args == nullptr) {
            goto error;
        }
        std::vector<PyObject*> args_v = {args...};
        for (int i = 0; i < args_v.size() ; i ++) {
            assert(args_v[i] != nullptr);
            PyTuple_SetItem(t_args, i, args_v[i]);
            Py_INCREF(args_v[i]);
        }
#ifdef GIL
        PyGILState_STATE gstate;
        gstate = PyGILState_Ensure();
#endif
        res = PyObject_Call(target, t_args, nullptr);
#ifdef GIL
        PyGILState_Release(gstate);
#endif
        Py_DECREF(t_args);
    }
    error:
    Py_DECREF(target);
    for (auto &i: {args...})
        Py_DECREF(i);
    return res;
}

PyObject* Call0(PyObject *target) {
    PyObject* res = nullptr;
    auto tstate = PyThreadState_GET();
    if (target == nullptr){
        if (!PyErr_Occurred())
            PyErr_Format(PyExc_TypeError,
                         "missing target in call");
        return nullptr;
    }
#ifdef GIL
    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();
#endif
    if (PyCFunction_Check(target)) {
        if (tstate->use_tracing && tstate->c_profileobj && g_pyjionSettings.profiling) {
            // Call the function with profiling hooks
            trace(tstate, tstate->frame, PyTrace_C_CALL, target, tstate->c_profilefunc, tstate->c_profileobj);
            res = PyObject_Vectorcall(target, nullptr, 0 | PY_VECTORCALL_ARGUMENTS_OFFSET, nullptr);
            if (res == nullptr)
                trace(tstate, tstate->frame, PyTrace_C_EXCEPTION, target, tstate->c_profilefunc, tstate->c_profileobj);
            else
                trace(tstate, tstate->frame, PyTrace_C_RETURN, target, tstate->c_profilefunc, tstate->c_profileobj);
        } else {
            // Regular function call
            res = PyObject_Vectorcall(target, nullptr, 0 | PY_VECTORCALL_ARGUMENTS_OFFSET, nullptr);
        }
    }
    else {
        res = PyObject_CallNoArgs(target);
    }
#ifdef GIL
    PyGILState_Release(gstate);
#endif
    Py_DECREF(target);
    return res;
}

PyObject* Call1(PyObject *target, PyObject* arg0) {
    return Call<PyObject *>(target, arg0);
}

PyObject* Call2(PyObject *target, PyObject* arg0, PyObject* arg1) {
    return Call<PyObject*>(target, arg0, arg1);
}

PyObject* Call3(PyObject *target, PyObject* arg0, PyObject* arg1, PyObject* arg2) {
    return Call<PyObject*>(target, arg0, arg1, arg2);
}

PyObject* Call4(PyObject *target, PyObject* arg0, PyObject* arg1, PyObject* arg2, PyObject* arg3) {
    return Call<PyObject*>(target, arg0, arg1, arg2, arg3);
}

PyObject* Call5(PyObject *target, PyObject* arg0, PyObject* arg1, PyObject* arg2, PyObject* arg3, PyObject* arg4){
    return Call<PyObject*>(target, arg0, arg1, arg2, arg3, arg4);
}

PyObject* Call6(PyObject *target, PyObject* arg0, PyObject* arg1, PyObject* arg2, PyObject* arg3, PyObject* arg4, PyObject* arg5){
    return Call<PyObject*>(target, arg0, arg1, arg2, arg3, arg4, arg5);
}

PyObject* Call7(PyObject *target, PyObject* arg0, PyObject* arg1, PyObject* arg2, PyObject* arg3, PyObject* arg4, PyObject* arg5, PyObject* arg6){
    return Call<PyObject*>(target, arg0, arg1, arg2, arg3, arg4, arg5, arg6);
}

PyObject* Call8(PyObject *target, PyObject* arg0, PyObject* arg1, PyObject* arg2, PyObject* arg3, PyObject* arg4, PyObject* arg5, PyObject* arg6, PyObject* arg7){
    return Call<PyObject*>(target, arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7);
}

PyObject* Call9(PyObject *target, PyObject* arg0, PyObject* arg1, PyObject* arg2, PyObject* arg3, PyObject* arg4, PyObject* arg5, PyObject* arg6, PyObject* arg7, PyObject* arg8){
    return Call<PyObject*>(target, arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8);
}

PyObject* Call10(PyObject *target, PyObject* arg0, PyObject* arg1, PyObject* arg2, PyObject* arg3, PyObject* arg4, PyObject* arg5, PyObject* arg6, PyObject* arg7, PyObject* arg8, PyObject* arg9){
    return Call<PyObject*>(target, arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9);
}

PyObject* MethCall0(PyObject* self, PyMethodLocation* method_info) {
    PyObject* res;
    if (method_info->object != nullptr)
        res = Call<PyObject*>(method_info->method, method_info->object);
    else
        res = Call0(method_info->method);
    delete method_info;
    return res;
}

PyObject* MethCall1(PyObject* self, PyMethodLocation* method_info, PyObject* arg1) {
    PyObject* res;
    if (method_info->object != nullptr)
        res = Call<PyObject*>(method_info->method, method_info->object, arg1);
    else
        res = Call<PyObject*>(method_info->method, arg1);
    delete method_info;
    return res;
}

PyObject* MethCall2(PyObject* self, PyMethodLocation* method_info, PyObject* arg1, PyObject* arg2) {
    PyObject* res;
    if (method_info->object != nullptr)
        res = Call<PyObject*>(method_info->method, method_info->object, arg1, arg2);
    else
        res = Call<PyObject*>(method_info->method, arg1, arg2);
    delete method_info;
    return res;
}

PyObject* MethCall3(PyObject* self, PyMethodLocation* method_info, PyObject* arg1, PyObject* arg2, PyObject* arg3) {
    PyObject* res;
    if (method_info->object != nullptr)
        res = Call<PyObject*>(method_info->method, method_info->object, arg1, arg2, arg3);
    else
        res = Call<PyObject*>(method_info->method, arg1, arg2, arg3);
    delete method_info;
    return res;
}

PyObject* MethCall4(PyObject* self, PyMethodLocation* method_info, PyObject* arg1, PyObject* arg2, PyObject* arg3, PyObject* arg4) {
    PyObject* res;
    if (method_info->object != nullptr)
        res = Call<PyObject*>(method_info->method, method_info->object, arg1, arg2, arg3, arg4);
    else
        res = Call<PyObject*>(method_info->method, arg1, arg2, arg3, arg4);
    delete method_info;
    return res;
}

PyObject* MethCall5(PyObject* self, PyMethodLocation* method_info, PyObject* arg1, PyObject* arg2, PyObject* arg3, PyObject* arg4, PyObject* arg5){
    PyObject* res;
    if (method_info->object != nullptr)
        res = Call<PyObject*>(method_info->method, method_info->object, arg1, arg2, arg3, arg4, arg5);
    else
        res = Call<PyObject*>(method_info->method, arg1, arg2, arg3, arg4, arg5);
    delete method_info;
    return res;
}

PyObject* MethCall6(PyObject* self, PyMethodLocation* method_info, PyObject* arg1, PyObject* arg2, PyObject* arg3, PyObject* arg4, PyObject* arg5, PyObject* arg6){
    PyObject* res;
    if (method_info->object != nullptr)
        res = Call<PyObject*>(method_info->method, method_info->object, arg1, arg2, arg3, arg4, arg5, arg6);
    else
        res = Call<PyObject*>(method_info->method, arg1, arg2, arg3, arg4, arg5, arg6);
    delete method_info;
    return res;
}

PyObject* MethCall7(PyObject* self, PyMethodLocation* method_info, PyObject* arg1, PyObject* arg2, PyObject* arg3, PyObject* arg4, PyObject* arg5, PyObject* arg6, PyObject* arg7){
    PyObject* res;
    if (method_info->object != nullptr)
        res = Call<PyObject*>(method_info->method, method_info->object, arg1, arg2, arg3, arg4, arg5, arg6, arg7);
    else
        res = Call<PyObject*>(method_info->method, arg1, arg2, arg3, arg4, arg5, arg6, arg7);
    delete method_info;
    return res;
}

PyObject* MethCall8(PyObject* self, PyMethodLocation* method_info, PyObject* arg1, PyObject* arg2, PyObject* arg3, PyObject* arg4, PyObject* arg5, PyObject* arg6, PyObject* arg7, PyObject* arg8){
    PyObject* res;
    if (method_info->object != nullptr)
        res = Call<PyObject*>(method_info->method, method_info->object, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8);
    else
        res = Call<PyObject*>(method_info->method, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8);
    delete method_info;
    return res;
}

PyObject* MethCall9(PyObject* self, PyMethodLocation* method_info, PyObject* arg1, PyObject* arg2, PyObject* arg3, PyObject* arg4, PyObject* arg5, PyObject* arg6, PyObject* arg7, PyObject* arg8, PyObject* arg9){
    PyObject* res;
    if (method_info->object != nullptr)
        res = Call<PyObject*>(method_info->method, method_info->object, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9);
    else
        res = Call<PyObject*>(method_info->method, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9);
    delete method_info;
    return res;
}

PyObject* MethCall10(PyObject* self, PyMethodLocation* method_info, PyObject* arg1, PyObject* arg2, PyObject* arg3, PyObject* arg4, PyObject* arg5, PyObject* arg6, PyObject* arg7, PyObject* arg8, PyObject* arg9, PyObject* arg10){
    PyObject* res;
    if (method_info->object != nullptr)
        res = Call<PyObject*>(method_info->method, method_info->object, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10);
    else
        res = Call<PyObject*>(method_info->method, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10);
    delete method_info;
    return res;
}

PyObject* MethCallN(PyObject* self, PyMethodLocation* method_info, PyObject* args) {
    PyObject* res;
    if(!PyTuple_Check(args)) {
        PyErr_Format(PyExc_TypeError,
                     "invalid arguments for method call");
        Py_DECREF(args);
        delete method_info;
        return nullptr;
    }
    // TODO : Support vector arg calls.
    if (method_info->object != nullptr)
    {
        auto target = method_info->method;
        if (target == nullptr)
        {
            PyErr_Format(PyExc_ValueError,
                         "cannot resolve method call");
            Py_DECREF(args);
            delete method_info;
            return nullptr;
        }
        auto obj =  method_info->object;
        auto args_tuple = PyTuple_New(PyTuple_Size(args) + 1);

        assert(obj != nullptr);
        if (PyTuple_SetItem(args_tuple, 0, obj) == -1){
            return nullptr;
        }

        Py_INCREF(obj);

        for (int i = 0 ; i < PyTuple_Size(args) ; i ++){
            auto ix = PyTuple_GET_ITEM(args, i);
            assert(ix != nullptr);
            if (PyTuple_SetItem(args_tuple, i+1, ix) == -1){
                return nullptr;
            }
            Py_INCREF(ix);
        }

#ifdef GIL
        PyGILState_STATE gstate;
        gstate = PyGILState_Ensure();
#endif
        res = PyObject_Call(target, args_tuple, nullptr);
#ifdef GIL
        PyGILState_Release(gstate);
#endif
        Py_DECREF(args_tuple);
        Py_DECREF(args);
        Py_DECREF(target);
        Py_DECREF(obj);
        delete method_info;
        return res;
    }
    else {
        auto target = method_info->method;
#ifdef GIL
        PyGILState_STATE gstate;
        gstate = PyGILState_Ensure();
#endif
        res = PyObject_Call(target, args, nullptr);
#ifdef GIL
        PyGILState_Release(gstate);
#endif
        Py_DECREF(args);
        Py_DECREF(target);
        delete method_info;
        return res;
    }
}

PyObject* PyJit_KwCallN(PyObject *target, PyObject* args, PyObject* names) {
	PyObject* result = nullptr, *kwArgs = nullptr;

	auto argCount = PyTuple_Size(args) - PyTuple_Size(names);
	PyObject* posArgs;
	posArgs = PyTuple_New(argCount);
	if (posArgs == nullptr) {
		goto error;
	}
	for (auto i = 0; i < argCount; i++) {
		auto item = PyTuple_GetItem(args, i);
		Py_INCREF(item);
		if (PyTuple_SetItem(posArgs, i, item) == -1){
		    goto error;
		}
	}
	kwArgs = PyDict_New();
	if (kwArgs == nullptr) {
		goto error;
	}

	for (auto i = 0; i < PyTuple_GET_SIZE(names); i++) {
		PyDict_SetItem(
			kwArgs,
			PyTuple_GET_ITEM(names, i),
			PyTuple_GET_ITEM(args, i + argCount)
		);
	}
#ifdef GIL
    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();
#endif
	result = PyObject_Call(target, posArgs, kwArgs);
#ifdef GIL
	PyGILState_Release(gstate);
#endif
error:
	Py_XDECREF(kwArgs);
	Py_XDECREF(posArgs);
	Py_DECREF(target);
	Py_DECREF(args);
	Py_DECREF(names);
	return result;
}

PyObject* PyJit_PyTuple_New(ssize_t len){
    auto t = PyTuple_New(len);
    return t;
}

PyObject* PyJit_Is(PyObject* lhs, PyObject* rhs) {
    auto res = lhs == rhs ? Py_True : Py_False;
    Py_DECREF(lhs);
    Py_DECREF(rhs);
    Py_INCREF(res);
    return res;
}

PyObject* PyJit_IsNot(PyObject* lhs, PyObject* rhs) {
    auto res = lhs == rhs ? Py_False : Py_True;
    Py_DECREF(lhs);
    Py_DECREF(rhs);
    Py_INCREF(res);
    return res;
}

int PyJit_Is_Bool(PyObject* lhs, PyObject* rhs) {
    Py_DECREF(lhs);
    Py_DECREF(rhs);
    return lhs == rhs;
}

int PyJit_IsNot_Bool(PyObject* lhs, PyObject* rhs) {
    Py_DECREF(lhs);
    Py_DECREF(rhs);
    return lhs != rhs;
}

void PyJit_DecRef(PyObject* value) {
	Py_XDECREF(value);
}

static int g_counter;
int PyJit_PeriodicWork() {
	g_counter++;
	if (!(g_counter & 0xfff)) {
		// Pulse the GIL
		g_counter = 0;
		Py_BEGIN_ALLOW_THREADS
		Py_END_ALLOW_THREADS
	}

	if (!(g_counter & 0xffff)) {
		auto ts = PyThreadState_GET();
		if (ts->async_exc != nullptr) {
			PyErr_SetNone(ts->async_exc);
			Py_DECREF(ts->async_exc);
			ts->async_exc = nullptr;
			return -1;
		}
		return Py_MakePendingCalls();
	}

	return 0;
}

PyObject* PyJit_UnicodeJoinArray(PyObject** items, Py_ssize_t count) {
	auto empty = PyUnicode_New(0, 0);
	auto res = _PyUnicode_JoinArray(empty, items, count);
	for (auto i = 0; i < count; i++) {
		Py_DECREF(items[i]);
	}
	Py_DECREF(empty);
	return res;
}

PyObject* PyJit_FormatObject(PyObject* item, PyObject*fmtSpec) {
	auto res = PyObject_Format(item, fmtSpec);
	Py_DECREF(item);
	Py_DECREF(fmtSpec);
	return res;
}

PyMethodLocation* PyJit_LoadMethod(PyObject* object, PyObject* name) {
    auto * result = new PyMethodLocation;
    PyObject* method = nullptr;
    int meth_found = _PyObject_GetMethod(object, name, &method);
    result->method = method;
    if (!meth_found) {
        Py_DECREF(object);
        result->object = nullptr;
    } else {
        result->object = object;
    }
    return result;
}

PyObject* PyJit_FormatValue(PyObject* item) {
	if (PyUnicode_CheckExact(item)) {
		return item;
	}

	auto res = PyObject_Format(item, nullptr);
	Py_DECREF(item);
	return res;
}

inline int trace(PyThreadState *tstate, PyFrameObject *f, int ty, PyObject *args, Py_tracefunc func, PyObject* tracearg) {
    tstate->tracing++;
    tstate->use_tracing = 0;
    int result = func(tracearg, f, ty, args);
    tstate->use_tracing = ((tstate->c_tracefunc != NULL)
                           || (tstate->c_profilefunc != NULL));
    tstate->tracing--;
    return result;
}

void PyJit_TraceLine(PyFrameObject* f, int* instr_lb, int* instr_ub, int* instr_prev){
    auto tstate = PyThreadState_GET();
    if (tstate->c_tracefunc != nullptr && !tstate->tracing) {
        int result = 0;
        int line = f->f_lineno;

        /* If the last instruction executed isn't in the current
           instruction window, reset the window.
        */
        if (f->f_lasti < *instr_lb || f->f_lasti >= *instr_ub) {
            PyAddrPair bounds;
            line = _PyCode_CheckLineNumber(f->f_code, f->f_lasti,
                                           &bounds);
            *instr_lb = bounds.ap_lower;
            *instr_ub = bounds.ap_upper;
        }
        /* If the last instruction falls at the start of a line or if it
           represents a jump backwards, update the frame's line number and
           then call the trace function if we're tracing source lines.
        */
        if ((f->f_lasti == *instr_lb || f->f_lasti < *instr_prev)) {
            f->f_lineno = line;
            if (f->f_trace_lines) {
                if (tstate->tracing)
                    return;
                result = trace(tstate, f, PyTrace_LINE, Py_None, tstate->c_tracefunc, tstate->c_traceobj);
            }
        }
        /* Always emit an opcode event if we're tracing all opcodes. */
        if (f->f_trace_opcodes) {
            if (tstate->tracing)
                return;
            result = trace(tstate, f, PyTrace_OPCODE, Py_None, tstate->c_tracefunc, tstate->c_traceobj);
        }
        *instr_prev = f->f_lasti;

        // TODO : Handle possible change of instruction address
        //  should lookup jump address and branch (f->f_lasti);
        // TODO : Handle error in call trace function
    }
}

inline int protected_trace(PyThreadState* tstate, PyFrameObject* f, int ty, PyObject* arg, Py_tracefunc func, PyObject* tracearg){
    int result = 0;
    PyObject *type, *value, *traceback;
    PyErr_Fetch(&type, &value, &traceback);

    if (tstate->tracing)
        return 0;
    result = trace(tstate, f, ty, arg, func, tracearg);

    if (result == 0) {
        PyErr_Restore(type, value, traceback);
        return 0;
    } else {
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(traceback);
        return -1;
    }
}

void PyJit_TraceFrameEntry(PyFrameObject* f){
    auto tstate = PyThreadState_GET();
    if (tstate->c_tracefunc != nullptr && !tstate->tracing) {
        protected_trace(tstate, f, PyTrace_CALL, Py_None, tstate->c_tracefunc, tstate->c_traceobj);
    }
}

void PyJit_TraceFrameExit(PyFrameObject* f){
    auto tstate = PyThreadState_GET();
    if (tstate->c_tracefunc != nullptr && !tstate->tracing) {
        protected_trace(tstate, f, PyTrace_RETURN, Py_None, tstate->c_tracefunc, tstate->c_traceobj);
    }
}

void PyJit_ProfileFrameEntry(PyFrameObject* f){
    auto tstate = PyThreadState_GET();
    if (tstate->c_profilefunc != nullptr && !tstate->tracing) {
        protected_trace(tstate, f, PyTrace_CALL, Py_None, tstate->c_profilefunc, tstate->c_profileobj);
    }
}

void PyJit_ProfileFrameExit(PyFrameObject* f){
    auto tstate = PyThreadState_GET();
    if (tstate->c_profilefunc != nullptr && !tstate->tracing) {
        protected_trace(tstate, f, PyTrace_RETURN, Py_None, tstate->c_profilefunc, tstate->c_profileobj);
    }
}

void PyJit_TraceFrameException(PyFrameObject* f){
    auto tstate = PyThreadState_GET();
    if (tstate->c_tracefunc != nullptr) {
        PyObject *type, *value, *traceback, *orig_traceback, *arg;
        int result = 0;
        PyErr_Fetch(&type, &value, &orig_traceback);
        if (value == NULL) {
            value = Py_None;
            Py_INCREF(value);
        }
        PyErr_NormalizeException(&type, &value, &orig_traceback);
        traceback = (orig_traceback != NULL) ? orig_traceback : Py_None;
        arg = PyTuple_Pack(3, type, value, traceback);
        if (arg == NULL) {
            PyErr_Restore(type, value, orig_traceback);
            return;
        }

        if (tstate->tracing)
            return;
        result = trace(tstate, f, PyTrace_EXCEPTION, arg, tstate->c_tracefunc, tstate->c_traceobj);
        Py_DECREF(arg);
        if (result == 0) {
            PyErr_Restore(type, value, orig_traceback);
        }
        else {
            Py_XDECREF(type);
            Py_XDECREF(value);
            Py_XDECREF(orig_traceback);
        }
    }
}
